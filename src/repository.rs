use crate::models::{AccountProfile, Vod, VodClip};
use anyhow::{Context, Result, bail};
use chrono::{DateTime, Utc};
use rusqlite::{Connection, OptionalExtension, params};
use std::path::Path;
use std::sync::{Arc, Mutex, MutexGuard};

#[derive(Clone)]
pub(crate) struct VodRepository {
    connection: Arc<Mutex<Connection>>,
}

impl VodRepository {
    pub(crate) fn open(path: &Path) -> Result<Self> {
        let connection = Connection::open(path)
            .with_context(|| format!("Could not open VodLink database {}", path.display()))?;
        let repo = Self { connection: Arc::new(Mutex::new(connection)) };
        repo.migrate()?;
        Ok(repo)
    }

    fn conn(&self) -> Result<MutexGuard<'_, Connection>> {
        self.connection.lock().map_err(|_| anyhow::anyhow!("VodLink database lock was poisoned"))
    }

    fn migrate(&self) -> Result<()> {
        let conn = self.conn()?;
        conn.execute_batch(
            r#"
            PRAGMA journal_mode=WAL;
            PRAGMA foreign_keys=ON;

            CREATE TABLE IF NOT EXISTS vods(
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                game TEXT NOT NULL,
                youtube_id TEXT NOT NULL UNIQUE,
                account_email TEXT NOT NULL DEFAULT '',
                stream_status TEXT NOT NULL,
                started_at TEXT NOT NULL,
                duration_ms INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS friend_vods(
                owner_email TEXT NOT NULL,
                owner_name TEXT NOT NULL DEFAULT '',
                owner_picture_url TEXT NOT NULL DEFAULT '',
                game TEXT NOT NULL,
                youtube_id TEXT NOT NULL,
                started_at TEXT NOT NULL,
                duration_ms INTEGER NOT NULL DEFAULT 0,
                received_at TEXT NOT NULL,
                PRIMARY KEY(owner_email,youtube_id)
            );
            CREATE TABLE IF NOT EXISTS friends(
                email TEXT PRIMARY KEY,
                display_name TEXT,
                picture_url TEXT NOT NULL DEFAULT '',
                added_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS settings(
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS user_games(
                executable TEXT PRIMARY KEY,
                display_name TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS vod_clips(
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                youtube_id TEXT NOT NULL,
                clip_id TEXT NOT NULL DEFAULT '',
                clip_url TEXT NOT NULL DEFAULT '',
                title TEXT NOT NULL,
                start_seconds INTEGER NOT NULL DEFAULT 0,
                end_seconds INTEGER NOT NULL DEFAULT 0,
                created_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_vods_game ON vods(game);
            CREATE INDEX IF NOT EXISTS idx_vods_started ON vods(started_at);
            CREATE INDEX IF NOT EXISTS idx_friend_vods_game ON friend_vods(game);
            CREATE INDEX IF NOT EXISTS idx_clips_youtube ON vod_clips(youtube_id);
            "#,
        )?;
        Self::ensure_column(&conn, "vods", "account_email", "TEXT NOT NULL DEFAULT ''")?;
        Self::ensure_column(&conn, "friend_vods", "owner_name", "TEXT NOT NULL DEFAULT ''")?;
        Self::ensure_column(&conn, "friend_vods", "owner_picture_url", "TEXT NOT NULL DEFAULT ''")?;
        Self::ensure_column(&conn, "friends", "picture_url", "TEXT NOT NULL DEFAULT ''")?;
        Self::ensure_column(&conn, "vod_clips", "clip_id", "TEXT NOT NULL DEFAULT ''")?;
        Self::ensure_column(&conn, "vod_clips", "clip_url", "TEXT NOT NULL DEFAULT ''")?;
        Ok(())
    }

    fn ensure_column(conn: &Connection, table: &str, column: &str, declaration: &str) -> Result<()> {
        let mut statement = conn.prepare(&format!("PRAGMA table_info({table})"))?;
        let mut rows = statement.query([])?;
        while let Some(row) = rows.next()? {
            let name: String = row.get(1)?;
            if name == column {
                return Ok(());
            }
        }
        conn.execute(&format!("ALTER TABLE {table} ADD COLUMN {column} {declaration}"), [])?;
        Ok(())
    }

    pub(crate) fn setting(&self, key: &str) -> Result<Option<String>> {
        Ok(self.conn()?.query_row(
            "SELECT value FROM settings WHERE key=?1",
            [key],
            |row| row.get(0),
        ).optional()?)
    }

    pub(crate) fn set_setting(&self, key: &str, value: &str) -> Result<()> {
        self.conn()?.execute(
            "INSERT INTO settings(key,value) VALUES(?1,?2)
             ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            params![key, value],
        )?;
        Ok(())
    }

    pub(crate) fn remove_setting(&self, key: &str) -> Result<()> {
        self.conn()?.execute("DELETE FROM settings WHERE key=?1", [key])?;
        Ok(())
    }

    pub(crate) fn upsert_own_vod(&self, vod: &Vod) -> Result<()> {
        if vod.youtube_id.trim().is_empty() {
            bail!("Cannot save a VOD without a YouTube id");
        }
        self.conn()?.execute(
            r#"INSERT INTO vods(game,youtube_id,account_email,stream_status,started_at,duration_ms)
               VALUES(?1,?2,?3,?4,?5,?6)
               ON CONFLICT(youtube_id) DO UPDATE SET
                 game=excluded.game,
                 account_email=CASE WHEN excluded.account_email='' THEN vods.account_email ELSE excluded.account_email END,
                 stream_status=excluded.stream_status,
                 started_at=excluded.started_at,
                 duration_ms=excluded.duration_ms"#,
            params![
                vod.game,
                vod.youtube_id,
                vod.account_email.trim().to_lowercase(),
                vod.stream_status,
                vod.started_at.to_rfc3339(),
                vod.duration_ms.max(0),
            ],
        )?;
        Ok(())
    }

    pub(crate) fn upsert_friend_vod(&self, vod: &Vod) -> Result<()> {
        let email = vod.owner_email.trim().to_lowercase();
        if email.is_empty() || vod.youtube_id.trim().is_empty() {
            bail!("Friend VOD requires an owner email and YouTube id");
        }
        let now = Utc::now().to_rfc3339();
        self.conn()?.execute(
            r#"INSERT INTO friend_vods(owner_email,owner_name,owner_picture_url,game,youtube_id,started_at,duration_ms,received_at)
               VALUES(?1,?2,?3,?4,?5,?6,?7,?8)
               ON CONFLICT(owner_email,youtube_id) DO UPDATE SET
                 owner_name=CASE WHEN excluded.owner_name='' THEN friend_vods.owner_name ELSE excluded.owner_name END,
                 owner_picture_url=CASE WHEN excluded.owner_picture_url='' THEN friend_vods.owner_picture_url ELSE excluded.owner_picture_url END,
                 game=excluded.game,started_at=excluded.started_at,duration_ms=excluded.duration_ms,received_at=excluded.received_at"#,
            params![
                email,
                vod.owner_name,
                vod.owner_picture_url,
                vod.game,
                vod.youtube_id,
                vod.started_at.to_rfc3339(),
                vod.duration_ms.max(0),
                now,
            ],
        )?;
        if !vod.owner_name.trim().is_empty() || !vod.owner_picture_url.trim().is_empty() {
            self.upsert_friend_profile(&AccountProfile::new(
                &vod.owner_email,
                &vod.owner_name,
                &vod.owner_picture_url,
            ))?;
        }
        Ok(())
    }

    pub(crate) fn remove_own_vod(&self, youtube_id: &str) -> Result<()> {
        let mut conn = self.conn()?;
        let tx = conn.transaction()?;
        tx.execute("DELETE FROM vod_clips WHERE youtube_id=?1", [youtube_id])?;
        tx.execute("DELETE FROM vods WHERE youtube_id=?1", [youtube_id])?;
        tx.commit()?;
        Ok(())
    }

    pub(crate) fn remove_friend_vod(&self, youtube_id: &str) -> Result<()> {
        let mut conn = self.conn()?;
        let tx = conn.transaction()?;
        tx.execute("DELETE FROM vod_clips WHERE youtube_id=?1", [youtube_id])?;
        tx.execute("DELETE FROM friend_vods WHERE youtube_id=?1", [youtube_id])?;
        tx.commit()?;
        Ok(())
    }

    pub(crate) fn own_vod(&self, youtube_id: &str) -> Result<Option<Vod>> {
        let conn = self.conn()?;
        conn.query_row(
            "SELECT id,game,youtube_id,stream_status,started_at,duration_ms,account_email FROM vods WHERE youtube_id=?1 LIMIT 1",
            [youtube_id],
            |row| {
                let started: String = row.get(4)?;
                Ok(Vod {
                    id: row.get(0)?,
                    game: row.get(1)?,
                    youtube_id: row.get(2)?,
                    stream_status: row.get(3)?,
                    started_at: parse_utc(&started),
                    duration_ms: row.get(5)?,
                    account_email: row.get(6)?,
                    owner_email: String::new(),
                    owner_name: String::new(),
                    owner_picture_url: String::new(),
                    title: String::new(),
                })
            },
        ).optional().map_err(Into::into)
    }

    pub(crate) fn list(&self, game: Option<&str>) -> Result<Vec<Vod>> {
        let conn = self.conn()?;
        let mut result = Vec::new();
        let mut own_sql = String::from(
            "SELECT id,game,youtube_id,stream_status,started_at,duration_ms,account_email FROM vods"
        );
        if game.is_some() {
            own_sql.push_str(" WHERE game=?1");
        }
        own_sql.push_str(" ORDER BY started_at DESC");
        let mut statement = conn.prepare(&own_sql)?;
        let mapper = |row: &rusqlite::Row<'_>| -> rusqlite::Result<Vod> {
            let started: String = row.get(4)?;
            Ok(Vod {
                id: row.get(0)?,
                game: row.get(1)?,
                youtube_id: row.get(2)?,
                stream_status: row.get(3)?,
                started_at: parse_utc(&started),
                duration_ms: row.get(5)?,
                account_email: row.get(6)?,
                owner_email: String::new(),
                owner_name: String::new(),
                owner_picture_url: String::new(),
                title: String::new(),
            })
        };
        if let Some(game) = game {
            for row in statement.query_map([game], mapper)? {
                result.push(row?);
            }
        } else {
            for row in statement.query_map([], mapper)? {
                result.push(row?);
            }
        }

        let mut friend_sql = String::from(
            "SELECT owner_email,owner_name,owner_picture_url,game,youtube_id,started_at,duration_ms FROM friend_vods"
        );
        if game.is_some() {
            friend_sql.push_str(" WHERE game=?1");
        }
        friend_sql.push_str(" ORDER BY started_at DESC");
        let mut statement = conn.prepare(&friend_sql)?;
        let mapper = |row: &rusqlite::Row<'_>| -> rusqlite::Result<Vod> {
            let started: String = row.get(5)?;
            Ok(Vod {
                id: 0,
                game: row.get(3)?,
                youtube_id: row.get(4)?,
                stream_status: "shared".to_owned(),
                started_at: parse_utc(&started),
                duration_ms: row.get(6)?,
                account_email: String::new(),
                owner_email: row.get(0)?,
                owner_name: row.get(1)?,
                owner_picture_url: row.get(2)?,
                title: String::new(),
            })
        };
        if let Some(game) = game {
            for row in statement.query_map([game], mapper)? {
                result.push(row?);
            }
        } else {
            for row in statement.query_map([], mapper)? {
                result.push(row?);
            }
        }
        result.sort_by(|a, b| b.started_at.cmp(&a.started_at));
        Ok(result)
    }

    pub(crate) fn games(&self) -> Result<Vec<String>> {
        let conn = self.conn()?;
        let mut statement = conn.prepare(
            "SELECT game FROM vods UNION SELECT game FROM friend_vods ORDER BY game COLLATE NOCASE"
        )?;
        Ok(statement.query_map([], |row| row.get::<_, String>(0))?
            .collect::<rusqlite::Result<Vec<_>>>()?)
    }

    pub(crate) fn add_friend(&self, profile: &AccountProfile) -> Result<()> {
        if profile.email.is_empty() {
            bail!("Friend email cannot be empty");
        }
        self.conn()?.execute(
            r#"INSERT INTO friends(email,display_name,picture_url,added_at)
               VALUES(?1,?2,?3,?4)
               ON CONFLICT(email) DO UPDATE SET
                 display_name=CASE WHEN excluded.display_name='' THEN friends.display_name ELSE excluded.display_name END,
                 picture_url=CASE WHEN excluded.picture_url='' THEN friends.picture_url ELSE excluded.picture_url END"#,
            params![profile.email, profile.display_name, profile.picture_url, Utc::now().to_rfc3339()],
        )?;
        Ok(())
    }

    fn upsert_friend_profile(&self, profile: &AccountProfile) -> Result<()> {
        let conn = self.conn()?;
        conn.execute(
            "UPDATE friends SET
             display_name=CASE WHEN ?2='' THEN display_name ELSE ?2 END,
             picture_url=CASE WHEN ?3='' THEN picture_url ELSE ?3 END
             WHERE email=?1",
            params![profile.email, profile.display_name, profile.picture_url],
        )?;
        Ok(())
    }

    pub(crate) fn friends(&self) -> Result<Vec<AccountProfile>> {
        let conn = self.conn()?;
        let mut statement = conn.prepare(
            "SELECT email,COALESCE(display_name,''),picture_url FROM friends ORDER BY COALESCE(display_name,email) COLLATE NOCASE"
        )?;
        Ok(statement.query_map([], |row| {
            Ok(AccountProfile::new(
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
            ))
        })?.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    pub(crate) fn remove_friend(&self, email: &str) -> Result<()> {
        let email = email.trim().to_lowercase();
        let mut conn = self.conn()?;
        let tx = conn.transaction()?;
        tx.execute(
            "DELETE FROM vod_clips WHERE youtube_id IN (SELECT youtube_id FROM friend_vods WHERE owner_email=?1)",
            [&email],
        )?;
        tx.execute("DELETE FROM friend_vods WHERE owner_email=?1", [&email])?;
        tx.execute("DELETE FROM friends WHERE email=?1", [&email])?;
        tx.commit()?;
        Ok(())
    }

    pub(crate) fn clear_account_data(&self) -> Result<()> {
        let mut conn = self.conn()?;
        let tx = conn.transaction()?;
        tx.execute("DELETE FROM friend_vods", [])?;
        tx.execute("DELETE FROM friends", [])?;
        tx.commit()?;
        Ok(())
    }

    pub(crate) fn user_games(&self) -> Result<Vec<(String, String)>> {
        let conn = self.conn()?;
        let mut statement = conn.prepare(
            "SELECT executable,display_name FROM user_games ORDER BY display_name COLLATE NOCASE"
        )?;
        Ok(statement.query_map([], |row| Ok((row.get(0)?, row.get(1)?)))?
            .collect::<rusqlite::Result<Vec<_>>>()?)
    }

    pub(crate) fn set_user_game(&self, executable: &str, display_name: &str) -> Result<()> {
        let executable = executable.trim().to_lowercase();
        let display_name = display_name.trim();
        if executable.is_empty() || display_name.is_empty() {
            bail!("Game executable and display name cannot be empty");
        }
        self.conn()?.execute(
            "INSERT INTO user_games(executable,display_name) VALUES(?1,?2)
             ON CONFLICT(executable) DO UPDATE SET display_name=excluded.display_name",
            params![executable, display_name],
        )?;
        Ok(())
    }

    pub(crate) fn clips_for_vod(&self, youtube_id: &str) -> Result<Vec<VodClip>> {
        let conn = self.conn()?;
        let mut statement = conn.prepare(
            "SELECT id,youtube_id,clip_id,clip_url,title,start_seconds,end_seconds,created_at
             FROM vod_clips WHERE youtube_id=?1 ORDER BY start_seconds,id"
        )?;
        Ok(statement.query_map([youtube_id], map_clip)?
            .collect::<rusqlite::Result<Vec<_>>>()?)
    }

    pub(crate) fn add_clip(&self, clip: VodClip) -> Result<()> {
        let clip = clip.normalize();
        if clip.youtube_id.trim().is_empty() {
            bail!("Clip requires a parent YouTube VOD");
        }
        let conn = self.conn()?;
        if !clip.clip_id.trim().is_empty() {
            conn.execute("DELETE FROM vod_clips WHERE youtube_id=?1 AND clip_id=?2",
                params![clip.youtube_id, clip.clip_id])?;
        } else if !clip.clip_url.trim().is_empty() {
            conn.execute("DELETE FROM vod_clips WHERE youtube_id=?1 AND clip_url=?2",
                params![clip.youtube_id, clip.clip_url])?;
        }
        conn.execute(
            "INSERT INTO vod_clips(youtube_id,clip_id,clip_url,title,start_seconds,end_seconds,created_at)
             VALUES(?1,?2,?3,?4,?5,?6,?7)",
            params![
                clip.youtube_id, clip.clip_id, clip.clip_url, clip.title,
                clip.start_seconds, clip.end_seconds, clip.created_at.to_rfc3339()
            ],
        )?;
        Ok(())
    }

    pub(crate) fn replace_clips_for_vod(&self, youtube_id: &str, clips: &[VodClip]) -> Result<()> {
        let mut conn = self.conn()?;
        let tx = conn.transaction()?;
        tx.execute("DELETE FROM vod_clips WHERE youtube_id=?1", [youtube_id])?;
        for clip in clips.iter().cloned().map(VodClip::normalize) {
            tx.execute(
                "INSERT INTO vod_clips(youtube_id,clip_id,clip_url,title,start_seconds,end_seconds,created_at)
                 VALUES(?1,?2,?3,?4,?5,?6,?7)",
                params![
                    youtube_id, clip.clip_id, clip.clip_url, clip.title,
                    clip.start_seconds, clip.end_seconds, clip.created_at.to_rfc3339()
                ],
            )?;
        }
        tx.commit()?;
        Ok(())
    }
}

fn parse_utc(value: &str) -> DateTime<Utc> {
    DateTime::parse_from_rfc3339(value)
        .map(|dt| dt.with_timezone(&Utc))
        .unwrap_or_else(|_| Utc::now())
}

fn map_clip(row: &rusqlite::Row<'_>) -> rusqlite::Result<VodClip> {
    let created: String = row.get(7)?;
    Ok(VodClip {
        id: row.get(0)?,
        youtube_id: row.get(1)?,
        clip_id: row.get(2)?,
        clip_url: row.get(3)?,
        title: row.get(4)?,
        start_seconds: row.get(5)?,
        end_seconds: row.get(6)?,
        created_at: parse_utc(&created),
    })
}
