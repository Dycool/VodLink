#include "ObsRuntime.h"

#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QProcessEnvironment>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QString appLocalDataRoot()
{
    return AppPaths::dataRoot();
}

QString appDirectoryRuntimeRoot()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    QDir dir(QCoreApplication::applicationDirPath());
    // VodLink.app/Contents/MacOS -> VodLink.app/Contents/Frameworks/obs-runtime
    if (dir.dirName() == QStringLiteral("MacOS") && dir.cdUp()) {
        const QString frameworksRuntime = dir.filePath(QStringLiteral("Frameworks/obs-runtime"));
        if (QDir(frameworksRuntime).exists()) {
            return frameworksRuntime;
        }
        // Backward-compatible fallback for older local dev bundles only.
        return dir.filePath(QStringLiteral("Resources/obs-runtime"));
    }
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("obs-runtime"));
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("obs-runtime"));
#endif
}

bool ensureDir(const QString &path, QString *error)
{
    QDir dir(path);
    if (dir.exists()) {
        return true;
    }
    if (dir.mkpath(QStringLiteral("."))) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("Could not create %1").arg(QDir::toNativeSeparators(path));
    }
    return false;
}

bool writeDataFile(const QByteArray &data, const QString &destinationPath, QString *error)
{
    QFileInfo destinationInfo(destinationPath);
    if (destinationInfo.exists() && destinationInfo.size() == data.size()) {
        QFile existing(destinationPath);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == data) {
            return true;
        }
    }

    if (!ensureDir(destinationInfo.dir().absolutePath(), error)) {
        return false;
    }

    QFile::remove(destinationPath);
    QSaveFile out(destinationPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not write private OBS runtime file %1")
                         .arg(QDir::toNativeSeparators(destinationPath));
        }
        return false;
    }
    if (out.write(data) != data.size() || !out.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not commit private OBS runtime file %1")
                         .arg(QDir::toNativeSeparators(destinationPath));
        }
        return false;
    }
    return true;
}

bool copyResourceFile(const QString &resourcePath, const QString &destinationPath, QString *error)
{
    QFile source(resourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not read embedded OBS runtime file %1").arg(resourcePath);
        }
        return false;
    }
    return writeDataFile(source.readAll(), destinationPath, error);
}

#if defined(Q_OS_WIN)
bool readRCDATAResource(int resourceId, QByteArray *out, QString *error)
{
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resource == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("Embedded OBS runtime resource %1 was not found.").arg(resourceId);
        }
        return false;
    }
    HGLOBAL handle = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void *bytes = LockResource(handle);
    if (handle == nullptr || bytes == nullptr || size == 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Embedded OBS runtime resource %1 is empty or unreadable.").arg(resourceId);
        }
        return false;
    }
    *out = QByteArray(static_cast<const char *>(bytes), static_cast<int>(size));
    return true;
}
#endif

QStringList existingDirectories(const QStringList &paths)
{
    QStringList result;
    for (const QString &path : paths) {
        if (!path.trimmed().isEmpty() && QDir(path).exists() && !result.contains(path)) {
            result.append(path);
        }
    }
    return result;
}

bool fileExistsInAnyDir(const QStringList &directories, const QString &fileName, QString *foundPath = nullptr)
{
    for (const QString &directory : directories) {
        const QString path = QDir(directory).filePath(fileName);
        if (QFileInfo::exists(path)) {
            if (foundPath != nullptr) {
                *foundPath = path;
            }
            return true;
        }
    }
    return false;
}

bool fileExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isFile();
}
} // namespace

bool ObsRuntime::prepare(QString *error)
{
#if defined(Q_OS_WIN)
    m_rootPath = QDir(appLocalDataRoot()).filePath(QStringLiteral("obs-runtime"));
    if (!extractEmbeddedRuntime(error)) {
        return false;
    }
#else
    m_rootPath = appDirectoryRuntimeRoot();
    if (!QDir(m_rootPath).exists()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "VodLink's private OBS runtime was not found at %1. Bundle obs-runtime with the app; do not rely on a system OBS install.")
                         .arg(QDir::toNativeSeparators(m_rootPath));
        }
        return false;
    }
