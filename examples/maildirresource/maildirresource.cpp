/*
 *   Copyright (C) 2015 Christian Mollekopf <chrigi_1@fastmail.fm>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 */

#include "maildirresource.h"
#include "facade.h"
#include "entitybuffer.h"
#include "pipeline.h"
#include "mail_generated.h"
#include "createentity_generated.h"
#include "modifyentity_generated.h"
#include "deleteentity_generated.h"
#include "domainadaptor.h"
#include "resourceconfig.h"
#include "commands.h"
#include "index.h"
#include "log.h"
#include "domain/mail.h"
#include "definitions.h"
#include "facadefactory.h"
#include "indexupdater.h"
#include "libmaildir/maildir.h"
#include <QDate>
#include <QUuid>
#include <QDir>
#include <QDirIterator>
#include <KMime/KMime/KMimeMessage>

//This is the resources entity type, and not the domain type
#define ENTITY_TYPE_MAIL "mail"
#define ENTITY_TYPE_FOLDER "folder"

MaildirResource::MaildirResource(const QByteArray &instanceIdentifier, const QSharedPointer<Akonadi2::Pipeline> &pipeline)
    : Akonadi2::GenericResource(instanceIdentifier, pipeline),
    mMailAdaptorFactory(QSharedPointer<MaildirMailAdaptorFactory>::create()),
    mFolderAdaptorFactory(QSharedPointer<MaildirFolderAdaptorFactory>::create())
{
    addType(ENTITY_TYPE_MAIL, mMailAdaptorFactory,
            QVector<Akonadi2::Preprocessor*>() << new DefaultIndexUpdater<Akonadi2::ApplicationDomain::Mail>);
    addType(ENTITY_TYPE_FOLDER, mFolderAdaptorFactory,
            QVector<Akonadi2::Preprocessor*>() << new DefaultIndexUpdater<Akonadi2::ApplicationDomain::Folder>);
    auto config = ResourceConfig::getConfiguration(instanceIdentifier);
    mMaildirPath = QDir::cleanPath(QDir::fromNativeSeparators(config.value("path").toString()));
    //Chop a trailing slash if necessary
    if (mMaildirPath.endsWith("/")) {
        mMaildirPath.chop(1);
    }
    Trace() << "Started maildir resource for maildir: " << mMaildirPath;
}

void MaildirResource::recordRemoteId(const QByteArray &bufferType, const QByteArray &localId, const QByteArray &remoteId, Akonadi2::Storage::Transaction &transaction)
{
    Index index("rid.mapping." + bufferType, transaction);
    Index localIndex("localid.mapping." + bufferType, transaction);
    index.add(remoteId, localId);
    localIndex.add(localId, remoteId);
}

void MaildirResource::removeRemoteId(const QByteArray &bufferType, const QByteArray &localId, const QByteArray &remoteId, Akonadi2::Storage::Transaction &transaction)
{
    Index index("rid.mapping." + bufferType, transaction);
    Index localIndex("localid.mapping." + bufferType, transaction);
    index.remove(remoteId, localId);
    localIndex.remove(localId, remoteId);
}

QString MaildirResource::resolveRemoteId(const QByteArray &bufferType, const QString &remoteId, Akonadi2::Storage::Transaction &transaction)
{
    //Lookup local id for remote id, or insert a new pair otherwise
    Index index("rid.mapping." + bufferType, transaction);
    Index localIndex("localid.mapping." + bufferType, transaction);
    QByteArray akonadiId = index.lookup(remoteId.toUtf8());
    if (akonadiId.isEmpty()) {
        akonadiId = QUuid::createUuid().toString().toUtf8();
        index.add(remoteId.toUtf8(), akonadiId);
        localIndex.add(akonadiId, remoteId.toUtf8());
    }
    return akonadiId;
}

QString MaildirResource::resolveLocalId(const QByteArray &bufferType, const QByteArray &localId, Akonadi2::Storage::Transaction &transaction)
{
    Index index("localid.mapping." + bufferType, transaction);
    QByteArray remoteId = index.lookup(localId);
    if (remoteId.isEmpty()) {
        Warning() << "Couldn't find the remote id for " << localId;
    }
    return remoteId;
}

