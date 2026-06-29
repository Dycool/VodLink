#include "AppPaths.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QtGlobal>

namespace {

QString cleanPath(QString path)
{
    path = QDir::cleanPath(path.trimmed());
    while (path.endsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('\\'))) {
        path.chop(1);
    }
    return path;
}

QString genericDataBase()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!base.trimmed().isEmpty()) {
        return cleanPath(base);
    }

#if defined(Q_OS_WIN)
    return cleanPath(QDir::home().filePath(QStringLiteral("AppData/Local")));
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return cleanPath(QDir::home().filePath(QStringLiteral("Library/Application Support")));
#else
    return cleanPath(QDir::home().filePath(QStringLiteral(".local/share")));
#endif
}

QString genericCacheBase()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (!base.trimmed().isEmpty()) {
        return cleanPath(base);
    }

#if defined(Q_OS_WIN)
    return cleanPath(QDir::home().filePath(QStringLiteral("AppData/Local")));
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return cleanPath(QDir::home().filePath(QStringLiteral("Library/Caches")));
#else
    return cleanPath(QDir::home().filePath(QStringLiteral(".cache")));
#endif
}

QString legacyDoubleDataRoot()
{
    return cleanPath(QDir(genericDataBase()).filePath(QStringLiteral("VodLink/VodLink")));
}

QString legacyDoubleCacheRoot()
{
    return cleanPath(QDir(genericCacheBase()).filePath(QStringLiteral("VodLink/VodLink")));
}

bool isVodLinkOwnedPath(const QString &path)
{
    const QString p = cleanPath(path);
    if (p.isEmpty()) {
        return false;
    }
    const QString slash = QDir::fromNativeSeparators(p);
    const QStringList parts = slash.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.contains(QStringLiteral("VodLink"));
}

void addUniqueVodLinkPath(QStringList *paths, const QString &path)
{
    const QString clean = cleanPath(path);
    if (clean.isEmpty() || !isVodLinkOwnedPath(clean)) {
        return;
    }
    if (!paths->contains(clean)) {
        paths->append(clean);
    }
}

#if defined(Q_OS_WIN)
QString psSingleQuoted(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'") + value + QStringLiteral("'");
}
#endif

} // namespace

namespace AppPaths {

QString dataRoot()
{
    return cleanPath(QDir(genericDataBase()).filePath(QStringLiteral("VodLink")));
}

QString cacheRoot()
{
    // Keep WebEngine/player/cache under the same resettable tree instead of
    // letting Qt build another organization/application path.
    return cleanPath(QDir(dataRoot()).filePath(QStringLiteral("cache")));
}

QStringList resetTargets()
{
    QStringList paths;
    addUniqueVodLinkPath(&paths, dataRoot());
    addUniqueVodLinkPath(&paths, cacheRoot());
    addUniqueVodLinkPath(&paths, legacyDoubleDataRoot());
    addUniqueVodLinkPath(&paths, legacyDoubleCacheRoot());

    // Include Qt's current app-specific paths too, but only if they are clearly
    // VodLink-owned. This catches older builds that used organization/app naming.
    addUniqueVodLinkPath(&paths, QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    addUniqueVodLinkPath(&paths, QStandardPaths::writableLocation(QStandardPaths::CacheLocation));

    // Delete deeper/nested paths first, then the parent. This avoids harmless
    // "already gone" races and makes manual logs easier to read.
    std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });
    return paths;
}

bool scheduleResetAfterExit(QString *error)
{
    const QStringList targets = resetTargets();
    if (targets.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not resolve VodLink local data paths.");
        }
        return false;
    }

    const qint64 pid = QCoreApplication::applicationPid();

#if defined(Q_OS_WIN)
    QStringList quotedTargets;
    quotedTargets.reserve(targets.size());
    for (const QString &target : targets) {
        quotedTargets.append(psSingleQuoted(QDir::toNativeSeparators(target)));
    }

    const QString command = QStringLiteral(
        "$ErrorActionPreference='SilentlyContinue'; "
        "$pidToWait=%1; "
        "while (Get-Process -Id $pidToWait -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 250 }; "
        "Start-Sleep -Milliseconds 500; "
        "$targets=@(%2); "
        "foreach ($p in $targets) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue } }")
        .arg(pid)
        .arg(quotedTargets.join(QStringLiteral(",")));

    QStringList args;
    args << QStringLiteral("-NoProfile")
         << QStringLiteral("-ExecutionPolicy") << QStringLiteral("Bypass")
         << QStringLiteral("-WindowStyle") << QStringLiteral("Hidden")
         << QStringLiteral("-Command") << command;

    if (QProcess::startDetached(QStringLiteral("powershell.exe"), args)) {
        return true;
    }

    if (error != nullptr) {
        *error = QStringLiteral("Could not start the VodLink reset helper. Close VodLink and delete %1 manually.")
                     .arg(QDir::toNativeSeparators(dataRoot()));
    }
    return false;
#else
    QStringList args;
    args << QStringLiteral("-c")
         << QStringLiteral("pid=\"$1\"; shift; while kill -0 \"$pid\" 2>/dev/null; do sleep 0.25; done; sleep 0.5; rm -rf -- \"$@\"")
         << QStringLiteral("vodlink-reset")
         << QString::number(pid);
    args.append(targets);

    if (QProcess::startDetached(QStringLiteral("/bin/sh"), args)) {
        return true;
    }

    if (error != nullptr) {
        *error = QStringLiteral("Could not start the VodLink reset helper. Close VodLink and delete %1 manually.")
                     .arg(dataRoot());
    }
    return false;
#endif
}

} // namespace AppPaths