#endif

    m_privateConfigRoot = QDir(appLocalDataRoot()).filePath(QStringLiteral("obs-private"));
    m_moduleConfigPath = QDir(m_privateConfigRoot).filePath(QStringLiteral("modules"));
    m_profilePath = QDir(m_privateConfigRoot).filePath(QStringLiteral("profile"));
    m_cachePath = QDir(m_privateConfigRoot).filePath(QStringLiteral("cache"));
    m_logPath = QDir(m_privateConfigRoot).filePath(QStringLiteral("logs"));
    if (!ensurePrivateConfig(error) || !installPrivateEnvironment(error)) {
        return false;
    }

#if defined(Q_OS_WIN)
    if (!addWindowsDllSearchPaths(error)) {
        return false;
    }
#endif

    return validateRuntimeLayout(error);
}

QStringList ObsRuntime::coreDataPaths() const
{
    return existingDirectories({
        // The graphics system calls obs_find_data_file("default.effect"), etc.
        // The path therefore must point directly at the directory containing the
        // core .effect files. Keep additional parent/fallback layouts around for
        // different official OBS packages, but never rely on an installed OBS.
        QDir(m_rootPath).filePath(QStringLiteral("data/libobs")),
        QDir(m_rootPath).filePath(QStringLiteral("data/obs-studio/libobs")),
        QDir(m_rootPath).filePath(QStringLiteral("Resources/data/libobs")),
        QDir(m_rootPath).filePath(QStringLiteral("Resources/data/obs-studio/libobs")),
        QDir(m_rootPath).filePath(QStringLiteral("usr/share/obs/libobs")),
        QDir(m_rootPath).filePath(QStringLiteral("usr/share/obs/obs-studio/libobs")),
        // Compatibility fallbacks for OBS builds whose compiled data prefix is
        // one level above libobs. Adding both is harmless; obs_find_data_file()
        // stops at the first matching file.
        QDir(m_rootPath).filePath(QStringLiteral("data")),
        QDir(m_rootPath).filePath(QStringLiteral("Resources/data")),
        QDir(m_rootPath).filePath(QStringLiteral("usr/share/obs")),
    });
}

QString ObsRuntime::coreDataPath() const
{
    const QStringList paths = coreDataPaths();
    if (!paths.isEmpty()) {
        return paths.first();
    }
    return QDir(m_rootPath).filePath(QStringLiteral("data/libobs"));
}

QStringList ObsRuntime::moduleBinPaths() const
{
    return existingDirectories({
        // Only OBS module/plugin directories belong here. Do not add bin/,
        // Frameworks/, or plain lib dirs: obs_load_all_modules would try to treat
        // dependency DLLs/dylibs/so files as OBS plugins, which is slow and can
        // crash at game start. Those dependency directories are handled by
        // AddDllDirectory/RPATH/LD_LIBRARY_PATH instead.
        QDir(m_rootPath).filePath(QStringLiteral("obs-plugins/64bit")),
        QDir(m_rootPath).filePath(QStringLiteral("obs-plugins")),
        QDir(m_rootPath).filePath(QStringLiteral("plugins/64bit")),
        QDir(m_rootPath).filePath(QStringLiteral("plugins")),
        QDir(m_rootPath).filePath(QStringLiteral("PlugIns")),
        QDir(m_rootPath).filePath(QStringLiteral("usr/lib/x86_64-linux-gnu/obs-plugins")),
        QDir(m_rootPath).filePath(QStringLiteral("usr/lib/aarch64-linux-gnu/obs-plugins")),
        QDir(m_rootPath).filePath(QStringLiteral("usr/lib/obs-plugins")),
    });
}

