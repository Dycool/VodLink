#pragma once

#include "Vod.h"

#include <QHash>
#include <QSqlDatabase>
#include <QStringList>
#include <QVector>

class VodRepository final
{
public:
    VodRepository();
    ~VodRepository();

    bool open(QString *error);
    // Closes the database connection and removes VodLink's whole local data
    // directory. Used by the destructive reset button before the app exits.
    bool deleteAllLocalData(QString *error);

    // My own VODs.
    bool add(const Vod &vod, QString *error);
    bool upsertOwnVod(const Vod &vod, QString *error);
    bool removeOwnVod(const QString &youtubeId, QString *error);
    [[nodiscard]] bool hasOwnVod(const QString &youtubeId, QString *error) const;
    [[nodiscard]] QVector<Vod> list(const QString &game, QString *error) const;

    // VODs received from friends via the Worker (owner_email is the friend).
    bool upsertFriendVod(const Vod &vod, QString *error);
    bool removeFriendVod(const QString &youtubeId, QString *error);
    [[nodiscard]] QVector<Vod> friendVods(const QString &game, QString *error) const;

    // Mine + friends for one game, ordered by start time (drives the synced player).
    [[nodiscard]] QVector<Vod> sessionVods(const QString &game, QString *error) const;

    // Distinct games across both my VODs and friend VODs.
    [[nodiscard]] QStringList games(QString *error) const;

    // Local cache of real YouTube Clips attached to a VOD. YouTube remains the
    // source of truth; this cache is rebuilt from VodLink metadata on login/sync.
    bool addClip(const VodClip &clip, QString *error);
    bool replaceClipsForVod(const QString &youtubeId, const QVector<VodClip> &clips, QString *error);
    [[nodiscard]] QVector<VodClip> clipsForVod(const QString &youtubeId, QString *error) const;

    // Clears ephemeral friend/session data only. Cached YouTube VODs and clips are
    // intentionally preserved across Google account switches.
    bool clearAccountData(QString *error);

    // Local friend list (emails). Mutual friendship is resolved by the Worker.
    bool addFriend(const QString &email, QString *error);
    bool removeFriend(const QString &email, QString *error);
    [[nodiscard]] QStringList friends(QString *error) const;
    [[nodiscard]] QVector<AccountProfile> friendProfiles(QString *error) const;

    // Key/value app settings (share toggle, refresh token, worker url, ...).
    [[nodiscard]] QString setting(const QString &key, const QString &defaultValue = {}) const;
    bool setSetting(const QString &key, const QString &value, QString *error = nullptr);

    // User-added executable -> game name entries that extend the catalog.
    [[nodiscard]] QHash<QString, QString> userGames(QString *error) const;
    bool addUserGame(const QString &executableLower, const QString &name, QString *error);

private:
    QString m_connectionName;
    QSqlDatabase m_database;
};
