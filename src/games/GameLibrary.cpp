#include "GameLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#  include <QSettings>
#endif

void GameLibrary::refresh()
{
    m_games.clear();
    scanSteam();
    scanEpic();
}

QString GameLibrary::normalize(const QString &path)
{
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.contains(QStringLiteral("//"))) {
        p.replace(QStringLiteral("//"), QStringLiteral("/"));
    }
    p = p.toLower();
    while (p.endsWith(QLatin1Char('/'))) {
        p.chop(1);
    }
    return p;
}

void GameLibrary::addGame(const QString &name, const QString &installDir, const QString &source,
                          const QString &appId)
{
    const QString dir = normalize(installDir);
    if (name.isEmpty() || dir.isEmpty()) {
        return;
    }
    for (const InstalledGame &game : m_games) {
        if (game.installDir == dir) {
            return; // already known
        }
    }
    m_games.push_back({name, dir, source, appId});
}

QString GameLibrary::matchByPath(const QString &fullExecutablePath) const
{
    if (fullExecutablePath.isEmpty()) {
        return {};
    }
    const QString path = normalize(fullExecutablePath);
    for (const InstalledGame &game : m_games) {
        if (path == game.installDir || path.startsWith(game.installDir + QLatin1Char('/'))) {
            return game.name;
        }
    }
    return {};
}

void GameLibrary::scanSteam()
{
    QStringList steamRoots;
#ifdef Q_OS_WIN
    QSettings registry(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                       QSettings::NativeFormat);
    steamRoots << registry.value(QStringLiteral("SteamPath")).toString();
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    steamRoots << QDir::homePath() + QStringLiteral("/Library/Application Support/Steam");
#else
    // Cover the common native and Flatpak Steam layouts. Some of these are
    // symlinks on certain distros; duplicates are filtered below.
    steamRoots << QDir::homePath() + QStringLiteral("/.steam/root")
               << QDir::homePath() + QStringLiteral("/.steam/steam")
               << QDir::homePath() + QStringLiteral("/.local/share/Steam")
               << QDir::homePath() + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam");
#endif

    // Collect every Steam library root from libraryfolders.vdf (plus each base).
    QStringList libraries;
    QStringList seenSteamRoots;
    for (const QString &rawSteamPath : steamRoots) {
        if (rawSteamPath.isEmpty() || !QDir(rawSteamPath).exists()) {
            continue;
        }
        const QString steamPath = QFileInfo(rawSteamPath).canonicalFilePath().isEmpty()
                                      ? rawSteamPath
                                      : QFileInfo(rawSteamPath).canonicalFilePath();
        const QString normalizedSteamPath = normalize(steamPath);
        if (seenSteamRoots.contains(normalizedSteamPath)) {
            continue;
        }
        seenSteamRoots << normalizedSteamPath;
        libraries << steamPath;

        QFile vdf(steamPath + QStringLiteral("/steamapps/libraryfolders.vdf"));
        if (vdf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(vdf.readAll());
            static const QRegularExpression pathRe(QStringLiteral("\"path\"\\s*\"([^\"]+)\""));
            QRegularExpressionMatchIterator it = pathRe.globalMatch(content);
            while (it.hasNext()) {
                QString path = it.next().captured(1);
                path.replace(QStringLiteral("\\\""), QStringLiteral("\""));
                path.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
                libraries << path;
            }
        }
    }
    if (libraries.isEmpty()) {
        return;
    }

    static const QRegularExpression nameRe(QStringLiteral("\"name\"\\s*\"([^\"]+)\""));
    static const QRegularExpression dirRe(QStringLiteral("\"installdir\"\\s*\"([^\"]+)\""));
    static const QRegularExpression appRe(QStringLiteral("\"appid\"\\s*\"([^\"]+)\""));

    QStringList seenLibraries;
    for (const QString &library : libraries) {
        const QString normalizedLib = normalize(library);
        if (seenLibraries.contains(normalizedLib)) {
            continue;
        }
        seenLibraries << normalizedLib;

        QDir steamapps(library + QStringLiteral("/steamapps"));
        const QStringList manifests = steamapps.entryList(
            {QStringLiteral("appmanifest_*.acf")}, QDir::Files);
        for (const QString &manifest : manifests) {
            QFile acf(steamapps.filePath(manifest));
            if (!acf.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }
            const QString text = QString::fromUtf8(acf.readAll());
            const QRegularExpressionMatch nameMatch = nameRe.match(text);
            const QRegularExpressionMatch dirMatch = dirRe.match(text);
            if (!nameMatch.hasMatch() || !dirMatch.hasMatch()) {
                continue;
            }
            const QString installDir = steamapps.filePath(
                QStringLiteral("common/") + dirMatch.captured(1));
            addGame(nameMatch.captured(1), installDir, QStringLiteral("steam"),
                    appRe.match(text).captured(1));
        }
    }
}

void GameLibrary::scanEpic()
{
#ifdef Q_OS_WIN
    const QString programData = qEnvironmentVariable("PROGRAMDATA");
    if (programData.isEmpty()) {
        return;
    }
    QDir manifests(programData + QStringLiteral("/Epic/EpicGamesLauncher/Data/Manifests"));
    if (!manifests.exists()) {
        return;
    }
    const QStringList items = manifests.entryList({QStringLiteral("*.item")}, QDir::Files);
    for (const QString &item : items) {
        QFile file(manifests.filePath(item));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        addGame(object.value(QStringLiteral("DisplayName")).toString(),
                object.value(QStringLiteral("InstallLocation")).toString(),
                QStringLiteral("epic"));
    }
#endif
}