QStringList ObsRuntime::moduleDataPaths() const
{
    return existingDirectories({
        // OBS portable ZIP layout.
        QDir(m_rootPath).filePath(QStringLiteral("data/obs-plugins")),
        QDir(m_rootPath).filePath(QStringLiteral("share/obs/obs-plugins")),
        // OBS.app Contents layout.
        QDir(m_rootPath).filePath(QStringLiteral("Resources/data/obs-plugins")),
        QDir(m_rootPath).filePath(QStringLiteral("Resources/data/obs-studio/plugins")),
        // Extracted OBS .deb layout.
        QDir(m_rootPath).filePath(QStringLiteral("usr/share/obs/obs-plugins")),
        QDir(m_rootPath).filePath(QStringLiteral("usr/share/obs/obs-studio/plugins")),
    });
}

QList<QPair<QString, QString>> ObsRuntime::modulePathPairs() const
{
    QList<QPair<QString, QString>> pairs;
    const auto addPair = [&pairs](const QString &binPath, const QString &dataPath,
                                  const QString &binProbe, const QString &dataProbe) {
        if (QDir(binProbe).exists() && QDir(dataProbe).exists()) {
            const QPair<QString, QString> pair(binPath, dataPath);
            if (!pairs.contains(pair)) {
                pairs.append(pair);
            }
        }
    };

    // Match the shape used by OBS/Streamlabs: the binary path points at module
    // binaries, while the data path is a module template containing %module%.
    // Passing only the data root can make plugins start with the wrong resource
    // directory; passing the full template lets libobs resolve each module's
    // private data exactly the way OBS Studio does.
    const QDir root(m_rootPath);

    const QString obsPlugins64 = root.filePath(QStringLiteral("obs-plugins/64bit"));
    const QString obsPlugins = root.filePath(QStringLiteral("obs-plugins"));
    const QString portableData = root.filePath(QStringLiteral("data/obs-plugins"));
    addPair(obsPlugins64,
            QDir(portableData).filePath(QStringLiteral("%module%")),
            obsPlugins64,
            portableData);
    addPair(obsPlugins,
            QDir(portableData).filePath(QStringLiteral("%module%")),
            obsPlugins,
            portableData);

    const QString plugins64 = root.filePath(QStringLiteral("plugins/64bit"));
    const QString plugins = root.filePath(QStringLiteral("plugins"));
    const QString shareData = root.filePath(QStringLiteral("share/obs/obs-plugins"));
    addPair(plugins64,
            QDir(shareData).filePath(QStringLiteral("%module%")),
            plugins64,
            shareData);
    addPair(plugins,
            QDir(shareData).filePath(QStringLiteral("%module%")),
            plugins,
            shareData);

    const QString macPlugins = root.filePath(QStringLiteral("PlugIns"));
    addPair(root.filePath(QStringLiteral("PlugIns/%module%.plugin/Contents/MacOS")),
            root.filePath(QStringLiteral("PlugIns/%module%.plugin/Contents/Resources")),
            macPlugins,
            macPlugins);

    const QString linuxData = root.filePath(QStringLiteral("usr/share/obs/obs-plugins"));
    const QString linuxStudioData = root.filePath(QStringLiteral("usr/share/obs/obs-studio/plugins"));
    const QString linuxX64 = root.filePath(QStringLiteral("usr/lib/x86_64-linux-gnu/obs-plugins"));
    const QString linuxArm64 = root.filePath(QStringLiteral("usr/lib/aarch64-linux-gnu/obs-plugins"));
    const QString linuxGeneric = root.filePath(QStringLiteral("usr/lib/obs-plugins"));
    addPair(linuxX64,
            QDir(linuxData).filePath(QStringLiteral("%module%")),
            linuxX64,
            linuxData);
    addPair(linuxArm64,
            QDir(linuxData).filePath(QStringLiteral("%module%")),
            linuxArm64,
            linuxData);
    addPair(linuxGeneric,
            QDir(linuxData).filePath(QStringLiteral("%module%")),
            linuxGeneric,
            linuxData);
    addPair(linuxX64,
            QDir(linuxStudioData).filePath(QStringLiteral("%module%")),
            linuxX64,
            linuxStudioData);
    addPair(linuxArm64,
            QDir(linuxStudioData).filePath(QStringLiteral("%module%")),
            linuxArm64,
            linuxStudioData);
    addPair(linuxGeneric,
            QDir(linuxStudioData).filePath(QStringLiteral("%module%")),
            linuxGeneric,
            linuxStudioData);

    return pairs;
}