static QStringList listRecursive( const QString &root, const KPIM::Maildir &dir )
{
    QStringList list;
    foreach (const QString &sub, dir.subFolderList()) {
        const KPIM::Maildir md = dir.subFolder(sub);
        if (!md.isValid()) {
            continue;
        }
        QString path = root + "/" + sub;
        list << path;
        list += listRecursive(path, md );
    }
    return list;
}

QStringList MaildirResource::listAvailableFolders()
{
    KPIM::Maildir dir(mMaildirPath, true);
    if (!dir.isValid()) {
        return QStringList();
    }
    QStringList folderList;
    folderList << mMaildirPath;
    folderList += listRecursive(mMaildirPath, dir);
    return folderList;
}

static void createEntity(const QByteArray &akonadiId, const QByteArray &bufferType, const Akonadi2::ApplicationDomain::ApplicationDomainType &domainObject, DomainTypeAdaptorFactoryInterface &adaptorFactory, std::function<void(const QByteArray &)> callback)
{
    //These changes are coming from the source
    const auto replayToSource = false;
    flatbuffers::FlatBufferBuilder entityFbb;
    adaptorFactory.createBuffer(domainObject, entityFbb);
    flatbuffers::FlatBufferBuilder fbb;
    //This is the resource type and not the domain type
    auto entityId = fbb.CreateString(akonadiId.toStdString());
    auto type = fbb.CreateString(bufferType.toStdString());
    auto delta = Akonadi2::EntityBuffer::appendAsVector(fbb, entityFbb.GetBufferPointer(), entityFbb.GetSize());
    auto location = Akonadi2::Commands::CreateCreateEntity(fbb, entityId, type, delta, replayToSource);
    Akonadi2::Commands::FinishCreateEntityBuffer(fbb, location);
    callback(QByteArray::fromRawData(reinterpret_cast<char const *>(fbb.GetBufferPointer()), fbb.GetSize()));
}

static void modifyEntity(const QByteArray &akonadiId, qint64 revision, const QByteArray &bufferType, const Akonadi2::ApplicationDomain::ApplicationDomainType &domainObject, DomainTypeAdaptorFactoryInterface &adaptorFactory, std::function<void(const QByteArray &)> callback)
{
    //These changes are coming from the source
    const auto replayToSource = false;
    flatbuffers::FlatBufferBuilder entityFbb;
    adaptorFactory.createBuffer(domainObject, entityFbb);
    flatbuffers::FlatBufferBuilder fbb;
    auto entityId = fbb.CreateString(akonadiId.toStdString());
    //This is the resource type and not the domain type
    auto type = fbb.CreateString(bufferType.toStdString());
    auto delta = Akonadi2::EntityBuffer::appendAsVector(fbb, entityFbb.GetBufferPointer(), entityFbb.GetSize());
    //TODO removals
    auto location = Akonadi2::Commands::CreateModifyEntity(fbb, revision, entityId, 0, type, delta, replayToSource);
    Akonadi2::Commands::FinishModifyEntityBuffer(fbb, location);
    callback(QByteArray::fromRawData(reinterpret_cast<char const *>(fbb.GetBufferPointer()), fbb.GetSize()));
}

static void deleteEntity(const QByteArray &akonadiId, qint64 revision, const QByteArray &bufferType, std::function<void(const QByteArray &)> callback)
{
    //These changes are coming from the source
    const auto replayToSource = false;
    flatbuffers::FlatBufferBuilder fbb;
    auto entityId = fbb.CreateString(akonadiId.toStdString());
    //This is the resource type and not the domain type
    auto type = fbb.CreateString(bufferType.toStdString());
    auto location = Akonadi2::Commands::CreateDeleteEntity(fbb, revision, entityId, type, replayToSource);
    Akonadi2::Commands::FinishDeleteEntityBuffer(fbb, location);
    callback(QByteArray::fromRawData(reinterpret_cast<char const *>(fbb.GetBufferPointer()), fbb.GetSize()));
}

