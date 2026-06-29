#include "GameDetector.h"

#include "ForegroundProbe.h"
#include "GameCatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QSet>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
#  include <libproc.h>
#  include <sys/types.h>
#endif

#ifdef Q_OS_WIN
namespace {
// Collects PIDs that own a visible, titled, top-level (non-tool) window — i.e.
// foreground apps the user actually interacts with, not background services.
BOOL CALLBACK collectWindowedPid(HWND hwnd, LPARAM lParam)
{
    auto *pids = reinterpret_cast<QSet<DWORD> *>(lParam);
    if (IsWindowVisible(hwnd) == FALSE || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    if (GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        pids->insert(pid);
    }
    return TRUE;
}

QStringList windowedProcessNames()
{
    QSet<DWORD> pids;
    EnumWindows(collectWindowedPid, reinterpret_cast<LPARAM>(&pids));

    QSet<QString> names;
    for (DWORD pid : pids) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) {
            continue;
        }
        wchar_t buffer[MAX_PATH] = {0};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
            const QString name = QFileInfo(QString::fromWCharArray(buffer, static_cast<int>(size))).fileName().toLower();
            // Skip known non-game apps (browsers, system utilities, VodLink itself, …).
            if (!name.isEmpty() && !ForegroundProbe::isDenylisted(name)) {
                names.insert(name);
            }
        }
        CloseHandle(process);
    }
    QStringList sorted(names.constBegin(), names.constEnd());
    sorted.sort();
    return sorted;
}
} // namespace
#endif

GameDetector::GameDetector(GameCatalog *catalog, QObject *parent)
    : QObject(parent), m_catalog(catalog)
{
    connect(&m_timer, &QTimer::timeout, this, &GameDetector::scan);
}

void GameDetector::start(int intervalMs)
{
    m_catalog->refreshLibrary();
    scan();
    m_timer.start(intervalMs);
}

void GameDetector::stop()
{
    m_timer.stop();
}

QStringList GameDetector::runningProcesses(QString *error) const
{
#ifdef Q_OS_WIN
    // Only apps with a real window, minus the blacklist — so the manual picker
    // isn't flooded with background services. May legitimately be empty (callers
    // must handle that), which is preferable to dumping every process.
    Q_UNUSED(error);
    return windowedProcessNames();
#else
    const QVector<ProcessInfo> processes = scanProcesses(error);
    if (error != nullptr && !error->isEmpty()) {
        return {};
    }
    QSet<QString> names;
    for (const ProcessInfo &info : processes) {
        if (!info.name.isEmpty()) {
            names.insert(info.name);
        }
    }
    QStringList sorted(names.constBegin(), names.constEnd());
    sorted.sort();
    return sorted;
#endif
}

void GameDetector::scan()
{
    QString error;
    const QVector<ProcessInfo> processes = scanProcesses(&error);
    if (!error.isEmpty()) {
        emit scanFailed(error);
        return;
    }

    // Periodically pick up newly installed games.
    if (++m_scanCount % kLibraryRefreshEveryScans == 0) {
        m_catalog->refreshLibrary();
    }

    // 1) Identify every running process (install-folder match, then fallbacks).
    QHash<QString, GameDefinition> current;
    QSet<QString> runningNames;
    for (const ProcessInfo &process : processes) {
        runningNames.insert(process.name);
        const QString name = m_catalog->identify(process.name, process.path);
        if (!name.isEmpty()) {
            auto it = current.find(name);
            if (it == current.end()) {
                current.insert(name, GameDefinition{name, {process.name}});
            } else if (!it->processNames.contains(process.name)) {
                it->processNames.push_back(process.name);
            }
        }
    }

    // Only catalog-identified games (installed via known launchers / known
    // executables) are recorded. The fullscreen-foreground heuristic was removed
    // because it misfired on ordinary apps (e.g. the Snipping Tool).
    Q_UNUSED(runningNames);

    // The first scan establishes a baseline. Games that were already open
    // when VodLink started did not produce a launch edge and must not stream.
    if (!m_primed) {
        m_activeGames = current;
        m_primed = true;
        return;
    }

    // 3) Emit start/stop edges.
    for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
        if (!m_activeGames.contains(it.key())) {
            m_activeGames.insert(it.key(), it.value());
            emit gameStarted(it.value());
        }
    }
    const QStringList previouslyActive = m_activeGames.keys();
    for (const QString &name : previouslyActive) {
        if (!current.contains(name)) {
            const GameDefinition stopped = m_activeGames.take(name);
            emit gameStopped(stopped);
        }
    }
}

void GameDetector::collectHeuristicGame(const QSet<QString> &runningNames,
                                        QHash<QString, GameDefinition> *current)
{
    const ForegroundApp app = ForegroundProbe::fullscreenForeground();
    if (!app.valid || ForegroundProbe::isDenylisted(app.executable)) {
        m_foregroundCandidate.clear();
        m_foregroundStreak = 0;
        return;
    }
    // If detection already named it (install folder or fallback), it's handled.
    if (!m_catalog->identify(app.executable, app.path).isEmpty()) {
        m_foregroundCandidate.clear();
        m_foregroundStreak = 0;
        return;
    }
    if (!runningNames.contains(app.executable)) {
        return;
    }

    // Debounce: require the same fullscreen app across consecutive scans.
    if (app.executable == m_foregroundCandidate) {
        ++m_foregroundStreak;
    } else {
        m_foregroundCandidate = app.executable;
        m_foregroundStreak = 1;
    }
    if (m_foregroundStreak < kForegroundScansToConfirm) {
        return;
    }

    const QString name = ForegroundProbe::deriveName(app);
    if (!name.isEmpty()) {
        current->insert(name, GameDefinition{name, {app.executable}});
    }
}

