#include "configplugin.h"

#include "icalcalendarfile.h"

#include <QtQml>

void ConfigPlugin::registerTypes (const char *uri)
{
    Q_ASSERT(uri == QLatin1String("org.kde.akonadi2.config"));
    qmlRegisterType<ICalCalendarFile>(uri, 0, 1, "ICalFile");
}