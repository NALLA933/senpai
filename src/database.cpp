// AnonXMusic C++ port — Phase 1 data layer
// database.cpp — implementation of the SQLite-backed, write-through cache.

#include "anonx/database.hpp"

#include <sqlite3.h>

#include <iostream>

namespace anonx {
namespace {

// Small RAII wrapper around a prepared statement: prepare in the ctor,
// finalize in the dtor. Keeps every DB helper exception-safe.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw DatabaseError(std::string("prepare failed: ") + sqlite3_errmsg(db));
        }
    }
    ~Stmt() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bindInt(int idx, std::int64_t v) { sqlite3_bind_int64(stmt_, idx, v); }
    void bindText(int idx, const std::string& v) {
        sqlite3_bind_text(stmt_, idx, v.c_str(), -1, SQLITE_TRANSIENT);
    }
    int          step() { return sqlite3_step(stmt_); }
    std::int64_t colInt(int c) { return sqlite3_column_int64(stmt_, c); }
    std::string  colText(int c) {
        const unsigned char* t = sqlite3_column_text(stmt_, c);
        return t ? reinterpret_cast<const char*>(t) : std::string{};
    }

private:
    sqlite3*      db_   = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

void logError(const char* ctx, const std::exception& e) {
    std::cerr << "[Database] " << ctx << ": " << e.what() << std::endl;
}

// Schema kept in sync with schema.sql (admin_play is persistent).
const char* const kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS chats (
    chat_id     INTEGER PRIMARY KEY,
    cmd_delete  INTEGER NOT NULL DEFAULT 0,
    admin_play  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS users (
    user_id INTEGER PRIMARY KEY
);
CREATE TABLE IF NOT EXISTS auth (
    chat_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    PRIMARY KEY (chat_id, user_id)
);
CREATE INDEX IF NOT EXISTS idx_auth_chat ON auth (chat_id);
CREATE TABLE IF NOT EXISTS lang (
    chat_id   INTEGER PRIMARY KEY,
    lang_code TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS assistant (
    chat_id INTEGER PRIMARY KEY,
    num     INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS blacklist_chats (
    chat_id INTEGER PRIMARY KEY
);
CREATE TABLE IF NOT EXISTS blacklist_users (
    user_id INTEGER PRIMARY KEY
);
CREATE TABLE IF NOT EXISTS sudoers (
    user_id INTEGER PRIMARY KEY
);
CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
)SQL";

}  // namespace

// ============================================================
// Construction / teardown (RAII)
// ============================================================

Database::Database(const std::string& path) : rng_(std::random_device{}()) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
        throw DatabaseError("failed to open '" + path + "': " + msg);
    }
    // Concurrency + durability: WAL lets readers and a writer run together;
    // NORMAL is the recommended durability level with WAL; busy_timeout makes
    // the (rare) writer contention block briefly instead of failing.
    execOrThrow("PRAGMA journal_mode=WAL;");
    execOrThrow("PRAGMA synchronous=NORMAL;");
    execOrThrow("PRAGMA foreign_keys=ON;");
    sqlite3_busy_timeout(db_, 5000);

    execOrThrow(kSchemaSql);
}

Database::~Database() {
    if (db_) sqlite3_close_v2(db_);
}

// ============================================================
// Low-level helpers (mtx_ assumed held)
// ============================================================

void Database::execOrThrow(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DatabaseError(std::string("exec failed: ") + msg);
    }
}

void Database::loadIdSet(const char* sql, std::unordered_set<std::int64_t>& out) {
    Stmt st(db_, sql);
    while (st.step() == SQLITE_ROW) out.insert(st.colInt(0));
}

bool Database::insertId(const char* sql, std::int64_t id) {
    Stmt st(db_, sql);
    st.bindInt(1, id);
    if (st.step() != SQLITE_DONE) throw DatabaseError(sqlite3_errmsg(db_));
    return true;
}

bool Database::deleteId(const char* sql, std::int64_t id) {
    Stmt st(db_, sql);
    st.bindInt(1, id);
    if (st.step() != SQLITE_DONE) throw DatabaseError(sqlite3_errmsg(db_));
    return true;
}

