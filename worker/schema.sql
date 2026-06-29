-- VodLink session matchmaker schema.
--
-- The Worker stores nothing permanent: only transient play sessions used to
-- connect friends who streamed the same game at the same time. Rows are
-- garbage-collected on every /stop (anything older than 24h is deleted).
--
-- friends stores JSON metadata for the open session: the owner's friend emails
-- plus Google display name/avatar used by matched clients. Mutual friendship is
-- resolved at match time (the requester is in the row owner's friends AND the
-- row owner is in the requester's friends).

CREATE TABLE IF NOT EXISTS sessions (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  email      TEXT    NOT NULL,
  game       TEXT    NOT NULL,
  youtube_id TEXT    NOT NULL,
  started_at INTEGER NOT NULL,        -- epoch milliseconds
  stopped_at INTEGER,                 -- epoch milliseconds, NULL while live
  friends    TEXT    NOT NULL DEFAULT '[]'
);

-- Overlap queries filter by game and time window; this index keeps /stop cheap.
CREATE INDEX IF NOT EXISTS idx_sessions_game_time
  ON sessions (game, started_at);

-- Finding a caller's own open session on /stop.
CREATE INDEX IF NOT EXISTS idx_sessions_email_open
  ON sessions (email, stopped_at);

CREATE UNIQUE INDEX IF NOT EXISTS idx_sessions_youtube_id
  ON sessions (youtube_id);

-- Fixed-window request counters. Keys are SHA-256 hashes of IP/account scopes;
-- raw addresses and bearer tokens are never persisted.
CREATE TABLE IF NOT EXISTS request_limits (
  key    TEXT    NOT NULL,
  bucket INTEGER NOT NULL,
  count  INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  PRIMARY KEY (key, bucket)
);
