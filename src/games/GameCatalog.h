#pragma once

#include "GameLibrary.h"

#include <QHash>
#include <QString>

// Identifies the game a process belongs to. Detection is local and layered, in
// priority order (best first):
//   1. user-saved mapping (full executable path or basename -> name), from SQLite
//   2. installed-game folder match: the process's full path lives inside a game
//      folder a launcher (Steam/Epic/...) reported  -> the robust, scalable path
//   3. a small conservative hard-coded fallback list for borderless-friendly games
// Anything still unknown is left to manual add.
//
// Online sources (IGDB, Steam, RAWG) are great for *metadata* (covers, genres)
// but unreliable for the exact on-disk executable, so they are intentionally not
// used for detection. They can be layered on later for display only.
class GameCatalog
{
public:
    GameCatalog();

    // Re-scan installed-game folders from launcher manifests (no network).
    void refreshLibrary();

    // Full detection chain. fullPathLower may be empty (then only 1 and 3 apply).
    [[nodiscard]] QString identify(const QString &executableLower,
                                   const QString &fullPathLower) const;

    // Convenience for callers that only have an executable basename.
    [[nodiscard]] QString lookup(const QString &executableLower) const;

    void setUserEntries(const QHash<QString, QString> &entries);
    void addUserEntry(const QString &executableLower, const QString &displayName);

private:
    QHash<QString, QString> m_fallbacks; // hard-coded well-known executables
    QHash<QString, QString> m_user;      // user-saved overrides
    GameLibrary m_library;               // installed games from launcher manifests
};
