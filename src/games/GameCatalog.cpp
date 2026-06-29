#include "GameCatalog.h"

GameCatalog::GameCatalog()
    : m_fallbacks({
          // Keep this list intentionally conservative: these are mainstream
          // games/clients that commonly run in borderless/windowed fullscreen.
          // Games known to need exclusive-fullscreen compatibility should be
          // added by the user with "Add game manually" and recorded with
          // Full Desktop mode when needed.
          {QStringLiteral("cs2.exe"), QStringLiteral("Counter-Strike 2")},
          {QStringLiteral("valorant-win64-shipping.exe"), QStringLiteral("VALORANT")},
          {QStringLiteral("valorant.exe"), QStringLiteral("VALORANT")},
          {QStringLiteral("league of legends.exe"), QStringLiteral("League of Legends")},
          {QStringLiteral("overwatch.exe"), QStringLiteral("Overwatch 2")},
          {QStringLiteral("fortniteclient-win64-shipping.exe"), QStringLiteral("Fortnite")},
          {QStringLiteral("dota2.exe"), QStringLiteral("Dota 2")},
          {QStringLiteral("rocketleague.exe"), QStringLiteral("Rocket League")},
          {QStringLiteral("r5apex.exe"), QStringLiteral("Apex Legends")},
          {QStringLiteral("warframe.x64.exe"), QStringLiteral("Warframe")},
          {QStringLiteral("destiny2.exe"), QStringLiteral("Destiny 2")},
          {QStringLiteral("wow.exe"), QStringLiteral("World of Warcraft")},
          {QStringLiteral("hearthstone.exe"), QStringLiteral("Hearthstone")},
          {QStringLiteral("diablo iv.exe"), QStringLiteral("Diablo IV")},
          {QStringLiteral("starcraft ii.exe"), QStringLiteral("StarCraft II")},
          {QStringLiteral("sc2.exe"), QStringLiteral("StarCraft II")},
          {QStringLiteral("tf2.exe"), QStringLiteral("Team Fortress 2")},
          {QStringLiteral("portal2.exe"), QStringLiteral("Portal 2")},
          {QStringLiteral("amongus.exe"), QStringLiteral("Among Us")},
          {QStringLiteral("fallguys_client.exe"), QStringLiteral("Fall Guys")},
          {QStringLiteral("balatro.exe"), QStringLiteral("Balatro")},
          {QStringLiteral("vampire survivors.exe"), QStringLiteral("Vampire Survivors")},
      })
{
    // Make the fallback table cross-platform. The curated list is mostly
    // Windows executable names, but Linux/macOS process names often omit .exe
    // or use app bundle executable basenames. Add safe aliases so manual/user
    // detection and Proton/native launches both match.
    const auto existing = m_fallbacks;
    for (auto it = existing.constBegin(); it != existing.constEnd(); ++it) {
        QString key = it.key();
        if (key.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
            key.chop(4);
            if (!key.isEmpty() && !m_fallbacks.contains(key)) {
                m_fallbacks.insert(key, it.value());
            }
        }
    }

    const QHash<QString, QString> nativeAliases = {
        {QStringLiteral("cs2"), QStringLiteral("Counter-Strike 2")},
        {QStringLiteral("dota2"), QStringLiteral("Dota 2")},
        {QStringLiteral("portal2_linux"), QStringLiteral("Portal 2")},
        {QStringLiteral("balatro"), QStringLiteral("Balatro")},
    };
    for (auto it = nativeAliases.constBegin(); it != nativeAliases.constEnd(); ++it) {
        m_fallbacks.insert(it.key(), it.value());
    }
}

void GameCatalog::refreshLibrary()
{
    m_library.refresh();
}

QString GameCatalog::identify(const QString &executableLower, const QString &fullPathLower) const
{
    QString executable = executableLower.trimmed().toLower();
    QString executableNoExe = executable;
    if (executableNoExe.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        executableNoExe.chop(4);
    }

    QString fullPath = fullPathLower.trimmed().toLower();
    fullPath.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // 1. user-saved mapping wins. Manual entries can be exact executable paths
    // or plain basenames, so try exact path first, then basename, then alias.
    auto user = m_user.constFind(fullPath);
    if (user != m_user.constEnd()) {
        return user.value();
    }
    if (!fullPath.isEmpty()) {
        for (auto it = m_user.constBegin(); it != m_user.constEnd(); ++it) {
            const QString key = it.key().trimmed().toLower();
            if (key.size() >= 4 && fullPath.startsWith(key + QLatin1Char('/'))) {
                return it.value();
            }
        }
    }
    user = m_user.constFind(executable);
    if (user != m_user.constEnd()) {
        return user.value();
    }
    if (executableNoExe != executable) {
        user = m_user.constFind(executableNoExe);
        if (user != m_user.constEnd()) {
            return user.value();
        }
    }

    // 2. installed-game folder match (robust, scales to installed games).
    const QString byPath = m_library.matchByPath(fullPath);
    if (!byPath.isEmpty()) {
        return byPath;
    }

    // 3. conservative hard-coded fallback list for borderless-friendly games.
    const QString exact = m_fallbacks.value(executable);
    if (!exact.isEmpty()) {
        return exact;
    }
    return m_fallbacks.value(executableNoExe);
}

QString GameCatalog::lookup(const QString &executableLower) const
{
    return identify(executableLower, QString());
}

void GameCatalog::setUserEntries(const QHash<QString, QString> &entries)
{
    m_user = entries;
}

void GameCatalog::addUserEntry(const QString &executableLower, const QString &displayName)
{
    m_user.insert(executableLower, displayName);
}