QByteArray ObsRuntime::moduleConfigPathUtf8() const
{
    return QFile::encodeName(m_moduleConfigPath);
}

QByteArray ObsRuntime::coreDataPathUtf8() const
{
    return QFile::encodeName(coreDataPath());
}

bool ObsRuntime::validateRuntimeLayout(QString *error) const
{
    if (m_rootPath.trimmed().isEmpty() || !QDir(m_rootPath).exists()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink's private OBS runtime root is missing: %1")
                         .arg(QDir::toNativeSeparators(m_rootPath));
        }
        return false;
    }

    const QStringList corePaths = coreDataPaths();
    if (corePaths.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink's private OBS runtime has no libobs core data directory under %1")
                         .arg(QDir::toNativeSeparators(m_rootPath));
        }
        return false;
    }

    const QStringList requiredCoreFiles = {
        QStringLiteral("default.effect"),
        QStringLiteral("default_rect.effect"),
        QStringLiteral("opaque.effect"),
        QStringLiteral("solid.effect"),
        QStringLiteral("repeat.effect"),
        QStringLiteral("format_conversion.effect"),
        QStringLiteral("bicubic_scale.effect"),
        QStringLiteral("lanczos_scale.effect"),
        QStringLiteral("area.effect"),
        QStringLiteral("bilinear_lowres_scale.effect"),
        QStringLiteral("premultiplied_alpha.effect"),
    };
    QStringList missingCore;
    for (const QString &file : requiredCoreFiles) {
        if (!fileExistsInAnyDir(corePaths, file)) {
            missingCore.append(file);
        }
    }
    if (!missingCore.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink's private OBS runtime is missing libobs core data files: %1. Checked: %2")
                         .arg(missingCore.join(QStringLiteral(", ")),
                              corePaths.join(QStringLiteral(", ")));
        }
        return false;
    }

    if (modulePathPairs().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink's private OBS runtime has no valid OBS module/data path pairs under %1")
                         .arg(QDir::toNativeSeparators(m_rootPath));
        }
        return false;
    }

#if defined(Q_OS_WIN)
    const QDir root(m_rootPath);
    const QStringList requiredWindowsFiles = {
        root.filePath(QStringLiteral("bin/64bit/obs.dll")),
        root.filePath(QStringLiteral("bin/64bit/libobs-d3d11.dll")),
        root.filePath(QStringLiteral("obs-plugins/64bit/obs-outputs.dll")),
        root.filePath(QStringLiteral("obs-plugins/64bit/rtmp-services.dll")),
        root.filePath(QStringLiteral("obs-plugins/64bit/obs-ffmpeg.dll")),
        root.filePath(QStringLiteral("obs-plugins/64bit/win-capture.dll")),
        root.filePath(QStringLiteral("obs-plugins/64bit/win-wasapi.dll")),
    };
    QStringList missing;
    for (const QString &path : requiredWindowsFiles) {
        if (!fileExists(path)) {
            missing.append(QDir::toNativeSeparators(path));
        }
    }
    if (!missing.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink's private OBS runtime is incomplete: %1")
                         .arg(missing.join(QStringLiteral(", ")));
        }
        return false;
    }
#endif

    return true;
}