void Database::ensureChatsLoaded() {
    if (chatsLoaded_) return;
    loadIdSet("SELECT chat_id FROM chats;", chats_);
    chatsLoaded_ = true;
}
void Database::ensureUsersLoaded() {
    if (usersLoaded_) return;
    loadIdSet("SELECT user_id FROM users;", users_);
    usersLoaded_ = true;
}
void Database::ensureAuthLoaded(std::int64_t chatId) {
    if (auth_.find(chatId) != auth_.end()) return;
    std::unordered_set<std::int64_t> s;
    Stmt st(db_, "SELECT user_id FROM auth WHERE chat_id=?;");
    st.bindInt(1, chatId);
    while (st.step() == SQLITE_ROW) s.insert(st.colInt(0));
    auth_.emplace(chatId, std::move(s));
}
void Database::ensureBlChatsLoaded() {
    if (blChatsLoaded_) return;
    loadIdSet("SELECT chat_id FROM blacklist_chats;", blChats_);
    blChatsLoaded_ = true;
}
void Database::ensureBlUsersLoaded() {
    if (blUsersLoaded_) return;
    loadIdSet("SELECT user_id FROM blacklist_users;", blUsers_);
    blUsersLoaded_ = true;
}
void Database::ensureSudoLoaded() {
    if (sudoLoaded_) return;
    loadIdSet("SELECT user_id FROM sudoers;", sudoers_);
    sudoLoaded_ = true;
}

// ============================================================
// Global config
// ============================================================

void Database::setDefaultLang(const std::string& code) {
    std::lock_guard<std::mutex> lk(mtx_);
    defaultLang_ = code;
}
void Database::setAssistantCount(int count) {
    std::lock_guard<std::mutex> lk(mtx_);
    assistantCount_ = count < 1 ? 1 : count;
}

// ============================================================
// chats
// ============================================================

bool Database::isChat(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureChatsLoaded();
        return chats_.count(chatId) > 0;
    } catch (const std::exception& e) {
        logError("isChat", e);
        return false;
    }
}
bool Database::addChat(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureChatsLoaded();
        if (chats_.count(chatId)) return true;
        insertId("INSERT OR IGNORE INTO chats(chat_id) VALUES(?);", chatId);
        chats_.insert(chatId);
        return true;
    } catch (const std::exception& e) {
        logError("addChat", e);
        return false;
    }
}
bool Database::removeChat(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        deleteId("DELETE FROM chats WHERE chat_id=?;", chatId);
        chats_.erase(chatId);
        cmdDelete_.erase(chatId);
        adminPlay_.erase(chatId);
        return true;
    } catch (const std::exception& e) {
        logError("removeChat", e);
        return false;
    }
}
std::vector<std::int64_t> Database::getChats() {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureChatsLoaded();
        return std::vector<std::int64_t>(chats_.begin(), chats_.end());
    } catch (const std::exception& e) {
        logError("getChats", e);
        return {};
    }
}
std::size_t Database::chatCount() {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureChatsLoaded();
        return chats_.size();
    } catch (const std::exception& e) {
        logError("chatCount", e);
        return 0;
    }
}

// ============================================================
// users
// ============================================================

bool Database::isUser(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureUsersLoaded();
        return users_.count(userId) > 0;
    } catch (const std::exception& e) {
        logError("isUser", e);
        return false;
    }
}
bool Database::addUser(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureUsersLoaded();
        if (users_.count(userId)) return true;
        insertId("INSERT OR IGNORE INTO users(user_id) VALUES(?);", userId);
        users_.insert(userId);
        return true;
    } catch (const std::exception& e) {
        logError("addUser", e);
        return false;
    }
}
bool Database::removeUser(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        deleteId("DELETE FROM users WHERE user_id=?;", userId);
        users_.erase(userId);
        return true;
    } catch (const std::exception& e) {
        logError("removeUser", e);
        return false;
    }
}
std::vector<std::int64_t> Database::getUsers() {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureUsersLoaded();
        return std::vector<std::int64_t>(users_.begin(), users_.end());
    } catch (const std::exception& e) {
        logError("getUsers", e);
        return {};
    }
}
std::size_t Database::userCount() {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureUsersLoaded();
        return users_.size();
    } catch (const std::exception& e) {
        logError("userCount", e);
        return 0;
    }
}

