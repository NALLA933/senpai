-- AnonXMusic C++ port — Phase 1 data layer
-- SQLite schema. Mirrors the MongoDB collections from the original Python bot.
--
-- Notes:
--   * All ids are Telegram ids and fit in a signed 64-bit integer.
--     Group/chat ids are negative, user ids are positive.
--   * cmd_delete and admin_play live as columns on the `chats` row, exactly
--     like the original stored them as fields on the chatsdb document.
--     admin_play is PERSISTENT (survives restarts) as in the reference code.
--   * Ephemeral runtime state (active_calls, loop) is RAM-only and never
--     touches this file — see CacheManager, not a table here.

-- Concurrency / durability pragmas are applied at runtime by Database
-- (WAL journal, NORMAL synchronous, busy_timeout). They are shown here for
-- reference only:
--   PRAGMA journal_mode = WAL;
--   PRAGMA synchronous  = NORMAL;
--   PRAGMA foreign_keys = ON;

-- Chats the bot has been added to (+ per-chat boolean settings).
CREATE TABLE IF NOT EXISTS chats (
    chat_id     INTEGER PRIMARY KEY,
    cmd_delete  INTEGER NOT NULL DEFAULT 0,   -- 0/1 : auto-delete command messages
    admin_play  INTEGER NOT NULL DEFAULT 0    -- 0/1 : only admins may use /play
);

-- Every user that has interacted with the bot (for broadcast / stats).
CREATE TABLE IF NOT EXISTS users (
    user_id INTEGER PRIMARY KEY
);

-- Per-chat authorized (non-admin) users allowed to control playback.
-- One row per (chat_id, user_id) pair == a set keyed by chat_id.
CREATE TABLE IF NOT EXISTS auth (
    chat_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    PRIMARY KEY (chat_id, user_id)
);
CREATE INDEX IF NOT EXISTS idx_auth_chat ON auth (chat_id);

-- Per-chat interface language code (defaults to config LANG_CODE when absent).
CREATE TABLE IF NOT EXISTS lang (
    chat_id   INTEGER PRIMARY KEY,
    lang_code TEXT NOT NULL
);

-- Per-chat assigned assistant (userbot) account number, 1..N.
CREATE TABLE IF NOT EXISTS assistant (
    chat_id INTEGER PRIMARY KEY,
    num     INTEGER NOT NULL
);

-- Globally blacklisted chats and users.
CREATE TABLE IF NOT EXISTS blacklist_chats (
    chat_id INTEGER PRIMARY KEY
);
CREATE TABLE IF NOT EXISTS blacklist_users (
    user_id INTEGER PRIMARY KEY
);

-- Bot sudo users.
CREATE TABLE IF NOT EXISTS sudoers (
    user_id INTEGER PRIMARY KEY
);

-- Bot-wide switches that must survive a restart (the Python bot stored these as
-- single MongoDB documents). Currently: 'logger' = '0'/'1' (log-group posting).
CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
