#include "VodRepository.h"

#include "app/AppPaths.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace {
Vod readVod(const QSqlQuery &query)
{
    Vod vod;
    vod.id = query.value(0).toLongLong();
    vod.game = query.value(1).toString();
    vod.youtubeId = query.value(2).toString();
    vod.streamStatus = query.value(3).toString();
    vod.startedAt = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs);
    vod.durationMs = query.value(5).toLongLong();
    vod.accountEmail = query.value(6).toString();
    vod.ownerEmail = query.value(7).toString();
    vod.ownerName = query.value(8).toString();
    vod.ownerPictureUrl = query.value(9).toString();
    return vod;
}

VodClip readClip(const QSqlQuery &query)
{
    VodClip clip;
    clip.id = query.value(0).toLongLong();
    clip.youtubeId = query.value(1).toString();
    clip.clipId = query.value(2).toString();
    clip.clipUrl = query.value(3).toString();
    clip.title = query.value(4).toString();
    clip.startSeconds = query.value(5).toInt();
    clip.endSeconds = query.value(6).toInt();
    clip.createdAt = QDateTime::fromString(query.value(7).toString(), Qt::ISODateWithMs);
    return clip;
}
} // namespace

VodRepository::VodRepository()
    : m_connectionName(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

VodRepository::~VodRepository()
{
    if (m_database.isValid()) {
        m_database.close();
    }
    m_database = {};
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool VodRepository::deleteAllLocalData(QString *error)
{
    // Close SQLite now, then let an external helper delete the whole VodLink
    // local-data tree after this process exits. Removing AppData while the app is
    // still alive is unreliable on Windows because QtWebEngine/OBS may still hold
    // files open.
    if (m_database.isValid()) {
        m_database.close();
    }
    m_database = {};
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }

    return AppPaths::scheduleResetAfterExit(error);
}

bool VodRepository::open(QString *error)
{
    const QString dataPath = AppPaths::dataRoot();
    if (!QDir().mkpath(dataPath)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not create the VodLink data directory: %1").arg(dataPath);
        }
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(QDir(dataPath).filePath(QStringLiteral("vodlink.db")));
    if (!m_database.open()) {
        if (error != nullptr) {
            *error = m_database.lastError().text();
        }
        return false;
    }

    const QStringList schema = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS vods ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "game TEXT NOT NULL,"
                       "youtube_id TEXT NOT NULL UNIQUE,"
                       "account_email TEXT NOT NULL DEFAULT '',"
                       "stream_status TEXT NOT NULL,"
                       "started_at TEXT NOT NULL,"
                       "duration_ms INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS friend_vods ("
                       "owner_email TEXT NOT NULL,"
                       "owner_name TEXT NOT NULL DEFAULT '',"
                       "owner_picture_url TEXT NOT NULL DEFAULT '',"
                       "game TEXT NOT NULL,"
                       "youtube_id TEXT NOT NULL,"
                       "started_at TEXT NOT NULL,"
                       "duration_ms INTEGER NOT NULL DEFAULT 0,"
                       "received_at TEXT NOT NULL,"
                       "PRIMARY KEY (owner_email, youtube_id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS friends ("
                       "email TEXT PRIMARY KEY,"
                       "display_name TEXT,"
                       "picture_url TEXT NOT NULL DEFAULT '',"
                       "added_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS settings ("
                       "key TEXT PRIMARY KEY,"
                       "value TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS user_games ("
                       "executable TEXT PRIMARY KEY,"
                       "display_name TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS vod_clips ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "youtube_id TEXT NOT NULL,"
                       "clip_id TEXT NOT NULL DEFAULT '',"
                       "clip_url TEXT NOT NULL DEFAULT '',"
                       "title TEXT NOT NULL,"
                       "start_seconds INTEGER NOT NULL DEFAULT 0,"
                       "end_seconds INTEGER NOT NULL DEFAULT 0,"
                       "created_at TEXT NOT NULL)"),
    };
    QSqlQuery query(m_database);
    for (const QString &statement : schema) {
        if (!query.exec(statement)) {
            if (error != nullptr) {
                *error = query.lastError().text();
            }
            return false;
        }
    }
    const auto ensureColumn = [this, error](const QString &table, const QString &column,
                                            const QString &definition) {
        QSqlQuery columns(m_database);
        if (!columns.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
            if (error != nullptr) {
                *error = columns.lastError().text();
            }
            return false;
        }
        while (columns.next()) {
            if (columns.value(1).toString() == column) {
                return true;
            }
        }
        QSqlQuery alter(m_database);
        if (!alter.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3")
                            .arg(table, column, definition))) {
            if (error != nullptr) {
                *error = alter.lastError().text();
            }
            return false;
        }
        return true;
    };
    if (!ensureColumn(QStringLiteral("vods"), QStringLiteral("account_email"),
                      QStringLiteral("TEXT NOT NULL DEFAULT ''"))
        || !ensureColumn(QStringLiteral("friend_vods"), QStringLiteral("owner_name"),
                      QStringLiteral("TEXT NOT NULL DEFAULT ''"))
        || !ensureColumn(QStringLiteral("friend_vods"), QStringLiteral("owner_picture_url"),
                         QStringLiteral("TEXT NOT NULL DEFAULT ''"))
        || !ensureColumn(QStringLiteral("friends"), QStringLiteral("picture_url"),
                         QStringLiteral("TEXT NOT NULL DEFAULT ''"))
        || !ensureColumn(QStringLiteral("vod_clips"), QStringLiteral("clip_id"),
                         QStringLiteral("TEXT NOT NULL DEFAULT ''"))
        || !ensureColumn(QStringLiteral("vod_clips"), QStringLiteral("clip_url"),
                         QStringLiteral("TEXT NOT NULL DEFAULT ''"))) {
        return false;
    }
    return true;
}