// ============================================================
// auth
// ============================================================

bool Database::isAuth(std::int64_t chatId, std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureAuthLoaded(chatId);
        return auth_[chatId].count(userId) > 0;
    } catch (const std::exception& e) {
        logError("isAuth", e);
        return false;
    }
}
bool Database::addAuth(std::int64_t chatId, std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureAuthLoaded(chatId);
        auto& s = auth_[chatId];
        if (s.count(userId)) return true;
        Stmt st(db_, "INSERT OR IGNORE INTO auth(chat_id,user_id) VALUES(?,?);");
        st.bindInt(1, chatId);
        st.bindInt(2, userId);
        if (st.step() != SQLITE_DONE) throw DatabaseError(sqlite3_errmsg(db_));
        s.insert(userId);
        return true;
    } catch (const std::exception& e) {
        logError("addAuth", e);
        return false;
    }
}
bool Database::removeAuth(std::int64_t chatId, std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureAuthLoaded(chatId);
        auto& s = auth_[chatId];
        if (!s.count(userId)) return true;
        Stmt st(db_, "DELETE FROM auth WHERE chat_id=? AND user_id=?;");
        st.bindInt(1, chatId);
        st.bindInt(2, userId);
        if (st.step() != SQLITE_DONE) throw DatabaseError(sqlite3_errmsg(db_));
        s.erase(userId);
        return true;
    } catch (const std::exception& e) {
        logError("removeAuth", e);
        return false;
    }
}
std::vector<std::int64_t> Database::getAuthUsers(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureAuthLoaded(chatId);
        const auto& s = auth_[chatId];
        return std::vector<std::int64_t>(s.begin(), s.end());
    } catch (const std::exception& e) {
        logError("getAuthUsers", e);
        return {};
    }
}

// ============================================================
// language
// ============================================================

std::string Database::getLang(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        auto it = lang_.find(chatId);
        if (it != lang_.end()) return it->second;
        Stmt st(db_, "SELECT lang_code FROM lang WHERE chat_id=?;");
        st.bindInt(1, chatId);
        std::string code = defaultLang_;
        if (st.step() == SQLITE_ROW) code = st.colText(0);
        lang_[chatId] = code;
        return code;
    } catch (const std::exception& e) {
        logError("getLang", e);
        return defaultLang_;
    }
}
bool Database::setLang(std::int64_t chatId, const std::string& langCode) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        Stmt st(db_,
                "INSERT INTO lang(chat_id,lang_code) VALUES(?,?) "
                "ON CONFLICT(chat_id) DO UPDATE SET lang_code=excluded.lang_code;");
        st.bindInt(1, chatId);
        st.bindText(2, langCode);
        if (st.step() != SQLITE_DONE) throw DatabaseError(sqlite3_errmsg(db_));
        lang_[chatId] = langCode;
        return true;
    } catch (const std::exception& e) {
        logError("setLang", e);
        return false;
    }
}

// ============================================================
// assistant
// ============================================================

int Database::assignAssistantLocked(std::int64_t chatId) {
    int count = assistantCount_ < 1 ? 1 : assistantCount_;
    std::uniform_int_distribution<int> dist(1, count);
    int num = dist(rng_);
    try {
        Stmt st(db_,
                "INSERT INTO assistant(chat_id,num) VALUES(?,?) "
                "ON CONFLICT(chat_id) DO UPDATE SET num=excluded.num;");
        st.bindInt(1, chatId);
        st.bindInt(2, num);
        st.step();  // best effort; cache is authoritative for this session
    } catch (const std::exception& e) {
        logError("assignAssistant", e);
    }
    assistant_[chatId] = num;
    return num;
}
int Database::setAssistant(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    return assignAssistantLocked(chatId);
}
int Database::getAssistant(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        int num = 0;
        auto it = assistant_.find(chatId);
        if (it != assistant_.end()) {
            num = it->second;
        } else {
            Stmt st(db_, "SELECT num FROM assistant WHERE chat_id=?;");
            st.bindInt(1, chatId);
            if (st.step() == SQLITE_ROW) num = static_cast<int>(st.colInt(0));
        }
        int count = assistantCount_ < 1 ? 1 : assistantCount_;
        if (num < 1 || num > count) {
            num = assignAssistantLocked(chatId);
        } else {
            assistant_[chatId] = num;
        }
        return num;
    } catch (const std::exception& e) {
        logError("getAssistant", e);
        return assignAssistantLocked(chatId);
    }
}

