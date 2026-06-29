#pragma once

#include <QString>
#include <QVector>

// A game actually installed on this PC, discovered from a launcher's local
// manifests. Detection matches a running process's full path against installDir.
struct InstalledGame
{
    QString name;
    QString installDir; // normalised: lowercase, forward slashes, no trailing slash
    QString source;     // "steam", "epic", ...
    QString appId;      // launcher id when known (e.g. Steam appid)
};

// Discovers installed games from local launcher manifests (no network). This is
// the robust core of detection: online databases know game names but not the
// exact executable on a user's disk, whereas the launcher already recorded where
// each game lives. We map a running process path -> the game whose folder
// contains it.
class GameLibrary
{
public:
    void refresh();

    // Returns the game whose install folder contains fullExecutablePath, or empty.
    [[nodiscard]] QString matchByPath(const QString &fullExecutablePath) const;

    [[nodiscard]] const QVector<InstalledGame> &games() const { return m_games; }

private:
    void scanSteam();
    void scanEpic();
    void addGame(const QString &name, const QString &installDir, const QString &source,
                 const QString &appId = {});

    [[nodiscard]] static QString normalize(const QString &path);

    QVector<InstalledGame> m_games;
};