bool VodRepository::add(const Vod &vod, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO vods(game, youtube_id, account_email, stream_status, started_at, duration_ms) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(vod.game);
    query.addBindValue(vod.youtubeId);
    query.addBindValue(vod.accountEmail.trimmed().toLower());
    query.addBindValue(vod.streamStatus);
    query.addBindValue(vod.startedAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(vod.durationMs);
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

bool VodRepository::upsertOwnVod(const Vod &vod, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO vods(game, youtube_id, account_email, stream_status, started_at, duration_ms) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(youtube_id) DO UPDATE SET "
        "game = excluded.game, "
        "account_email = CASE WHEN excluded.account_email != '' THEN excluded.account_email ELSE vods.account_email END, "
        "stream_status = excluded.stream_status, "
        "started_at = excluded.started_at, duration_ms = excluded.duration_ms"));
    query.addBindValue(vod.game);
    query.addBindValue(vod.youtubeId.trimmed());
    query.addBindValue(vod.accountEmail.trimmed().toLower());
    query.addBindValue(vod.streamStatus.isEmpty() ? QStringLiteral("processing") : vod.streamStatus);
    query.addBindValue(vod.startedAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(vod.durationMs);
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

bool VodRepository::removeOwnVod(const QString &youtubeId, QString *error)
{
    QSqlQuery clips(m_database);
    clips.prepare(QStringLiteral("DELETE FROM vod_clips WHERE youtube_id = ?"));
    clips.addBindValue(youtubeId.trimmed());
    clips.exec();

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM vods WHERE youtube_id = ?"));
    query.addBindValue(youtubeId.trimmed());
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

bool VodRepository::hasOwnVod(const QString &youtubeId, QString *error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT 1 FROM vods WHERE youtube_id = ? LIMIT 1"));
    query.addBindValue(youtubeId.trimmed());
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return query.next();
}

QVector<Vod> VodRepository::list(const QString &game, QString *error) const
{
    QSqlQuery query(m_database);
    const QString columns = QStringLiteral(
        "SELECT id, game, youtube_id, stream_status, started_at, duration_ms, "
        "account_email, '' AS owner_email, '' AS owner_name, '' AS owner_picture_url "
        "FROM vods");
    if (game.isEmpty()) {
        query.prepare(columns + QStringLiteral(" ORDER BY started_at DESC"));
    } else {
        query.prepare(columns + QStringLiteral(" WHERE game = ? ORDER BY started_at DESC"));
        query.addBindValue(game);
    }
    QVector<Vod> result;
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return result;
    }
    while (query.next()) {
        result.push_back(readVod(query));
    }
    return result;
}


bool VodRepository::removeFriendVod(const QString &youtubeId, QString *error)
{
    QSqlQuery clips(m_database);
    clips.prepare(QStringLiteral("DELETE FROM vod_clips WHERE youtube_id = ?"));
    clips.addBindValue(youtubeId.trimmed());
    clips.exec();

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM friend_vods WHERE youtube_id = ?"));
    query.addBindValue(youtubeId.trimmed());
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

bool VodRepository::upsertFriendVod(const Vod &vod, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO friend_vods(owner_email, owner_name, owner_picture_url, game, youtube_id, "
        "started_at, duration_ms, received_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(owner_email, youtube_id) DO UPDATE SET "
        "game = excluded.game, started_at = excluded.started_at, "
        "duration_ms = excluded.duration_ms, owner_name = excluded.owner_name, "
        "owner_picture_url = excluded.owner_picture_url"));
    query.addBindValue(vod.ownerEmail);
    query.addBindValue(vod.ownerName);
    query.addBindValue(vod.ownerPictureUrl);
    query.addBindValue(vod.game);
    query.addBindValue(vod.youtubeId);
    query.addBindValue(vod.startedAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(vod.durationMs);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    QSqlQuery profile(m_database);
    profile.prepare(QStringLiteral(
        "UPDATE friends SET display_name = CASE WHEN ? != '' THEN ? ELSE display_name END, "
        "picture_url = CASE WHEN ? != '' THEN ? ELSE picture_url END WHERE email = ?"));
    profile.addBindValue(vod.ownerName);
    profile.addBindValue(vod.ownerName);
    profile.addBindValue(vod.ownerPictureUrl);
    profile.addBindValue(vod.ownerPictureUrl);
    profile.addBindValue(vod.ownerEmail.toLower());
    profile.exec();
    return true;
}

QVector<Vod> VodRepository::friendVods(const QString &game, QString *error) const
{
    QSqlQuery query(m_database);
    const QString columns = QStringLiteral(
        "SELECT 0 AS id, game, youtube_id, 'shared' AS stream_status, started_at, duration_ms, "
        "'' AS account_email, owner_email, owner_name, owner_picture_url FROM friend_vods");
    if (game.isEmpty()) {
        query.prepare(columns + QStringLiteral(" ORDER BY started_at DESC"));
    } else {
        query.prepare(columns + QStringLiteral(" WHERE game = ? ORDER BY started_at DESC"));
        query.addBindValue(game);
    }
    QVector<Vod> result;
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return result;
    }
    while (query.next()) {
        result.push_back(readVod(query));
    }
    return result;
}

QVector<Vod> VodRepository::sessionVods(const QString &game, QString *error) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, game, youtube_id, stream_status, started_at, duration_ms, "
        "account_email, '' AS owner_email, '' AS owner_name, '' AS owner_picture_url "
        "FROM vods WHERE game = ? "
        "UNION ALL "
        "SELECT 0 AS id, game, youtube_id, 'shared' AS stream_status, started_at, duration_ms, "
        "'' AS account_email, owner_email, owner_name, owner_picture_url FROM friend_vods WHERE game = ? "
        "ORDER BY started_at ASC"));
    query.addBindValue(game);
    query.addBindValue(game);
    QVector<Vod> result;
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return result;
    }
    while (query.next()) {
        result.push_back(readVod(query));
    }
    return result;
}