// ============================================================
// blacklist
// ============================================================

bool Database::isBlacklistedChat(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureBlChatsLoaded();
        return blChats_.count(chatId) > 0;
    } catch (const std::exception& e) {
        logError("isBlacklistedChat", e);
        return false;
    }
}
bool Database::addBlacklistChat(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureBlChatsLoaded();
        if (blChats_.count(chatId)) return true;
        insertId("INSERT OR IGNORE INTO blacklist_chats(chat_id) VALUES(?);", chatId);
        blChats_.insert(chatId);
        return true;
    } catch (const std::exception& e) {
        logError("addBlacklistChat", e);
        return false;
    }
}
bool Database::removeBlacklistChat(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        deleteId("DELETE FROM blacklist_chats WHERE chat_id=?;", chatId);
        blChats_.erase(chatId);
        return true;
    } catch (const std::exception& e) {
        logError("removeBlacklistChat", e);
        return false;
    }
}
std::vector<std::int64_t> Database::getBlacklistedChats() {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureBlChatsLoaded();
        return std::vector<std::int64_t>(blChats_.begin(), blChats_.end());
    } catch (const std::exception& e) {
        logError("getBlacklistedChats", e);
        return {};
    }
}

bool Database::isBlacklistedUser(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureBlUsersLoaded();
        return blUsers_.count(userId) > 0;
    } catch (const std::exception& e) {
        logError("isBlacklistedUser", e);
        return false;
    }
}
bool Database::addBlacklistUser(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureBlUsersLoaded();
        if (blUsers_.count(userId)) return true;
        insertId("INSERT OR IGNORE INTO blacklist_users(user_id) VALUES(?);", userId);
        blUsers_.insert(userId);
        return true;
    } catch (const std::exception& e) {
        logError("addBlacklistUser", e);
        return false;
    }
}
bool Database::removeBlacklistUser(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        deleteId("DELETE FROM blacklist_users WHERE user_id=?;", userId);
        blUsers_.erase(userId);
        return true;
    } catch (const std::exception& e) {
        logError("removeBlacklistUser", e);
        return false;
    }
}
std::vector<std::int64_t> Database::getBlacklistedUsers() {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureBlUsersLoaded();
        return std::vector<std::int64_t>(blUsers_.begin(), blUsers_.end());
    } catch (const std::exception& e) {
        logError("getBlacklistedUsers", e);
        return {};
    }
}

bool Database::addBlacklist(std::int64_t id) {
    return id < 0 ? addBlacklistChat(id) : addBlacklistUser(id);
}
bool Database::removeBlacklist(std::int64_t id) {
    return id < 0 ? removeBlacklistChat(id) : removeBlacklistUser(id);
}

// ============================================================
// sudoers
// ============================================================

bool Database::isSudo(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureSudoLoaded();
        return sudoers_.count(userId) > 0;
    } catch (const std::exception& e) {
        logError("isSudo", e);
        return false;
    }
}
bool Database::addSudo(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureSudoLoaded();
        if (sudoers_.count(userId)) return true;
        insertId("INSERT OR IGNORE INTO sudoers(user_id) VALUES(?);", userId);
        sudoers_.insert(userId);
        return true;
    } catch (const std::exception& e) {
        logError("addSudo", e);
        return false;
    }
}
bool Database::removeSudo(std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        deleteId("DELETE FROM sudoers WHERE user_id=?;", userId);
        sudoers_.erase(userId);
        return true;
    } catch (const std::exception& e) {
        logError("removeSudo", e);
        return false;
    }
}
std::vector<std::int64_t> Database::getSudoers() {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        ensureSudoLoaded();
        return std::vector<std::int64_t>(sudoers_.begin(), sudoers_.end());
    } catch (const std::exception& e) {
        logError("getSudoers", e);
        return {};
    }
}

