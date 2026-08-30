// AnonXMusic C++ port — Phase 6b (admin & menu commands)
// admin_plugins.hpp — everything that is not playback.
//
// WHAT THIS PORTS
// The Python bot spreads these over anony/plugins/tools/* and
// anony/plugins/sudo/*; grouping them in one class keeps the wiring in
// plugins_router.cpp uniform and lets one demo drive the whole set:
//
//   auth.py        /auth /unauth /authlist       per-chat authorized users
//   blacklist.py   /blacklist /unblacklist       chat & user blacklist (sudo)
//   broadcast.py   /gcast /broadcast             mass copy/forward (sudo)
//   sudoers.py     /addsudo /rmsudo /sudolist    sudo list (owner / sudo)
//   language.py    /lang + the language menu     per-chat language
//   ping.py        /ping                         latency + host metrics
//   stats.py       /stats                        counters (+ sudo-only block)
//   start.py       /start /help /settings        the inline menus
//   logger.py      /logger on|off                log-group switch (sudo)
//   activevc.py    /ac /activevc                 active streams (sudo)
//   chat_watcher   (no command)                  serve + log new chats/users
//
// DESIGN — same three rules as Phase 6a, so the two halves compose:
//   * Handlers take transport-free events (CommandEvent / ButtonEvent), so this
//     translation unit links without TDLib; only plugins_router.cpp knows the
//     Dispatcher.
//   * Permission decisions come from guards.cpp (pure predicates) and every
//     user-visible string from the locale files — no literal English here.
//   * Host metrics arrive through SystemInfo, whose getters are virtual, so the
//     /ping and /stats cards are asserted byte for byte against FakeSystemInfo.
//
// DELIBERATE OMISSIONS (documented rather than silently skipped):
//   * /eval — arbitrary code execution, and there is no C++ interpreter to run
//     the snippet in. The locale keys (eval_*) stay unused.
//   * /logs, /restart — process-level operations that belong with the launcher,
//     not the command layer (log_fetch / restarting keys stay unused).
//   * /reload (admin cache) — Phase 6a resolves admin status live through
//     BotApi::getChatMemberStatus, so there is no cache to refresh yet
//     (admin_cache_* keys stay unused).
//   * The three version lines of `stats_sudo` still carry the Python labels
//     ("Python/Pyrogram/PyTgCalls") because the locale files are kept identical
//     to the original; the VALUES are this port's equivalents, which is why
//     SystemInfo names them toolchain/telegram/voice.

#ifndef ANONX_ADMIN_PLUGINS_HPP
#define ANONX_ADMIN_PLUGINS_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "anonx/bot_api.hpp"
#include "anonx/buttons.hpp"
#include "anonx/cache_manager.hpp"
#include "anonx/call_manager.hpp"
#include "anonx/config.hpp"
#include "anonx/database.hpp"
#include "anonx/lang.hpp"
#include "anonx/plugins.hpp"   // CommandEvent / ButtonEvent
#include "anonx/sysinfo.hpp"

namespace anonx {

class AdminPlugins {
public:
    struct Deps {
        BotApi&         api;
        Database&       db;
        CacheManager&   cache;
        CallManager&    calls;      // only for ping() — the voice-side latency
        SystemInfo&       sys;
        ThumbnailRenderer& thumb;
        const Language&   lang;
        const Config&     config;
    };

    explicit AdminPlugins(const Deps& deps);

    // ---- auth -------------------------------------------------------------
    void onAuth(const CommandEvent& ev);      // /auth /unauth  (admin)
    void onAuthList(const CommandEvent& ev);  // /authlist

    // ---- blacklist (sudo) -------------------------------------------------
    void onBlacklist(const CommandEvent& ev); // /blacklist /unblacklist /whitelist

    // ---- broadcast (sudo) -------------------------------------------------
    // /gcast /broadcast [-nochat] [-user] [-copy], addressed by reply.
    void onGcast(const CommandEvent& ev);

    // ---- sudoers ----------------------------------------------------------
    void onSudo(const CommandEvent& ev);      // /addsudo /rmsudo  (owner)
    void onSudoList(const CommandEvent& ev);  // /sudolist

    // ---- language ---------------------------------------------------------
    void onLang(const CommandEvent& ev);      // /lang  (admin)

    // ---- info -------------------------------------------------------------
    void onPing(const CommandEvent& ev);      // /ping
    void onStats(const CommandEvent& ev);     // /stats
    void onActiveVc(const CommandEvent& ev);  // /ac /activevc  (sudo)