#ifdef Q_OS_WIN

QVector<ProcessInfo> GameDetector::scanProcesses(QString *error) const
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not enumerate running processes.");
        }
        return {};
    }

    QVector<ProcessInfo> result;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessInfo info;
            info.name = QString::fromWCharArray(entry.szExeFile).toLower();

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (process != nullptr) {
                wchar_t buffer[MAX_PATH] = {0};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
                    info.path = QString::fromWCharArray(buffer, static_cast<int>(size)).toLower();
                }
                CloseHandle(process);
            }
            result.push_back(info);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

#elif defined(Q_OS_LINUX)

namespace {

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLocal8Bit(file.readAll()).trimmed();
}

QStringList readCmdline(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    QStringList result;
    for (const QByteArray &part : data.split('\0')) {
        const QString value = QString::fromLocal8Bit(part).trimmed();
        if (!value.isEmpty()) {
            result.push_back(value);
        }
    }
    return result;
}

bool looksLikeGamePath(const QString &path)
{
    const QString normalized = path.toLower();
    return normalized.contains(QStringLiteral("/steamapps/common/"))
        || normalized.contains(QStringLiteral("/gog games/"))
        || normalized.contains(QStringLiteral("/heroic/"))
        || normalized.contains(QStringLiteral("/lutris/"));
}

void addProcessCandidate(QVector<ProcessInfo> *out, QSet<QString> *seen,
                         const QString &name, const QString &path)
{
    const QString lowerPath = path.trimmed().toLower();
    QString lowerName = name.trimmed().toLower();
    if (lowerName.isEmpty() && !lowerPath.isEmpty()) {
        lowerName = QFileInfo(lowerPath).fileName().toLower();
    }
    if (lowerName.isEmpty() && lowerPath.isEmpty()) {
        return;
    }
    const QString key = lowerName + QLatin1Char('|') + lowerPath;
    if (seen->contains(key)) {
        return;
    }
    seen->insert(key);
    out->push_back({lowerName, lowerPath});
}

} // namespace

QVector<ProcessInfo> GameDetector::scanProcesses(QString *error) const
{
    Q_UNUSED(error);

    QVector<ProcessInfo> result;
    QSet<QString> seen;

    QDir proc(QStringLiteral("/proc"));
    const QStringList pids = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : pids) {
        bool numeric = false;
        pid.toInt(&numeric);
        if (!numeric) {
            continue;
        }

        const QString base = proc.filePath(pid);
        const QString comm = readTextFile(base + QStringLiteral("/comm")).toLower();
        const QString exePath = QFileInfo(base + QStringLiteral("/exe")).symLinkTarget();
        const QString cwdPath = QFileInfo(base + QStringLiteral("/cwd")).symLinkTarget();
        const QStringList cmdline = readCmdline(base + QStringLiteral("/cmdline"));

        // Native Linux games usually expose the real executable through /proc/pid/exe.
        addProcessCandidate(&result, &seen, comm, exePath);

        // Proton/Wine often exposes wine/preloader as /proc/pid/exe, while the
        // launched Windows .exe or game directory appears in cmdline/cwd. Add
        // those as extra candidates so installed-folder matching works for both
        // native and Proton games.
        for (const QString &arg : cmdline) {
            if (QDir::isAbsolutePath(arg) && QFileInfo(arg).exists()) {
                addProcessCandidate(&result, &seen, QFileInfo(arg).fileName(), arg);
            }
        }
        if (!cwdPath.isEmpty() && looksLikeGamePath(cwdPath)) {
            addProcessCandidate(&result, &seen, comm, cwdPath);
        }
    }

    return result;
}

#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)

QVector<ProcessInfo> GameDetector::scanProcesses(QString *error) const
{
    const int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not enumerate running processes.");
        }
        return {};
    }

    QVector<pid_t> pids(bytes / static_cast<int>(sizeof(pid_t)));
    const int used = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), bytes);
    if (used <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not enumerate running processes.");
        }
        return {};
    }
    pids.resize(used / static_cast<int>(sizeof(pid_t)));

    QVector<ProcessInfo> result;
    QSet<QString> seen;
    for (pid_t pid : pids) {
        if (pid <= 0) {
            continue;
        }

        char pathBuffer[PROC_PIDPATHINFO_MAXSIZE] = {};
        const int pathLength = proc_pidpath(pid, pathBuffer, sizeof(pathBuffer));
        char nameBuffer[PROC_PIDPATHINFO_MAXSIZE] = {};
        proc_name(pid, nameBuffer, sizeof(nameBuffer));

        const QString path = pathLength > 0
                                 ? QString::fromLocal8Bit(pathBuffer, pathLength)
                                 : QString();
        QString name = QString::fromLocal8Bit(nameBuffer).trimmed();
        if (name.isEmpty() && !path.isEmpty()) {
            name = QFileInfo(path).fileName();
        }
        name = name.trimmed().toLower();
        const QString lowerPath = path.trimmed().toLower();
        if (name.isEmpty() && lowerPath.isEmpty()) {
            continue;
        }
        const QString key = name + QLatin1Char('|') + lowerPath;
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        result.push_back({name, lowerPath});
    }
    return result;
}

#else

QVector<ProcessInfo> GameDetector::scanProcesses(QString *error) const
{
    Q_UNUSED(error);
    return {};
}

#endif
