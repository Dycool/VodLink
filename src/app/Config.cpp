#include "Config.h"

// These are defined by CMake (target_compile_definitions). The fallbacks keep
// the file compilable in editors/tools that don't pass the definitions; real
// builds always provide them.
#ifndef VODLINK_GOOGLE_CLIENT_ID
#  define VODLINK_GOOGLE_CLIENT_ID ""
#endif
#ifndef VODLINK_GOOGLE_CLIENT_SECRET
#  define VODLINK_GOOGLE_CLIENT_SECRET ""
#endif
#ifndef VODLINK_WORKER_URL
#  define VODLINK_WORKER_URL ""
#endif

QString Config::googleClientId() const
{
    return QString::fromLatin1(VODLINK_GOOGLE_CLIENT_ID);
}

QString Config::googleClientSecret() const
{
    return QString::fromLatin1(VODLINK_GOOGLE_CLIENT_SECRET);
}

QString Config::workerUrl() const
{
    QString url = QString::fromLatin1(VODLINK_WORKER_URL);
    while (url.endsWith(u'/')) {
        url.chop(1);
    }
    return url;
}
