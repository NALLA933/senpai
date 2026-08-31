// AnonXMusic C++ port — Phase 2
// config.cpp — implementation of Config: .env parsing + environment loading.

#include "anonx/config.hpp"

#include "anonx/utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace anonx {
namespace {

// Trim leading/trailing ASCII whitespace, including a trailing '\r' left behind
// by Windows (CRLF) line endings.
std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (b < e && isws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Strip a single pair of matching surrounding quotes (' or "), if present.
// The value's interior is left untouched — important for session strings, whose
// base64 padding contains '=' characters we must preserve.
std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2) {
        char f = s.front();
        char l = s.back();
        if ((f == '"' && l == '"') || (f == '\'' && l == '\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}



// Parse a .env file into a key->value map. Rules:
//   * blank lines and lines whose first non-space char is '#' are ignored
//   * an optional leading "export " is stripped
//   * the line is split on the FIRST '=' only (so '=' in the value survives)
//   * key and value are trimmed; the value then has one layer of quotes removed
std::unordered_map<std::string, std::string> parseEnvFile(const std::string& path) {
    std::unordered_map<std::string, std::string> out;
    if (path.empty()) return out;

    std::ifstream in(path);
    if (!in) return out;  // missing .env is not an error

    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        if (t.rfind("export ", 0) == 0) {  // starts with "export "
            t = trim(t.substr(7));
        }

        std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;  // not a KEY=VALUE line

        std::string key = trim(t.substr(0, eq));
        if (key.empty()) continue;

        std::string val = stripQuotes(trim(t.substr(eq + 1)));
        out[key] = val;
    }
    return out;
}

// Look up a key: real environment wins, then the .env map, then the default.
// (Matches python-dotenv load_dotenv(override=False): pre-existing env vars are
// not overwritten by the file.)
class EnvSource {
public:
    explicit EnvSource(std::unordered_map<std::string, std::string> dotenv)
        : dotenv_(std::move(dotenv)) {}

    std::string str(const char* key, const std::string& def = "") const {
        if (const char* e = std::getenv(key)) return std::string(e);
        auto it = dotenv_.find(key);
        if (it != dotenv_.end()) return it->second;
        return def;
    }

    // Robust integer read: empty/unparseable falls back to `def` (unlike Python's
    // int("") which would throw).
    std::int64_t integer(const char* key, std::int64_t def) const {
        std::string v = str(key);
        if (v.empty()) return def;
        try {
            std::size_t pos = 0;
            long long parsed = std::stoll(v, &pos);
            return static_cast<std::int64_t>(parsed);
        } catch (...) {
            return def;
        }
    }

    // Mirrors config.py's `getenv(...).lower() == "true"`.
    bool boolean(const char* key, bool def) const {
        std::string v = str(key);
        if (v.empty()) return def;
        return anonx::utils::toLower(v) == "true";
    }

private:
    std::unordered_map<std::string, std::string> dotenv_;
};

}  // namespace

Config Config::load(const std::string& envFile) {
    EnvSource env(parseEnvFile(envFile));
    Config c;

    c.api_id    = env.integer("API_ID", 0);
    c.api_hash  = env.str("API_HASH");
    c.bot_token = env.str("BOT_TOKEN");
    c.logger_id = env.integer("LOGGER_ID", 0);
    c.owner_id  = env.integer("OWNER_ID", 0);

    c.session1 = env.str("SESSION");
    c.session2 = env.str("SESSION2");
    c.session3 = env.str("SESSION3");

    // TDLib cannot import a Pyrogram SESSION string, so each assistant slot also
    // carries the phone number its one-time login needs.
    c.phone1 = env.str("PHONE_NUMBER");
    c.phone2 = env.str("PHONE_NUMBER2");
    c.phone3 = env.str("PHONE_NUMBER3");

    // MONGO_URL -> DB_PATH for the SQLite port.
    c.db_path = env.str("DB_PATH", "anonx.db");

    // COOKIES_URL: space-separated; keep only batbin.me links
    {
        std::string raw = env.str("COOKIES_URL");
        if (!raw.empty()) {
            c.cookies_url.clear(); // clear default if env is provided
            std::istringstream iss(raw);
            std::string tok;
            while (iss >> tok) {
                if (tok.find("batbin.me") != std::string::npos) {
                    c.cookies_url.push_back(tok);
                }
            }
        }
    }

    // ---- limits ----
    c.duration_limit_seconds = static_cast<int>(env.integer("DURATION_LIMIT", 60)) * 60;
    c.queue_limit            = static_cast<int>(env.integer("QUEUE_LIMIT", 20));
    c.playlist_limit         = static_cast<int>(env.integer("PLAYLIST_LIMIT", 20));

    // ---- links ----
    c.support_channel = env.str("SUPPORT_CHANNEL", "https://t.me/fallenx");
    c.support_chat    = env.str("SUPPORT_CHAT", "https://t.me/DevilsHeavenMF");

    // ---- feature flags ----
    c.auto_leave = env.boolean("AUTO_LEAVE", false);
    c.auto_end   = env.boolean("AUTO_END", false);
    c.thumb_gen  = env.boolean("THUMB_GEN", true);
    c.video_play = env.boolean("VIDEO_PLAY", true);

    // ---- localisation ----
    c.lang_code = env.str("LANG_CODE", "en");

    // ---- media / assets ----
    c.default_thumb = env.str("DEFAULT_THUMB", "https://te.legra.ph/file/3e40a408286d4eda24191.jpg");
    c.ping_img      = env.str("PING_IMG", "https://files.catbox.moe/haagg2.png");
    c.start_img     = env.str("START_IMG", "https://files.catbox.moe/zvziwk.jpg");

    return c;
}

void Config::check() const {
    std::vector<std::string> missing;
    if (api_id == 0)        missing.push_back("API_ID");
    if (api_hash.empty())   missing.push_back("API_HASH");
    if (bot_token.empty())  missing.push_back("BOT_TOKEN");
    if (logger_id == 0)     missing.push_back("LOGGER_ID");
    if (owner_id == 0)      missing.push_back("OWNER_ID");
    if (session1.empty())   missing.push_back("SESSION");

    // Enforce that if a session string is provided (or not provided but phone is), they match.
    // Assistant 1
    if (!session1.empty() && phone1.empty()) missing.push_back("PHONE_NUMBER (required when SESSION is set)");
    if (session1.empty() && !phone1.empty()) missing.push_back("SESSION (required when PHONE_NUMBER is set)");
    
    // Assistant 2
    if (!session2.empty() && phone2.empty()) missing.push_back("PHONE_NUMBER2 (required when SESSION2 is set)");
    if (session2.empty() && !phone2.empty()) missing.push_back("SESSION2 (required when PHONE_NUMBER2 is set)");

    // Assistant 3
    if (!session3.empty() && phone3.empty()) missing.push_back("PHONE_NUMBER3 (required when SESSION3 is set)");
    if (session3.empty() && !phone3.empty()) missing.push_back("SESSION3 (required when PHONE_NUMBER3 is set)");

    if (!missing.empty()) {
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i) list += ", ";
            list += missing[i];
        }
        throw ConfigError("Missing required environment variables/consistency: " + list);
    }
}

int Config::assistantCount() const {
    int n = 0;
    if (!session1.empty()) ++n;
    if (!session2.empty()) ++n;
    if (!session3.empty()) ++n;
    return n > 0 ? n : 1;
}

std::vector<std::string> Config::assistantPhones() const {
    std::vector<std::string> out;
    // Slot order matters: assistant 1 is the one Database::assistant() hands out
    // first, so its phone must stay in that position.
    for (const std::string* p : {&phone1, &phone2, &phone3}) {
        if (!p->empty()) out.push_back(*p);
    }
    return out;
}

std::string Config::redactedSummary() const {
    auto yn = [](bool b) { return b ? "yes" : "no"; };
    std::ostringstream os;
    os << "config: owner_id=" << owner_id
       << " logger_id=" << logger_id
       << " lang=" << lang_code
       << " assistants=" << assistantCount()
       << " db=" << db_path
       << " duration_limit=" << (duration_limit_seconds / 60) << "min"
       << " queue_limit=" << queue_limit
       << " playlist_limit=" << playlist_limit
       << " auto_leave=" << yn(auto_leave)
       << " auto_end=" << yn(auto_end)
       << " thumb_gen=" << yn(thumb_gen)
       << " video_play=" << yn(video_play)
       << " cookies=" << cookies_url.size()
       << " | secrets set: api_hash=" << yn(!api_hash.empty())
       << " bot_token=" << yn(!bot_token.empty())
       << " session=" << yn(!session1.empty())
       // Count only — a phone number is personal data and never gets logged.
       << " phones=" << assistantPhones().size();
    return os.str();
}

}  // namespace anonx