bool ObsRuntime::extractEmbeddedRuntime(QString *error)
{
#if defined(Q_OS_WIN)
#if defined(VODLINK_OBS_RUNTIME_EMBEDDED)
    return extractNativeWindowsRuntime(error);
#else
    // Development fallback only. This is still app-owned and app-local; it never
    // searches Program Files or any OBS Studio installation path.
    const QString appRuntime = appDirectoryRuntimeRoot();
    if (QDir(appRuntime).exists()) {
        m_rootPath = appRuntime;
        return true;
    }

    if (error != nullptr) {
        *error = QStringLiteral(
            "VodLink was built without an embedded OBS runtime and no app-local obs-runtime folder exists. Rebuild with -DVODLINK_OBS_RUNTIME_DIR=<private OBS runtime>.");
    }
    return false;
#endif
#else
    Q_UNUSED(error);
    return true;
#endif
}

#if defined(Q_OS_WIN)
bool ObsRuntime::extractNativeWindowsRuntime(QString *error)
{
    QByteArray manifest;
    if (!readRCDATAResource(100, &manifest, error)) {
        return false;
    }

    // The embedded OBS runtime changes often while we iterate on the private
    // bundle.  Do not keep a stale AppData extraction around: an old set of OBS
    // graphics DLLs beside a new exe can make obs_reset_video() fail with the
    // wonderfully useless OBS_VIDEO_FAIL (-1).  The CMake manifest includes the
    // original file hashes/sizes, so it is a stable runtime stamp.
    const QString stampPath = QDir(m_rootPath).filePath(QStringLiteral(".vodlink-runtime-manifest"));
    bool runtimeMatchesStamp = false;
    QFile stamp(stampPath);
    if (stamp.open(QIODevice::ReadOnly)) {
        runtimeMatchesStamp = stamp.readAll() == manifest;
    }
    if (!runtimeMatchesStamp && QDir(m_rootPath).exists()) {
        QDir oldRuntime(m_rootPath);
        if (!oldRuntime.removeRecursively()) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not replace stale private OBS runtime at %1. Close VodLink and delete that folder manually.")
                             .arg(QDir::toNativeSeparators(m_rootPath));
            }
            return false;
        }
    }
    if (!ensureDir(m_rootPath, error)) {
        return false;
    }

    const QList<QByteArray> lines = manifest.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QList<QByteArray> parts = line.split('\t');
        if (parts.size() != 2 && parts.size() != 3 && parts.size() != 5) {
            if (error != nullptr) {
                *error = QStringLiteral("Invalid embedded OBS runtime manifest entry.");
            }
            return false;
        }

        bool ok = false;
        const int resourceId = parts.at(0).toInt(&ok);
        const QByteArray storageKind = parts.size() >= 3 ? parts.at(1) : QByteArray("raw");
        QString relativePath = QString::fromUtf8(parts.size() >= 3 ? parts.at(2) : parts.at(1));
        relativePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        relativePath = QDir::cleanPath(relativePath);
        if (!ok || resourceId <= 0 || QDir::isAbsolutePath(relativePath) ||
            relativePath == QStringLiteral("..") || relativePath.startsWith(QStringLiteral("../"))) {
            if (error != nullptr) {
                *error = QStringLiteral("Unsafe embedded OBS runtime manifest entry: %1").arg(relativePath);
            }
            return false;
        }

        QByteArray data;
        if (!readRCDATAResource(resourceId, &data, error)) {
            return false;
        }
        if (storageKind == QByteArray("z")) {
            const QByteArray inflated = qUncompress(reinterpret_cast<const uchar *>(data.constData()),
                                                    static_cast<qsizetype>(data.size()));
            if (inflated.isNull() && !data.isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("Could not decompress embedded OBS runtime file %1").arg(relativePath);
                }
                return false;
            }
            data = inflated;
        }

        if (parts.size() == 5) {
            bool sizeOk = false;
            const qint64 expectedSize = QString::fromLatin1(parts.at(4)).toLongLong(&sizeOk);
            const QByteArray expectedSha = parts.at(3).trimmed().toLower();
            const QByteArray actualSha = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
            if (!sizeOk || data.size() != expectedSize || actualSha != expectedSha) {
                if (error != nullptr) {
                    *error = QStringLiteral("Embedded OBS runtime file %1 failed verification while extracting.")
                                 .arg(relativePath);
                }
                return false;
            }
        }

        if (!writeDataFile(data, QDir(m_rootPath).filePath(relativePath), error)) {
            return false;
        }
    }

    if (!writeDataFile(manifest, stampPath, error)) {
        return false;
    }
    return true;
}