static QSharedPointer<Akonadi2::ApplicationDomain::BufferAdaptor> getLatest(const Akonadi2::Storage::NamedDatabase &db, const QByteArray &uid, DomainTypeAdaptorFactoryInterface &adaptorFactory)
{
    QSharedPointer<Akonadi2::ApplicationDomain::BufferAdaptor> current;
    db.findLatest(uid, [&current, &adaptorFactory](const QByteArray &key, const QByteArray &data) -> bool {
        Akonadi2::EntityBuffer buffer(const_cast<const char *>(data.data()), data.size());
        if (!buffer.isValid()) {
            Warning() << "Read invalid buffer from disk";
        } else {
            current = adaptorFactory.createAdaptor(buffer.entity());
        }
        return false;
    },
    [](const Akonadi2::Storage::Error &error) {
        Warning() << "Failed to read current value from storage: " << error.message;
    });
    return current;
}

void MaildirResource::scanForRemovals(Akonadi2::Storage::Transaction &transaction, Akonadi2::Storage::Transaction &synchronizationTransaction, const QByteArray &bufferType, std::function<bool(const QByteArray &remoteId)> exists)
{
    auto mainDatabase = transaction.openDatabase(bufferType + ".main");
    //TODO Instead of iterating over all entries in the database, which can also pick up the same item multiple times,
    //we should rather iterate over an index that contains every uid exactly once. The remoteId index would be such an index,
    //but we currently fail to iterate over all entries in an index it seems.
    // auto remoteIds = synchronizationTransaction.openDatabase("rid.mapping." + bufferType, std::function<void(const Akonadi2::Storage::Error &)>(), true);
    mainDatabase.scan("", [this, &transaction, bufferType, &synchronizationTransaction, &exists](const QByteArray &key, const QByteArray &) {
        auto akonadiId = Akonadi2::Storage::uidFromKey(key);
        Trace() << "Checking for removal " << key;
        const auto remoteId = resolveLocalId(bufferType, akonadiId, synchronizationTransaction);
        if (!remoteId.isEmpty()) {
            if (!exists(remoteId.toLatin1())) {
                Trace() << "Found a removed entity: " << akonadiId;
                deleteEntity(akonadiId, Akonadi2::Storage::maxRevision(transaction), bufferType, [this](const QByteArray &buffer) {
                    enqueueCommand(mSynchronizerQueue, Akonadi2::Commands::DeleteEntityCommand, buffer);
                });
            }
        }
        return true;
    },
    [](const Akonadi2::Storage::Error &error) {
    });

}

void MaildirResource::createOrModify(Akonadi2::Storage::Transaction &transaction, Akonadi2::Storage::Transaction &synchronizationTransaction, DomainTypeAdaptorFactoryInterface &adaptorFactory, const QByteArray &bufferType, const QByteArray &remoteId, const Akonadi2::ApplicationDomain::ApplicationDomainType &entity)
{
    auto mainDatabase = transaction.openDatabase(bufferType + ".main");
    const auto akonadiId = resolveRemoteId(bufferType, remoteId, synchronizationTransaction).toLatin1();
    const auto found = mainDatabase.contains(akonadiId);
    if (!found) {
        Trace() << "Found a new entity: " << remoteId;
        createEntity(akonadiId, bufferType, entity, adaptorFactory, [this](const QByteArray &buffer) {
            enqueueCommand(mSynchronizerQueue, Akonadi2::Commands::CreateEntityCommand, buffer);
        });
    } else { //modification
        if (auto current = getLatest(mainDatabase, akonadiId, adaptorFactory)) {
            bool changed = false;
            for (const auto &property : entity.changedProperties()) {
                if (entity.getProperty(property) != current->getProperty(property)) {
                    Trace() << "Property changed " << akonadiId << property;
                    changed = true;
                }
            }
            if (changed) {
                Trace() << "Found a modified entity: " << remoteId;
                modifyEntity(akonadiId, Akonadi2::Storage::maxRevision(transaction), bufferType, entity, adaptorFactory, [this](const QByteArray &buffer) {
                    enqueueCommand(mSynchronizerQueue, Akonadi2::Commands::ModifyEntityCommand, buffer);
                });
            }
        } else {
            Warning() << "Failed to get current entity";
        }
    }
}

