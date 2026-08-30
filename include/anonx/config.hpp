// AnonXMusic C++ port — Phase 2
// config.hpp — runtime configuration, ported from the original Python config.py.
//
// Values are read from the process environment. If a `.env` file exists it is
// parsed first, but real environment variables take precedence (matching
// python-dotenv's default `override=False`).
//
// Two deliberate changes vs. the Python original:
//   * MONGO_URL is dropped — the C++ port persists to SQLite (see db_path).
//   * DB_PATH is added      — filesystem path to the SQLite database file.
//
// One addition forced by the client library:
//   * PHONE_NUMBER / PHONE_NUMBER2 / PHONE_NUMBER3. The SESSION values are
//     Pyrogram session strings and TDLib cannot import them, so each assistant
//     needs a one-time TDLib-native phone login. SESSION stays required (it is
//     what says "this assistant slot is configured", exactly as in config.py);
//     the matching PHONE_NUMBER is what the login actually uses.

#ifndef ANONX_CONFIG_HPP
#define ANONX_CONFIG_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace anonx {

// Thrown when required configuration is missing or unparseable.
// Mirrors the original config.py `raise SystemExit(...)` on missing env vars.
class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& msg) : std::runtime_error(msg) {}
};

class Config {
public:
    // ---- credentials / identity ----
    std::int64_t api_id = 0;       // API_ID     (required)
    std::string  api_hash;         // API_HASH   (required)
    std::string  bot_token;        // BOT_TOKEN  (required)
    std::int64_t logger_id = 0;    // LOGGER_ID  (required)
    std::int64_t owner_id = 0;     // OWNER_ID   (required)
    std::string  session1;         // SESSION    (required — first assistant)
    std::string  session2;         // SESSION2   (optional)
    std::string  session3;         // SESSION3   (optional)

    // Assistant phone numbers for the one-time TDLib login (see the header note).
    std::string  phone1;           // PHONE_NUMBER
    std::string  phone2;           // PHONE_NUMBER2
    std::string  phone3;           // PHONE_NUMBER3

    // ---- storage (replaces MONGO_URL) ----
    std::string db_path = "anonx.db";   // DB_PATH — SQLite database file

    // ---- limits ----
    // config.py stores DURATION_LIMIT already multiplied by 60 (seconds).
    int duration_limit_seconds = 60 * 60;   // DURATION_LIMIT (minutes) * 60
    int queue_limit = 20;                    // QUEUE_LIMIT
    int playlist_limit = 20;                 // PLAYLIST_LIMIT

    // ---- links ----
    std::string support_channel = "https://t.me/fallenx";        // SUPPORT_CHANNEL
    std::string support_chat    = "https://t.me/DevilsHeavenMF"; // SUPPORT_CHAT

    // ---- feature flags ----
    bool auto_leave = false;   // AUTO_LEAVE
    bool auto_end   = false;   // AUTO_END
    bool thumb_gen  = true;    // THUMB_GEN
    bool video_play = true;    // VIDEO_PLAY

    // ---- localisation ----
    std::string lang_code = "en";   // LANG_CODE

    // ---- media / assets ----
    std::vector<std::string> cookies_url;   // COOKIES_URL — batbin.me links only
    std::string default_thumb = "https://te.legra.ph/file/3e40a408286d4eda24191.jpg";
    std::string ping_img      = "https://files.catbox.moe/haagg2.png";
    std::string start_img     = "https://files.catbox.moe/zvziwk.jpg";

    // Build a Config from the environment, optionally seeded by a `.env` file.
    // Does NOT validate — call check() afterwards. `envFile` may be empty/missing
    // (in which case only the real environment is consulted).
    static Config load(const std::string& envFile = ".env");

    // Throw ConfigError if any required field is missing. Mirrors config.check().
    void check() const;

    // Number of configured assistant userbots (non-empty SESSION values), 1..3.
    // Used to seed Database::setAssistantCount(). Never returns less than 1.
    int assistantCount() const;

    // The phone numbers of the configured assistant slots, in order, skipping
    // slots whose PHONE_NUMBER is unset. Empty when none is configured, which is
    // why Runtime treats "no assistants" as non-fatal: the bot half still runs.
    std::vector<std::string> assistantPhones() const;

    // A human-readable, SECRET-FREE summary for logging at boot.
    // Never includes api_hash, bot_token, or any session string.
    std::string redactedSummary() const;
};

}  // namespace anonx

#endif  // ANONX_CONFIG_HPP
