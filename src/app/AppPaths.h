#pragma once

#include <QString>
#include <QStringList>

namespace AppPaths {

// Canonical VodLink-owned local data root.
// Windows: %LOCALAPPDATA%/VodLink (not %LOCALAPPDATA%/VodLink/VodLink)
// macOS:   ~/Library/Application Support/VodLink
// Linux:   ~/.local/share/VodLink
[[nodiscard]] QString dataRoot();

// Keep transient/cache files under the same VodLink-owned tree so the reset
// button has exactly one app-owned folder to remove.
[[nodiscard]] QString cacheRoot();

// All known current + legacy VodLink-owned roots that the destructive reset
// should remove after the process exits.
[[nodiscard]] QStringList resetTargets();

// Starts a tiny OS helper that waits for this process to exit and then deletes
// resetTargets().  Deleting from the running process is unreliable because
// SQLite, QtWebEngine and OBS can still have files open on Windows.
bool scheduleResetAfterExit(QString *error);

} // namespace AppPaths