void MaildirResource::synchronizeFolders(Akonadi2::Storage::Transaction &transaction)
{
    const QByteArray bufferType = ENTITY_TYPE_FOLDER;
    QStringList folderList = listAvailableFolders();
    Trace() << "Found folders " << folderList;

    Akonadi2::Storage store(Akonadi2::storageLocation(), mResourceInstanceIdentifier + ".synchronization", Akonadi2::Storage::ReadWrite);
    auto synchronizationTransaction = store.createTransaction(Akonadi2::Storage::ReadWrite);
    auto mainDatabase = transaction.openDatabase(bufferType + ".main");
    scanForRemovals(transaction, synchronizationTransaction, bufferType, [&folderList](const QByteArray &remoteId) -> bool {
        return folderList.contains(remoteId);
    });

    for (const auto folderPath : folderList) {
        const auto remoteId = folderPath.toUtf8();
        Trace() << "Processing folder " << remoteId;
        KPIM::Maildir md(folderPath, folderPath == mMaildirPath);

        Akonadi2::ApplicationDomain::Folder folder;
        folder.setProperty("name", md.name());
        folder.setProperty("icon", "folder");
        if (!md.isRoot()) {
            folder.setProperty("parent", resolveRemoteId(ENTITY_TYPE_FOLDER, md.parent().path(), synchronizationTransaction).toLatin1());
        }
        createOrModify(transaction, synchronizationTransaction, *mFolderAdaptorFactory, bufferType, remoteId, folder);
    }
}

void MaildirResource::synchronizeMails(Akonadi2::Storage::Transaction &transaction, const QString &path)
{
    Trace() << "Synchronizing mails" << path;
    const QByteArray bufferType = ENTITY_TYPE_MAIL;

    KPIM::Maildir maildir(path, true);
    if (!maildir.isValid()) {
        Warning() << "Failed to sync folder " << maildir.lastError();
        return;
    }

    auto listingPath = maildir.pathToCurrent();
    auto entryIterator = QSharedPointer<QDirIterator>::create(listingPath, QDir::Files);
    Trace() << "Looking into " << listingPath;

    QFileInfo entryInfo;

    Akonadi2::Storage store(Akonadi2::storageLocation(), mResourceInstanceIdentifier + ".synchronization", Akonadi2::Storage::ReadWrite);
    auto synchronizationTransaction = store.createTransaction(Akonadi2::Storage::ReadWrite);

    const auto folderLocalId = resolveRemoteId(ENTITY_TYPE_FOLDER, path, synchronizationTransaction);

    auto exists = [&listingPath](const QByteArray &remoteId) -> bool {
        return QFile(listingPath + "/" + remoteId).exists();
    };

    auto property = "folder";
    Index index(bufferType + ".index." + property, transaction);
    index.lookup(folderLocalId.toLatin1(), [&](const QByteArray &akonadiId) {
        const auto remoteId = resolveLocalId(bufferType, akonadiId, synchronizationTransaction);
        if (!remoteId.isEmpty()) {
            if (!exists(remoteId.toLatin1())) {
                Trace() << "Found a removed entity: " << akonadiId;
                deleteEntity(akonadiId, Akonadi2::Storage::maxRevision(transaction), bufferType, [this](const QByteArray &buffer) {
                    enqueueCommand(mSynchronizerQueue, Akonadi2::Commands::DeleteEntityCommand, buffer);
                });
            }
        }
    },
    [property](const Index::Error &error) {
        Warning() << "Error in index: " <<  error.message << property;
    });

    while (entryIterator->hasNext()) {
        QString filePath = entryIterator->next();
        QString fileName = entryIterator->fileName();
        const auto remoteId = fileName.toUtf8();

        KMime::Message *msg = new KMime::Message;
        auto filepath = listingPath + "/" + fileName;
        msg->setHead(KMime::CRLFtoLF(maildir.readEntryHeadersFromFile(filepath)));
        msg->parse();

        const auto flags = maildir.readEntryFlags(fileName);

        Trace() << "Found a mail " << filePath << fileName << msg->subject(true)->asUnicodeString();

        Akonadi2::ApplicationDomain::Mail mail;
        mail.setProperty("subject", msg->subject(true)->asUnicodeString());
        mail.setProperty("sender", msg->from(true)->asUnicodeString());
        mail.setProperty("senderName", msg->from(true)->asUnicodeString());
        mail.setProperty("date", msg->date(true)->dateTime());
        mail.setProperty("folder", folderLocalId);
        mail.setProperty("mimeMessage", filepath);
        mail.setProperty("unread", !flags.testFlag(KPIM::Maildir::Seen));
        mail.setProperty("important", flags.testFlag(KPIM::Maildir::Flagged));

        createOrModify(transaction, synchronizationTransaction, *mMailAdaptorFactory, bufferType, remoteId, mail);
    }
}

