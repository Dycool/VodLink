#pragma once

#include "GameDefinition.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

class GameCatalog;

// A running process with both its basename and full executable path. The full
// path is what lets detection match a process to an installed game's folder.
struct ProcessInfo
{
    QString name; // lowercased basename, e.g. "eldenring.exe"
    QString path; // lowercased full path, e.g. "d:/steamlibrary/.../eldenring.exe"
};

// Detects running games by identifying every running process (full path against
// installed-game folders, then fallbacks) and, on Windows, by treating an
// unrecognised fullscreen foreground app as a game (heuristic). Emits start/stop
// edges so the controller can drive capture.
class GameDetector final : public QObject
{
    Q_OBJECT

public:
    explicit GameDetector(GameCatalog *catalog, QObject *parent = nullptr);

    void start(int intervalMs = 2000);
    void stop();

    // Sorted list of running executable names (lowercased) for the manual
    // "add current game" picker. Empty on error.
    [[nodiscard]] QStringList runningProcesses(QString *error) const;

signals:
    void gameStarted(const GameDefinition &game);
    void gameStopped(const GameDefinition &game);
    void scanFailed(const QString &message);

private slots:
    void scan();

private:
    [[nodiscard]] QVector<ProcessInfo> scanProcesses(QString *error) const;
    void collectHeuristicGame(const QSet<QString> &runningNames,
                              QHash<QString, GameDefinition> *current);

    GameCatalog *m_catalog;
    QTimer m_timer;
    QHash<QString, GameDefinition> m_activeGames; // name -> definition that started it
    int m_scanCount = 0;
    bool m_primed = false;

    // Foreground heuristic debounce: a fullscreen app must persist before it counts.
    QString m_foregroundCandidate;
    int m_foregroundStreak = 0;
    static constexpr int kForegroundScansToConfirm = 2;
    // Re-scan installed-game folders periodically (installs change rarely).
    static constexpr int kLibraryRefreshEveryScans = 150;
};