#endif

bool ObsRuntime::extractResourceDirectory(const QString &resourcePath, const QString &destinationPath,
                                          QString *error)
{
    if (!ensureDir(destinationPath, error)) {
        return false;
    }

    const QDir sourceDir(resourcePath);
    const QFileInfoList entries = sourceDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QFileInfo &entry : entries) {
        const QString sourceChild = resourcePath + QLatin1Char('/') + entry.fileName();
        const QString destinationChild = QDir(destinationPath).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!extractResourceDirectory(sourceChild, destinationChild, error)) {
                return false;
            }
        } else if (!copyResourceFile(sourceChild, destinationChild, error)) {
            return false;
        }
    }
    return true;
}

bool ObsRuntime::ensurePrivateConfig(QString *error)
{
    const QStringList required = {
        m_privateConfigRoot,
        m_moduleConfigPath,
        m_profilePath,
        m_cachePath,
        m_logPath,
        QDir(m_profilePath).filePath(QStringLiteral("basic")),
        QDir(m_profilePath).filePath(QStringLiteral("plugin_config")),
    };
    for (const QString &path : required) {
        if (!ensureDir(path, error)) {
            return false;
        }
    }
    return true;
}

bool ObsRuntime::installPrivateEnvironment(QString *error) const
{
    if (m_privateConfigRoot.trimmed().isEmpty() || m_moduleConfigPath.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("VodLink's private OBS config paths were not initialized.");
        }
        return false;
    }

    // Some OBS plugins and dependency libraries consult environment variables
    // in addition to libobs' module_config_path. Scope them to VodLink-owned
    // folders before obs_startup/obs_load_all_modules so installed OBS Studio
    // profiles, plugin caches, and logs are never touched.
    qputenv("OBS_DISABLE_PLUGIN_CONFIG_IMPORT", "1");
    qputenv("OBS_USER_CONFIG_DIR", QFile::encodeName(m_privateConfigRoot));
    qputenv("OBS_CONFIG_DIR", QFile::encodeName(m_privateConfigRoot));
    qputenv("OBS_PROFILE_DIR", QFile::encodeName(m_profilePath));
    qputenv("OBS_CACHE_DIR", QFile::encodeName(m_cachePath));
    qputenv("OBS_LOG_DIR", QFile::encodeName(m_logPath));
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && !defined(Q_OS_MAC)
    // Linux plugin dependencies often use XDG_CONFIG_HOME/XDG_CACHE_HOME.
    // Point those at a private subtree while the embedded OBS runtime is active.
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(QDir(m_privateConfigRoot).filePath(QStringLiteral("xdg-config"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(QDir(m_privateConfigRoot).filePath(QStringLiteral("xdg-cache"))));
#endif
    return true;
}

bool ObsRuntime::addWindowsDllSearchPaths(QString *error) const
{
#if defined(Q_OS_WIN)
    QStringList paths = existingDirectories({
        QDir(m_rootPath).filePath(QStringLiteral("bin/64bit")),
        QDir(m_rootPath).filePath(QStringLiteral("bin")),
        m_rootPath,
    });
    paths.append(moduleBinPaths());
    paths.removeDuplicates();

    if (paths.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("The private OBS runtime has no DLL directories under %1")
                         .arg(QDir::toNativeSeparators(m_rootPath));
        }
        return false;
    }

    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    for (const QString &path : paths) {
        AddDllDirectory(reinterpret_cast<PCWSTR>(path.utf16()));
    }
    // Helps older transitive DLL loads from OBS plugins without adding any system
    // OBS folder to the process path.
    SetDllDirectoryW(reinterpret_cast<LPCWSTR>(paths.first().utf16()));
    return true;
#else
    Q_UNUSED(error);
    return true;
#endif
}
