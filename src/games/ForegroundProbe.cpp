#include "ForegroundProbe.h"

#include <QFileInfo>
#include <QSet>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace {

const QSet<QString> &denylist()
{
    // Common foreground apps that go fullscreen but are not games we should capture.
    static const QSet<QString> kDenylist = {
        QStringLiteral("explorer.exe"), QStringLiteral("searchhost.exe"),
        QStringLiteral("shellexperiencehost.exe"), QStringLiteral("startmenuexperiencehost.exe"),
        QStringLiteral("dwm.exe"), QStringLiteral("applicationframehost.exe"),
        QStringLiteral("textinputhost.exe"), QStringLiteral("lockapp.exe"),
        QStringLiteral("taskmgr.exe"), QStringLiteral("vodlink.exe"),
        QStringLiteral("chrome.exe"), QStringLiteral("firefox.exe"),
        QStringLiteral("msedge.exe"), QStringLiteral("brave.exe"),
        QStringLiteral("opera.exe"), QStringLiteral("vivaldi.exe"),
        QStringLiteral("code.exe"), QStringLiteral("devenv.exe"),
        QStringLiteral("rider64.exe"), QStringLiteral("clion64.exe"),
        QStringLiteral("discord.exe"), QStringLiteral("slack.exe"),
        QStringLiteral("spotify.exe"), QStringLiteral("steam.exe"),
        QStringLiteral("steamwebhelper.exe"), QStringLiteral("epicgameslauncher.exe"),
        QStringLiteral("battle.net.exe"), QStringLiteral("riotclientservices.exe"),
        QStringLiteral("powerpnt.exe"), QStringLiteral("vlc.exe"),
        QStringLiteral("mpc-hc64.exe"), QStringLiteral("zoom.exe"),
        // Windows utilities and common desktop apps that are never games.
        QStringLiteral("systemsettings.exe"), QStringLiteral("mspaint.exe"),
        QStringLiteral("notepad.exe"), QStringLiteral("calc.exe"),
        QStringLiteral("cmd.exe"), QStringLiteral("powershell.exe"),
        QStringLiteral("pwsh.exe"), QStringLiteral("windowsterminal.exe"),
        QStringLiteral("wt.exe"), QStringLiteral("conhost.exe"),
        QStringLiteral("snippingtool.exe"), QStringLiteral("screenclippinghost.exe"),
        QStringLiteral("fdm.exe"), QStringLiteral("msdownloadmanager.exe"),
        QStringLiteral("winword.exe"), QStringLiteral("excel.exe"),
        QStringLiteral("outlook.exe"), QStringLiteral("onenote.exe"),
        QStringLiteral("teams.exe"), QStringLiteral("ms-teams.exe"),
        QStringLiteral("explorer.exe"), QStringLiteral("searchapp.exe"),
        QStringLiteral("widgets.exe"), QStringLiteral("phonelink.exe"),
        QStringLiteral("settingssynchost.exe"), QStringLiteral("photosapp.exe"),
        QStringLiteral("microsoft.photos.exe"), QStringLiteral("acrobat.exe"),
        QStringLiteral("acrord32.exe"), QStringLiteral("thunderbird.exe"),
    };
    return kDenylist;
}

} // namespace

namespace ForegroundProbe {

bool isDenylisted(const QString &executableLower)
{
    return denylist().contains(executableLower);
}

QString deriveName(const ForegroundApp &app)
{
    QString title = app.title.trimmed();
    if (!title.isEmpty() && title.size() <= 60) {
        return title;
    }
    // Fall back to the executable basename without extension, title-cased loosely.
    QString base = app.executable;
    if (base.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        base.chop(4);
    }
    base.replace(u'_', u' ').replace(u'-', u' ');
    base = base.trimmed();
    if (base.isEmpty()) {
        return {};
    }
    base[0] = base[0].toUpper();
    return base;
}

#ifdef Q_OS_WIN

ForegroundApp fullscreenForeground()
{
    ForegroundApp result;

    HWND window = GetForegroundWindow();
    if (window == nullptr) {
        return result;
    }

    RECT windowRect;
    if (!GetWindowRect(window, &windowRect)) {
        return result;
    }

    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo;
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return result;
    }

    // Fullscreen heuristic: the window covers (at least) the whole monitor.
    const RECT &screen = monitorInfo.rcMonitor;
    const bool coversMonitor = windowRect.left <= screen.left && windowRect.top <= screen.top &&
                               windowRect.right >= screen.right && windowRect.bottom >= screen.bottom;
    if (!coversMonitor) {
        return result;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return result;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return result;
    }
    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    const bool gotPath = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (!gotPath) {
        return result;
    }

    const QString exePath = QString::fromWCharArray(path, static_cast<int>(size));
    result.path = exePath.toLower();
    result.executable = QFileInfo(exePath).fileName().toLower();

    wchar_t titleBuffer[512] = {0};
    const int titleLength = GetWindowTextW(window, titleBuffer, 512);
    if (titleLength > 0) {
        result.title = QString::fromWCharArray(titleBuffer, titleLength);
    }

    result.valid = !result.executable.isEmpty();
    return result;
}

#else

ForegroundApp fullscreenForeground()
{
    return {};
}

#endif

} // namespace ForegroundProbe