QStringList VodRepository::games(QString *error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT game FROM ("
            "SELECT DISTINCT game FROM vods UNION SELECT DISTINCT game FROM friend_vods"
            ") ORDER BY game"))) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return {};
    }
    QStringList result;
    while (query.next()) {
        result.push_back(query.value(0).toString());
    }
    return result;
}

bool VodRepository::addClip(const VodClip &clip, QString *error)
{
    const QString youtubeId = clip.youtubeId.trimmed();
    if (youtubeId.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("This clip has no parent YouTube VOD id.");
        }
        return false;
    }

    const int start = std::max(0, clip.startSeconds);
    const int end = std::max(start + 1, clip.endSeconds);
    QString title = clip.title.trimmed();
    if (title.isEmpty()) {
        title = QStringLiteral("Clip %1s–%2s").arg(start).arg(end);
    }

    QSqlQuery find(m_database);
    bool hasLookup = false;
    if (!clip.clipId.trimmed().isEmpty()) {
        find.prepare(QStringLiteral("SELECT id FROM vod_clips WHERE clip_id = ? LIMIT 1"));
        find.addBindValue(clip.clipId.trimmed());
        hasLookup = true;
    } else if (!clip.clipUrl.trimmed().isEmpty()) {
        find.prepare(QStringLiteral("SELECT id FROM vod_clips WHERE clip_url = ? LIMIT 1"));
        find.addBindValue(clip.clipUrl.trimmed());
        hasLookup = true;
    }

    qint64 existingId = 0;
    if (hasLookup) {
        if (!find.exec()) {
            if (error != nullptr) {
                *error = find.lastError().text();
            }
            return false;
        }
        if (find.next()) {
            existingId = find.value(0).toLongLong();
        }
    }

    QSqlQuery query(m_database);
    if (existingId > 0) {
        query.prepare(QStringLiteral(
            "UPDATE vod_clips SET youtube_id = ?, clip_id = ?, clip_url = ?, title = ?, "
            "start_seconds = ?, end_seconds = ? WHERE id = ?"));
        query.addBindValue(youtubeId);
        query.addBindValue(clip.clipId.trimmed());
        query.addBindValue(clip.clipUrl.trimmed());
        query.addBindValue(title.left(120));
        query.addBindValue(start);
        query.addBindValue(end);
        query.addBindValue(existingId);
    } else {
        query.prepare(QStringLiteral(
            "INSERT INTO vod_clips(youtube_id, clip_id, clip_url, title, start_seconds, end_seconds, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(youtubeId);
        query.addBindValue(clip.clipId.trimmed());
        query.addBindValue(clip.clipUrl.trimmed());
        query.addBindValue(title.left(120));
        query.addBindValue(start);
        query.addBindValue(end);
        query.addBindValue(clip.createdAt.isValid()
                               ? clip.createdAt.toUTC().toString(Qt::ISODateWithMs)
                               : QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    }

    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

bool VodRepository::replaceClipsForVod(const QString &youtubeId, const QVector<VodClip> &clips, QString *error)
{
    const QString id = youtubeId.trimmed();
    if (id.isEmpty()) {
        return true;
    }
    if (!m_database.transaction()) {
        if (error != nullptr) {
            *error = m_database.lastError().text();
        }
        return false;
    }

    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM vod_clips WHERE youtube_id = ?"));
    remove.addBindValue(id);
    if (!remove.exec()) {
        if (error != nullptr) {
            *error = remove.lastError().text();
        }
        m_database.rollback();
        return false;
    }

    for (VodClip clip : clips) {
        clip.youtubeId = id;
        if (!addClip(clip, error)) {
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        if (error != nullptr) {
            *error = m_database.lastError().text();
        }
        return false;
    }
    return true;
}

QVector<VodClip> VodRepository::clipsForVod(const QString &youtubeId, QString *error) const
{
    QVector<VodClip> result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, youtube_id, clip_id, clip_url, title, start_seconds, end_seconds, created_at "
        "FROM vod_clips WHERE youtube_id = ? ORDER BY start_seconds ASC, created_at ASC"));
    query.addBindValue(youtubeId.trimmed());
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return result;
    }
    while (query.next()) {
        result.push_back(readClip(query));
    }
    return result;
}

bool VodRepository::clearAccountData(QString *error)
{
    if (!m_database.transaction()) {
        if (error != nullptr) {
            *error = m_database.lastError().text();
        }
        return false;
    }

    const QStringList tables{
        // Conservative account switching: never delete cached YouTube VODs or
        // their imported YouTube Clips just because another Google account signed in.
        QStringLiteral("friend_vods"),
        QStringLiteral("friends"),
    };

    for (const QString &table : tables) {
        QSqlQuery query(m_database);
        if (!query.exec(QStringLiteral("DELETE FROM %1").arg(table))) {
            if (error != nullptr) {
                *error = query.lastError().text();
            }
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        if (error != nullptr) {
            *error = m_database.lastError().text();
        }
        return false;
    }
    return true;
}

bool VodRepository::addFriend(const QString &email, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO friends(email, display_name, picture_url, added_at) VALUES (?, '', '', ?) "
        "ON CONFLICT(email) DO NOTHING"));
    query.addBindValue(email.trimmed().toLower());
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

bool VodRepository::removeFriend(const QString &email, QString *error)
{
    const QString normalized = email.trimmed().toLower();
    if (!m_database.transaction()) {
        if (error != nullptr) {
            *error = m_database.lastError().text();
        }
        return false;
    }

    QSqlQuery friendClipVods(m_database);
    friendClipVods.prepare(QStringLiteral(
        "DELETE FROM vod_clips WHERE youtube_id IN "
        "(SELECT youtube_id FROM friend_vods WHERE owner_email = ?)"));
    friendClipVods.addBindValue(normalized);
    if (!friendClipVods.exec()) {
        if (error != nullptr) {
            *error = friendClipVods.lastError().text();
        }
        m_database.rollback();
        return false;
    }

    QSqlQuery friendVods(m_database);
    friendVods.prepare(QStringLiteral("DELETE FROM friend_vods WHERE owner_email = ?"));
    friendVods.addBindValue(normalized);
    if (!friendVods.exec()) {
        if (error != nullptr) {
            *error = friendVods.lastError().text();
        }
        m_database.rollback();
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM friends WHERE email = ?"));
    query.addBindValue(normalized);
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        m_database.rollback();
        return false;
    }

    if (!m_database.commit()) {
        if (error != nullptr) {
            *error = m_database.lastError().text();
        }
        return false;
    }
    return true;
}

QStringList VodRepository::friends(QString *error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT email FROM friends ORDER BY email"))) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return {};
    }
    QStringList result;
    while (query.next()) {
        result.push_back(query.value(0).toString());
    }
    return result;
}