    // ---- menus ------------------------------------------------------------
    void onStart(const CommandEvent& ev);     // /start
    void onHelp(const CommandEvent& ev);      // /help
    void onSettings(const CommandEvent& ev);  // /settings  (admin, groups)

    // ---- logger (sudo) ----------------------------------------------------
    void onLogger(const CommandEvent& ev);    // /logger on|off

    // Runs for EVERY message (see Dispatcher::onEveryMessage): registers a chat
    // or user the first time it is seen and posts the notice to the log group.
    void onSeen(const CommandEvent& ev);

    // Inline buttons for every menu payload documented in buttons.hpp
    // ("help …", "lang …", "settings …", "start menu", "close").
    void onMenu(const ButtonEvent& ev);

    // ---- command names (shared by the router and the tests) ---------------
    static std::vector<std::string> authCommands();       // auth, unauth
    static std::vector<std::string> authListCommands();
    static std::vector<std::string> blacklistCommands();  // blacklist, unblacklist…
    static std::vector<std::string> gcastCommands();
    static std::vector<std::string> sudoCommands();       // addsudo, rmsudo
    static std::vector<std::string> sudoListCommands();
    static std::vector<std::string> langCommands();
    static std::vector<std::string> pingCommands();
    static std::vector<std::string> statsCommands();
    static std::vector<std::string> activeVcCommands();   // ac, activevc
    static std::vector<std::string> startCommands();
    static std::vector<std::string> helpCommands();
    static std::vector<std::string> settingsCommands();
    static std::vector<std::string> loggerCommands();

    // Every group above, in registration order. `moduleCount()` derives the
    // "Modules:" figure of the stats card from it, so the number can never drift
    // away from what is actually wired up.
    static std::vector<std::vector<std::string>> allCommandGroups();
    static int moduleCount();

    // The nine help topics, as {label key, body key} pairs — help_0…help_8 label
    // the buttons, help_admins…help_sudo hold the pages.
    static const std::vector<std::pair<std::string, std::string>>& helpTopics();

private:
    LangView tr(std::int64_t chatId) const;

    // Labels for the keyboards, filled from one LangView.
    buttons::MenuText menuText(const LangView& L) const;

    // Menu bodies + keyboards, shared by the commands and their callbacks.
    std::string startCard(const LangView& L, const CommandEvent& ev) const;
    std::string helpBody(const LangView& L) const;
    std::string settingsCard(const LangView& L) const;
    InlineKeyboard startKeyboard(const LangView& L, bool isPrivate) const;
    InlineKeyboard helpKeyboard(const LangView& L) const;
    InlineKeyboard settingsKeyboard(const LangView& L, std::int64_t chatId) const;
    InlineKeyboard languageKeyboard(const LangView& L) const;

    // "https://t.me/<bot>?startgroup=true", or "" when the username is unknown.
    std::string addMeUrl() const;

    // Status-message handoff, identical to Plugins: "Fetching stats…" becomes the
    // finished card by editing the same message.
    void         setStatus(std::int64_t chatId, std::int64_t messageId);
    std::int64_t takeStatus(std::int64_t chatId);
    std::int64_t say(std::int64_t chatId, const std::string& html,
                     const InlineKeyboard& kb = {});
    std::int64_t sayPhoto(std::int64_t chatId, const std::string& photoPath,
                          const std::string& captionHtml,
                          const InlineKeyboard& kb = {});

    // Resolve the user a command is aimed at: the sender of the replied-to
    // message, else a numeric argument. Returns 0 when neither is present.
    std::int64_t resolveTarget(const CommandEvent& ev) const;

    // Post `html` to LOGGER_ID, if a log group is configured AND the logger is
    // switched on. Returns true when the message was sent.
    bool toLogGroup(const std::string& html);

    // Can `userId` change this chat's settings? (private chats: always).
    bool mayConfigure(const CommandEvent& ev) const;

    BotApi&         api_;
    Database&       db_;
    CacheManager&   cache_;
    CallManager&  calls_;
    SystemInfo&   sys_;
    ThumbnailRenderer& thumb_;
    const Language& lang_;
    const Config& config_;

    std::atomic<bool> broadcasting_{false};   // one /gcast at a time

    mutable std::mutex                   mutex_;
    std::map<std::int64_t, std::int64_t>  status_;   // chatId -> pending message
};

}  // namespace anonx

#endif  // ANONX_ADMIN_PLUGINS_HPP