// ============================================================
// per-chat flags (cmd_delete, admin_play) — persistent, write-through
// ============================================================

bool Database::getChatFlagLocked(std::int64_t chatId, const char* selectSql,
                                 std::unordered_map<std::int64_t, bool>& cache) {
    auto it = cache.find(chatId);
    if (it != cache.end()) return it->second;
    Stmt st(db_, selectSql);
    st.bindInt(1, chatId);
    bool val = false;
    if (st.step() == SQLITE_ROW) val = st.colInt(0) != 0;
    cache[chatId] = val;
    return val;
}
bool Database::setChatFlagLocked(std::int64_t chatId, bool enabled, const char* upsertSql,
                                 std::unordered_map<std::int64_t, bool>& cache) {
    Stmt st(db_, upsertSql);
    st.bindInt(1, chatId);
    st.bindInt(2, enabled ? 1 : 0);
    if (st.step() != SQLITE_DONE) throw DatabaseError(sqlite3_errmsg(db_));
    cache[chatId] = enabled;
    if (chatsLoaded_) chats_.insert(chatId);  // upsert may have created the row
    return true;
}

bool Database::getCmdDelete(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        return getChatFlagLocked(
            chatId, "SELECT cmd_delete FROM chats WHERE chat_id=?;", cmdDelete_);
    } catch (const std::exception& e) {
        logError("getCmdDelete", e);
        return false;
    }
}
bool Database::setCmdDelete(std::int64_t chatId, bool enabled) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        return setChatFlagLocked(
            chatId, enabled,
            "INSERT INTO chats(chat_id,cmd_delete) VALUES(?,?) "
            "ON CONFLICT(chat_id) DO UPDATE SET cmd_delete=excluded.cmd_delete;",
            cmdDelete_);
    } catch (const std::exception& e) {
        logError("setCmdDelete", e);
        return false;
    }
}
bool Database::getPlayMode(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        return getChatFlagLocked(
            chatId, "SELECT admin_play FROM chats WHERE chat_id=?;", adminPlay_);
    } catch (const std::exception& e) {
        logError("getPlayMode", e);
        return false;
    }
}
bool Database::setPlayMode(std::int64_t chatId, bool enabled) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        return setChatFlagLocked(
            chatId, enabled,
            "INSERT INTO chats(chat_id,admin_play) VALUES(?,?) "
            "ON CONFLICT(chat_id) DO UPDATE SET admin_play=excluded.admin_play;",
            adminPlay_);
    } catch (const std::exception& e) {
        logError("setPlayMode", e);
        return false;
    }
}

// ============================================================
// Global settings (persistent key/value)
// ============================================================

std::string Database::getSetting(const std::string& key,
                                 const std::string& fallback) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = settings_.find(key);
    if (it != settings_.end())
        return it->second;
    try {
        Stmt st(db_, "SELECT value FROM settings WHERE key=?;");
        st.bindText(1, key);
        std::string val = fallback;
        if (st.step() == SQLITE_ROW)
            val = st.colText(0);
        settings_[key] = val;
        return val;
    } catch (const std::exception& e) {
        logError("getSetting", e);
        return fallback;
    }
}

bool Database::setSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mtx_);
    try {
        Stmt st(db_,
                "INSERT INTO settings(key,value) VALUES(?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
        st.bindText(1, key);
        st.bindText(2, value);
        if (st.step() != SQLITE_DONE)
            throw DatabaseError(sqlite3_errmsg(db_));
        settings_[key] = value;
        return true;
    } catch (const std::exception& e) {
        logError("setSetting", e);
        return false;
    }
}

// The log-group toggle (/logger on|off). Defaults to enabled, matching the
// Python bot, which posts to LOGGER_ID unless the owner turns it off.
bool Database::getLoggerEnabled() { return getSetting("logger", "1") != "0"; }

bool Database::setLoggerEnabled(bool enabled) {
    return setSetting("logger", enabled ? "1" : "0");
}

}  // namespace anonx