QVector<AccountProfile> VodRepository::friendProfiles(QString *error) const
{
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT email, display_name, picture_url FROM friends ORDER BY "
            "CASE WHEN display_name = '' THEN email ELSE display_name END"))) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return {};
    }
    QVector<AccountProfile> result;
    while (query.next()) {
        result.push_back({query.value(0).toString(), query.value(1).toString(),
                          query.value(2).toString()});
    }
    return result;
}

QString VodRepository::setting(const QString &key, const QString &defaultValue) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return defaultValue;
}

bool VodRepository::setSetting(const QString &key, const QString &value, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO settings(key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

QHash<QString, QString> VodRepository::userGames(QString *error) const
{
    QSqlQuery query(m_database);
    QHash<QString, QString> result;
    if (!query.exec(QStringLiteral("SELECT executable, display_name FROM user_games"))) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return result;
    }
    while (query.next()) {
        result.insert(query.value(0).toString(), query.value(1).toString());
    }
    return result;
}

bool VodRepository::addUserGame(const QString &executableLower, const QString &name, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO user_games(executable, display_name) VALUES (?, ?) "
        "ON CONFLICT(executable) DO UPDATE SET display_name = excluded.display_name"));
    query.addBindValue(executableLower.trimmed().toLower());
    query.addBindValue(name);
    if (!query.exec()) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}
