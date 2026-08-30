// AnonXMusic C++ port — Phase 1 data layer
// database.hpp
//
// Lightweight persistence layer built directly on the SQLite C API
// (<sqlite3.h>). Replaces the MongoDB layer from the original Python bot.
//
// Design goals (from the project spec):
//   * Embedded, single-file DB (no server) -> SQLite, WAL mode.
//     The .db file can be continuously replicated to Cloudflare R2 with
//     Litestream out-of-process; from C++ it behaves like a normal local DB.
//   * Write-through in-memory cache:
//       - reads hit RAM first (unordered_map / unordered_set);
//       - on a miss the row is lazily loaded from SQLite and cached;
//       - writes update both the cache and SQLite.
//   * Thread-safe: a single mutex serialises every cache + DB access, because
//     multiple assistant accounts run on separate threads.
//   * RAII: the sqlite3* handle is opened in the constructor and closed in the
//     destructor. Setup failures throw DatabaseError; per-operation failures
//     are caught internally and reported via return codes (bool / defaults).
//
// Ephemeral state (active_calls, loop) is intentionally NOT here — see
// cache_manager.hpp.

#pragma once

#include <cstdint>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Opaque SQLite handles (defined by <sqlite3.h> in the .cpp).
struct sqlite3;
struct sqlite3_stmt;

namespace anonx {

// Thrown only for unrecoverable setup failures (open / pragma / schema).
// Normal per-call failures are reported through return values, not exceptions.
class DatabaseError : public std::runtime_error {
public:
    explicit DatabaseError(const std::string& msg) : std::runtime_error(msg) {}
};

class Database {
public:
    // Open (or create) the SQLite file at `path`, enable WAL, create the
    // schema. Throws DatabaseError if the database cannot be initialised.
    explicit Database(const std::string& path);
    ~Database();

    // Non-copyable, non-movable (owns a raw sqlite3* and a mutex).
    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&)                 = delete;
    Database& operator=(Database&&)      = delete;

    // ---- Global configuration (set once at boot, before serving) ----

    // Language returned by getLang() when a chat has none set. Default "en".
    void setDefaultLang(const std::string& code);

    // Number of assistant accounts available (>= 1). Used to pick / validate
    // per-chat assistant numbers. Default 1.
    void setAssistantCount(int count);

    // ==================== chats (persistent) ====================
    bool                     isChat(std::int64_t chatId);
    bool                     addChat(std::int64_t chatId);
    bool                     removeChat(std::int64_t chatId);
    std::vector<std::int64_t> getChats();
    std::size_t              chatCount();

    // ==================== users (persistent) ====================
    bool                     isUser(std::int64_t userId);
    bool                     addUser(std::int64_t userId);
    bool                     removeUser(std::int64_t userId);
    std::vector<std::int64_t> getUsers();
    std::size_t              userCount();

    // ============ auth: per-chat authorized users =============
    bool                     isAuth(std::int64_t chatId, std::int64_t userId);
    bool                     addAuth(std::int64_t chatId, std::int64_t userId);
    bool                     removeAuth(std::int64_t chatId, std::int64_t userId);
    std::vector<std::int64_t> getAuthUsers(std::int64_t chatId);

    // ============ language (per-chat, persistent) =============
    std::string getLang(std::int64_t chatId);         // returns default if unset
    bool        setLang(std::int64_t chatId, const std::string& langCode);

    // ==== assistant: per-chat account number in [1, assistantCount] ====
    // getAssistant assigns a random valid number (and persists it) if the chat
    // has none yet, or if the stored number is out of range.
    int getAssistant(std::int64_t chatId);
    int setAssistant(std::int64_t chatId);            // force (re)assign, returns number

    // ==================== blacklist (persistent) ====================
    bool                     isBlacklistedChat(std::int64_t chatId);
    bool                     addBlacklistChat(std::int64_t chatId);
    bool                     removeBlacklistChat(std::int64_t chatId);
    std::vector<std::int64_t> getBlacklistedChats();

    bool                     isBlacklistedUser(std::int64_t userId);
    bool                     addBlacklistUser(std::int64_t userId);
    bool                     removeBlacklistUser(std::int64_t userId);
    std::vector<std::int64_t> getBlacklistedUsers();

    // Convenience routing by id sign (chat ids are negative), matching the
    // original add_blacklist/del_blacklist behaviour.
    bool addBlacklist(std::int64_t id);
    bool removeBlacklist(std::int64_t id);

    // ==================== sudoers (persistent) ====================
    bool                     isSudo(std::int64_t userId);
    bool                     addSudo(std::int64_t userId);
    bool                     removeSudo(std::int64_t userId);
    std::vector<std::int64_t> getSudoers();

    // ========= per-chat flags (persistent, write-through) =========
    bool getCmdDelete(std::int64_t chatId);
    bool setCmdDelete(std::int64_t chatId, bool enabled);

    bool getPlayMode(std::int64_t chatId);            // admin_play
    bool setPlayMode(std::int64_t chatId, bool enabled);

    // ============ global settings (persistent key/value) ============
    // A tiny key/value table for bot-wide switches that must survive a restart
    // (the Python bot kept these as single documents in MongoDB). Values are
    // cached in RAM after the first read, like every other accessor here.
    std::string getSetting(const std::string& key, const std::string& fallback = "");
    bool        setSetting(const std::string& key, const std::string& value);

    // Log-group posting (/logger on|off). Defaults to enabled.
    bool getLoggerEnabled();
    bool setLoggerEnabled(bool enabled);

private:
    // --- low-level helpers; all assume mtx_ is already held ---
    void execOrThrow(const char* sql);                        // schema / pragmas
    void loadIdSet(const char* sql, std::unordered_set<std::int64_t>& out);
    bool insertId(const char* sql, std::int64_t id);          // single-param write
    bool deleteId(const char* sql, std::int64_t id);

    void ensureChatsLoaded();
    void ensureUsersLoaded();
    void ensureAuthLoaded(std::int64_t chatId);
    void ensureBlChatsLoaded();
    void ensureBlUsersLoaded();
    void ensureSudoLoaded();

    int  assignAssistantLocked(std::int64_t chatId);          // random assign + persist
    bool getChatFlagLocked(std::int64_t chatId, const char* selectSql,
                           std::unordered_map<std::int64_t, bool>& cache);
    bool setChatFlagLocked(std::int64_t chatId, bool enabled, const char* upsertSql,
                           std::unordered_map<std::int64_t, bool>& cache);

    sqlite3*   db_ = nullptr;
    std::mutex mtx_;

    // ---- in-memory caches ----
    std::unordered_set<std::int64_t> chats_;    bool chatsLoaded_   = false;
    std::unordered_set<std::int64_t> users_;    bool usersLoaded_   = false;
    // For auth, presence of a chat key means "this chat's set is loaded".
    std::unordered_map<std::int64_t, std::unordered_set<std::int64_t>> auth_;
    std::unordered_map<std::int64_t, std::string> lang_;
    std::unordered_map<std::int64_t, int>         assistant_;
    std::unordered_set<std::int64_t> blChats_;   bool blChatsLoaded_ = false;
    std::unordered_set<std::int64_t> blUsers_;   bool blUsersLoaded_ = false;
    std::unordered_set<std::int64_t> sudoers_;   bool sudoLoaded_    = false;
    std::unordered_map<std::int64_t, bool> cmdDelete_;
    std::unordered_map<std::int64_t, bool> adminPlay_;
    std::unordered_map<std::string, std::string> settings_;

    std::string  defaultLang_    = "en";
    int          assistantCount_ = 1;
    std::mt19937 rng_;
};

}  // namespace anonx