KAsync::Job<void> MaildirResource::synchronizeWithSource()
{
    Log() << " Synchronizing";
    return KAsync::start<void>([this]() {
        //Changereplay would deadlock otherwise when trying to open the synchronization store
        enableChangeReplay(false);
        auto transaction = Akonadi2::Storage(Akonadi2::storageLocation(), mResourceInstanceIdentifier, Akonadi2::Storage::ReadOnly).createTransaction(Akonadi2::Storage::ReadOnly);
        synchronizeFolders(transaction);
        for (const auto &folder : listAvailableFolders()) {
            synchronizeMails(transaction, folder);
        }
        Log() << "Done Synchronizing";
        enableChangeReplay(true);
    });
}

KAsync::Job<void> MaildirResource::replay(const QByteArray &type, const QByteArray &key, const QByteArray &value)
{
    //This results in a deadlock during sync
    Akonadi2::Storage store(Akonadi2::storageLocation(), mResourceInstanceIdentifier + ".synchronization", Akonadi2::Storage::ReadWrite);
    auto synchronizationTransaction = store.createTransaction(Akonadi2::Storage::ReadWrite);

    Trace() << "Replaying " << key << type;
    if (type == ENTITY_TYPE_FOLDER) {
        Akonadi2::EntityBuffer buffer(value.data(), value.size());
        const Akonadi2::Entity &entity = buffer.entity();
        const auto metadataBuffer = Akonadi2::EntityBuffer::readBuffer<Akonadi2::Metadata>(entity.metadata());
        if (metadataBuffer && !metadataBuffer->replayToSource()) {
            Trace() << "Change is coming from the source";
            return KAsync::null<void>();
        }
        const qint64 revision = metadataBuffer ? metadataBuffer->revision() : -1;
        const auto operation = metadataBuffer ? metadataBuffer->operation() : Akonadi2::Operation_Creation;
        if (operation == Akonadi2::Operation_Creation) {
            const Akonadi2::ApplicationDomain::Folder folder(mResourceInstanceIdentifier, Akonadi2::Storage::uidFromKey(key), revision, mFolderAdaptorFactory->createAdaptor(entity));
            auto folderName = folder.getProperty("name").toString();
            //TODO handle non toplevel folders
            auto path = mMaildirPath + "/" + folderName;
            Trace() << "Creating a new folder: " << path;
            KPIM::Maildir maildir(path, false);
            maildir.create();
            recordRemoteId(ENTITY_TYPE_FOLDER, folder.identifier(), path.toUtf8(), synchronizationTransaction);
        } else if (operation == Akonadi2::Operation_Removal) {
            const auto uid = Akonadi2::Storage::uidFromKey(key);
            const auto remoteId = resolveLocalId(ENTITY_TYPE_FOLDER, uid, synchronizationTransaction);
            const auto path = remoteId;
            Trace() << "Removing a folder: " << path;
            KPIM::Maildir maildir(path, false);
            maildir.remove();
            removeRemoteId(ENTITY_TYPE_FOLDER, uid, remoteId.toUtf8(), synchronizationTransaction);
        } else if (operation == Akonadi2::Operation_Modification) {
            Warning() << "Folder modifications are not implemented";
        } else {
            Warning() << "Unkown operation" << operation;
        }
    } else if (type == ENTITY_TYPE_MAIL) {
        Akonadi2::EntityBuffer buffer(value.data(), value.size());
        const Akonadi2::Entity &entity = buffer.entity();
        const auto metadataBuffer = Akonadi2::EntityBuffer::readBuffer<Akonadi2::Metadata>(entity.metadata());
        if (metadataBuffer && !metadataBuffer->replayToSource()) {
            Trace() << "Change is coming from the source";
            return KAsync::null<void>();
        }
        const qint64 revision = metadataBuffer ? metadataBuffer->revision() : -1;
        const auto operation = metadataBuffer ? metadataBuffer->operation() : Akonadi2::Operation_Creation;
        if (operation == Akonadi2::Operation_Creation) {
            const Akonadi2::ApplicationDomain::Mail mail(mResourceInstanceIdentifier, Akonadi2::Storage::uidFromKey(key), revision, mMailAdaptorFactory->createAdaptor(entity));
            auto parentFolder = mail.getProperty("folder").toByteArray();
            QByteArray parentFolderRemoteId;
            if (!parentFolder.isEmpty()) {
                parentFolderRemoteId = resolveLocalId(ENTITY_TYPE_FOLDER, parentFolder, synchronizationTransaction).toLatin1();
            } else {
                parentFolderRemoteId = mMaildirPath.toLatin1();
            }
            const auto parentFolderPath = parentFolderRemoteId;
            KPIM::Maildir maildir(parentFolderPath, false);
            //FIXME assemble the MIME message
            const auto id = maildir.addEntry("foobar");
            Trace() << "Creating a new mail: " << id;
            recordRemoteId(ENTITY_TYPE_MAIL, mail.identifier(), id.toUtf8(), synchronizationTransaction);
        } else if (operation == Akonadi2::Operation_Removal) {
            const auto uid = Akonadi2::Storage::uidFromKey(key);
            const auto remoteId = resolveLocalId(ENTITY_TYPE_MAIL, uid, synchronizationTransaction);
            const Akonadi2::ApplicationDomain::Mail mail(mResourceInstanceIdentifier, Akonadi2::Storage::uidFromKey(key), revision, mMailAdaptorFactory->createAdaptor(entity));
            auto parentFolder = mail.getProperty("folder").toByteArray();
            const auto parentFolderRemoteId = resolveLocalId(ENTITY_TYPE_FOLDER, parentFolder, synchronizationTransaction);
            const auto parentFolderPath = parentFolderRemoteId;
            KPIM::Maildir maildir(parentFolderPath, false);
            Trace() << "Removing a mail: " << remoteId;
            maildir.removeEntry(remoteId);
            removeRemoteId(ENTITY_TYPE_MAIL, uid, remoteId.toUtf8(), synchronizationTransaction);
        } else if (operation == Akonadi2::Operation_Modification) {
            Warning() << "Mail modifications are not implemented";
        } else {
            Warning() << "Unkown operation" << operation;
        }
    }
    return KAsync::null<void>();
}

void MaildirResource::removeFromDisk(const QByteArray &instanceIdentifier)
{
    GenericResource::removeFromDisk(instanceIdentifier);
    Akonadi2::Storage(Akonadi2::storageLocation(), instanceIdentifier + ".synchronization", Akonadi2::Storage::ReadWrite).removeFromDisk();
}

MaildirResourceFactory::MaildirResourceFactory(QObject *parent)
    : Akonadi2::ResourceFactory(parent)
{

}

Akonadi2::Resource *MaildirResourceFactory::createResource(const QByteArray &instanceIdentifier)
{
    return new MaildirResource(instanceIdentifier);
}

void MaildirResourceFactory::registerFacades(Akonadi2::FacadeFactory &factory)
{
    factory.registerFacade<Akonadi2::ApplicationDomain::Mail, MaildirResourceMailFacade>(PLUGIN_NAME);
    factory.registerFacade<Akonadi2::ApplicationDomain::Folder, MaildirResourceFolderFacade>(PLUGIN_NAME);
}

