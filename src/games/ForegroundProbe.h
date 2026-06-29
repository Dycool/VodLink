#pragma once

#include <QString>

// Heuristic detector for games not in the catalog. Reports the foreground
// application when it is running borderless/exclusive fullscreen on its monitor
// (the typical signature of a game), so VodLink can capture "every game" even
// without a catalog entry.
//
// Implemented on Windows via the Win32 API. On other platforms it is a no-op
// (catalog-only detection) — see README.
struct ForegroundApp
{
    QString executable; // lowercased basename, e.g. "mygame.exe"
    QString path;       // lowercased full executable path
    QString title;      // foreground window title
    bool valid = false;
};

namespace ForegroundProbe {

// Returns the foreground app if it covers its monitor fullscreen, else invalid.
[[nodiscard]] ForegroundApp fullscreenForeground();

// True for known non-games (shell, browsers, launchers, capture tools, VodLink).
[[nodiscard]] bool isDenylisted(const QString &executableLower);

// Derives a display name for an unknown game from its window title / executable.
[[nodiscard]] QString deriveName(const ForegroundApp &app);

} // namespace ForegroundProbe
