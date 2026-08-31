// AnonXMusic C++ port — Phase 6b (admin & menu commands)
// admin_plugins.cpp — see admin_plugins.hpp for the mapping to the Python files.
//
// Like plugins.cpp this file is only control flow: strings come from the locale
// tables, keyboards from buttons.cpp, permissions from guards.cpp and host
// metrics from SystemInfo.

#include "anonx/admin_plugins.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

#include "anonx/guards.hpp"

#include "anonx/utils.hpp"

namespace anonx {
namespace {

// Does `tokens` (a command's arguments) contain `flag`, case-insensitively?
bool hasFlag(const std::vector<std::string>& tokens, const std::string& flag) {
    for (std::size_t i = 1; i < tokens.size(); ++i)
        if (anonx::utils::toLower(tokens[i]) == flag)
            return true;
    return false;
}

// Shown where the Python card interpolates `user.username` and there is none.
const char kNoUsername[] = "-";

// The upstream project this port follows, for the start card's Source button.
const char kSourceUrl[] = "https://github.com/AnonymousX1025/AnonXMusic";

}  // namespace

AdminPlugins::AdminPlugins(const Deps& deps)
    : api_(deps.api),
      db_(deps.db),
      cache_(deps.cache),
      calls_(deps.calls),
      sys_(deps.sys),
      thumb_(deps.thumb),
      lang_(deps.lang),
      config_(deps.config) {}

// ---------------------------------------------------------------------------
// command names
// ---------------------------------------------------------------------------

std::vector<std::string> AdminPlugins::authCommands() { return {"auth", "unauth"}; }
std::vector<std::string> AdminPlugins::authListCommands() { return {"authlist"}; }
std::vector<std::string> AdminPlugins::blacklistCommands() {
    return {"blacklist", "unblacklist", "whitelist"};
}
std::vector<std::string> AdminPlugins::gcastCommands() { return {"gcast", "broadcast"}; }
std::vector<std::string> AdminPlugins::sudoCommands() { return {"addsudo", "rmsudo"}; }
std::vector<std::string> AdminPlugins::sudoListCommands() { return {"sudolist"}; }
std::vector<std::string> AdminPlugins::langCommands() { return {"lang", "language"}; }
std::vector<std::string> AdminPlugins::pingCommands() { return {"ping", "alive"}; }
std::vector<std::string> AdminPlugins::statsCommands() { return {"stats", "gstats"}; }
std::vector<std::string> AdminPlugins::activeVcCommands() { return {"ac", "activevc"}; }
std::vector<std::string> AdminPlugins::startCommands() { return {"start"}; }
std::vector<std::string> AdminPlugins::helpCommands() { return {"help"}; }
std::vector<std::string> AdminPlugins::settingsCommands() { return {"settings"}; }
std::vector<std::string> AdminPlugins::loggerCommands() { return {"logger"}; }

std::vector<std::vector<std::string>> AdminPlugins::allCommandGroups() {
    return {authCommands(),     authListCommands(), blacklistCommands(),
            gcastCommands(),    sudoCommands(),     sudoListCommands(),
            langCommands(),     pingCommands(),     statsCommands(),
            activeVcCommands(), startCommands(),    helpCommands(),
            settingsCommands(), loggerCommands()};
}

int AdminPlugins::moduleCount() {
    // The Python card counted loaded plugin files. The port's analogue is the
    // number of command groups it registers: the eight playback groups of
    // Plugins plus AdminPlugins' own, so the figure follows the code.
    const int playback = 8;
    return playback + static_cast<int>(allCommandGroups().size());
}

const std::vector<std::pair<std::string, std::string>>& AdminPlugins::helpTopics() {
    // Button label key, page body key — the order fixes the "help <n>" payloads.
    static const std::vector<std::pair<std::string, std::string>> topics = {
        {"help_0", "help_admins"}, {"help_1", "help_auth"},  {"help_2", "help_blist"},
        {"help_3", "help_lang"},   {"help_4", "help_ping"},  {"help_5", "help_play"},
        {"help_6", "help_queue"},  {"help_7", "help_stats"}, {"help_8", "help_sudo"},
    };
    return topics;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

LangView AdminPlugins::tr(std::int64_t chatId) const {
    return lang_.view(db_.getLang(chatId));
}

void AdminPlugins::setStatus(std::int64_t chatId, std::int64_t messageId) {
    if (messageId == 0)
        return;
    std::lock_guard<std::mutex> lk(mutex_);
    status_[chatId] = messageId;
}

std::int64_t AdminPlugins::takeStatus(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = status_.find(chatId);
    if (it == status_.end())
        return 0;
    const std::int64_t id = it->second;
    status_.erase(it);
    return id;
}

std::int64_t AdminPlugins::say(std::int64_t chatId, const std::string& html,
                               const InlineKeyboard& kb) {
    const std::int64_t pending = takeStatus(chatId);
    if (pending != 0 && api_.editMessageText(chatId, pending, html, kb))
        return pending;
    return api_.sendMessage(chatId, html, kb);
}

std::int64_t AdminPlugins::sayPhoto(std::int64_t chatId, const std::string& photoPath,
                                    const std::string& captionHtml,
                                    const InlineKeyboard& kb) {
    const std::int64_t pending = takeStatus(chatId);
    if (pending != 0) {
        if (api_.editMessageMedia(chatId, pending, photoPath, captionHtml, kb))
            return pending;
        api_.deleteMessage(chatId, pending);
    }
    return api_.sendPhoto(chatId, photoPath, captionHtml, kb);
}

std::int64_t AdminPlugins::resolveTarget(const CommandEvent& ev) const {
    if (ev.hasReply()) {
        const std::int64_t sender = api_.getMessageSenderId(ev.chatId, ev.replyToMessageId);
        if (sender != 0)
            return sender;
    }
    if (ev.command.size() >= 2) {
        std::int64_t id = 0;
        if (anonx::utils::parseI64(ev.command[1], id) && id > 0)
            return id;
    }
    return 0;
}

bool AdminPlugins::toLogGroup(const std::string& html) {
    if (config_.logger_id == 0 || !db_.getLoggerEnabled())
        return false;
    return api_.sendMessage(config_.logger_id, html) != 0;
}

bool AdminPlugins::mayConfigure(const CommandEvent& ev) const {
    return guards::adminCheck(api_, db_, ev.isPrivate, ev.chatId, ev.fromUserId) ||
           guards::isSudo(db_, config_, ev.fromUserId);
}

std::string AdminPlugins::addMeUrl() const {
    const std::string user = api_.botUsername();
    return user.empty() ? std::string() : "https://t.me/" + user + "?startgroup=true";
}

buttons::MenuText AdminPlugins::menuText(const LangView& L) const {
    buttons::MenuText t;
    t.help      = L["help"];
    t.addMe     = L["add_me"];
    t.support   = L["support"];
    t.channel   = L["channel"];
    t.source    = L["source"];
    t.language  = L["language"];
    t.cmdDelete = L["cmd_delete"];
    t.playMode  = L["play_mode"];
    t.back      = L["back"];
    t.close     = L["close"];
    return t;
}

// ---------------------------------------------------------------------------
// auth
// ---------------------------------------------------------------------------

void AdminPlugins::onAuth(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    const std::string name = ev.command.empty() ? std::string("auth") : ev.command[0];
    const bool adding = name == "auth";

    if (!guards::adminCheck(api_, db_, ev.isPrivate, ev.chatId, ev.fromUserId)) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }

    const std::int64_t target = resolveTarget(ev);
    if (target == 0) {
        api_.sendMessage(ev.chatId, L["user_not_found"]);
        return;
    }

    const std::string mention = api_.userMention(target);
    if (adding) {
        // An admin already has every power the auth list grants.
        if (guards::isAdmin(api_, ev.chatId, target)) {
            api_.sendMessage(ev.chatId, L["auth_is_admin"]);
            return;
        }
        db_.addAuth(ev.chatId, target);
        api_.sendMessage(ev.chatId, L.fmt("auth_added", mention));
    } else {
        db_.removeAuth(ev.chatId, target);
        api_.sendMessage(ev.chatId, L.fmt("auth_removed", mention));
    }
}

void AdminPlugins::onAuthList(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    const std::vector<std::int64_t> users = db_.getAuthUsers(ev.chatId);
    if (users.empty()) {
        api_.sendMessage(ev.chatId, L["auth_empty"]);
        return;
    }

    std::string text = L.fmt("auth_list", anonx::utils::htmlEscape(api_.chatTitle(ev.chatId)));
    for (std::int64_t id : users)
        text += "- " + api_.userMention(id) + "\n";
    api_.sendMessage(ev.chatId, text);
}

// ---------------------------------------------------------------------------
// blacklist
// ---------------------------------------------------------------------------

void AdminPlugins::onBlacklist(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    const std::string name = ev.command.empty() ? std::string("blacklist") : ev.command[0];
    const bool adding = name == "blacklist";

    if (!guards::isSudo(db_, config_, ev.fromUserId)) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }

    // The target may be given as an id, or by replying to someone's message.
    std::int64_t id = 0;
    if (ev.command.size() >= 2) {
        if (!anonx::utils::parseI64(ev.command[1], id) || id == 0) {
            api_.sendMessage(ev.chatId, L["bl_invalid"]);
            return;
        }
    } else if (ev.hasReply()) {
        id = api_.getMessageSenderId(ev.chatId, ev.replyToMessageId);
    }
    if (id == 0) {
        api_.sendMessage(ev.chatId, L.fmt("bl_usage", name));
        return;
    }

    // Chat ids are negative, user ids positive — the same split the Python
    // add_blacklist/del_blacklist helpers make.
    const bool isChat = id < 0;
    const bool listed = isChat ? db_.isBlacklistedChat(id) : db_.isBlacklistedUser(id);

    if (adding) {
        if (listed) {
            api_.sendMessage(ev.chatId, L["bl_already"]);
            return;
        }
        db_.addBlacklist(id);
        api_.sendMessage(ev.chatId, L["bl_added"]);
        if (!isChat) {
            // Courtesy notice to the user, in their own chat with the bot.
            api_.sendMessage(id, L.fmt("bl_user_notify", config_.support_chat));
        }
    } else {
        if (!listed) {
            api_.sendMessage(ev.chatId, L["bl_not"]);
            return;
        }
        db_.removeBlacklist(id);
        api_.sendMessage(ev.chatId, L["bl_removed"]);
    }
}

// ---------------------------------------------------------------------------
// broadcast
// ---------------------------------------------------------------------------

void AdminPlugins::onGcast(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!guards::isSudo(db_, config_, ev.fromUserId)) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }
    if (!ev.hasReply()) {
        api_.sendMessage(ev.chatId, L["gcast_usage"]);
        return;
    }

    bool expected = false;
    if (!broadcasting_.compare_exchange_strong(expected, true)) {
        api_.sendMessage(ev.chatId, L["gcast_active"]);
        return;
    }

    // Flags (help_sudo): -nochat skips groups, -user adds users, -copy strips the
    // "forwarded from" header.
    const bool noChat = hasFlag(ev.command, "-nochat");
    const bool toUsers = hasFlag(ev.command, "-user");
    const bool asCopy = hasFlag(ev.command, "-copy");

    api_.sendMessage(ev.chatId, L["gcast_start"]);

    std::size_t groups = 0, users = 0;
    const auto relay = [&](std::int64_t target) {
        return asCopy ? api_.copyMessage(ev.chatId, ev.replyToMessageId, target)
                      : api_.forwardMessage(ev.chatId, ev.replyToMessageId, target);
    };

    if (!noChat) {
        for (std::int64_t chatId : db_.getChats()) {
            if (db_.isBlacklistedChat(chatId))
                continue;
            if (relay(chatId))
                ++groups;
        }
    }
    if (toUsers) {
        for (std::int64_t userId : db_.getUsers()) {
            if (db_.isBlacklistedUser(userId))
                continue;
            if (relay(userId))
                ++users;
        }
    }

    broadcasting_ = false;
    api_.sendMessage(ev.chatId, L.fmt("gcast_end", groups, users));
}

// ---------------------------------------------------------------------------
// sudoers
// ---------------------------------------------------------------------------

void AdminPlugins::onSudo(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    const std::string name = ev.command.empty() ? std::string("addsudo") : ev.command[0];
    const bool adding = name == "addsudo";

    // Granting sudo is the owner's privilege alone (Python gates the handler with
    // filters.user(OWNER_ID); this port replies instead of staying silent).
    if (ev.fromUserId != config_.owner_id) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }

    const std::int64_t target = resolveTarget(ev);
    if (target == 0) {
        api_.sendMessage(ev.chatId, L["user_not_found"]);
        return;
    }

    const std::string mention = api_.userMention(target);
    if (adding) {
        if (db_.isSudo(target)) {
            api_.sendMessage(ev.chatId, L.fmt("sudo_already", mention));
            return;
        }
        db_.addSudo(target);
        api_.sendMessage(ev.chatId, L.fmt("sudo_added", mention));
    } else {
        if (!db_.isSudo(target)) {
            api_.sendMessage(ev.chatId, L.fmt("sudo_not", mention));
            return;
        }
        db_.removeSudo(target);
        api_.sendMessage(ev.chatId, L.fmt("sudo_removed", mention));
    }
}

void AdminPlugins::onSudoList(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    setStatus(ev.chatId, api_.sendMessage(ev.chatId, L["sudo_fetching"]));

    std::string text = L.fmt("sudo_owner", api_.userMention(config_.owner_id));
    const std::vector<std::int64_t> sudoers = db_.getSudoers();
    if (!sudoers.empty()) {
        text += L["sudo_users"];
        for (std::int64_t id : sudoers)
            text += "\n- " + api_.userMention(id);
    }
    say(ev.chatId, text);
}

// ---------------------------------------------------------------------------
// language
// ---------------------------------------------------------------------------

InlineKeyboard AdminPlugins::languageKeyboard(const LangView& L) const {
    // Only offer languages whose file actually loaded, so no button can lead to
    // a chat set to a missing translation.
    std::vector<std::pair<std::string, std::string>> available;
    for (const auto& entry : Language::allCodes())
        if (lang_.loaded(entry.first))
            available.push_back(entry);
    return buttons::langMenu(available, menuText(L));
}

void AdminPlugins::onLang(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!mayConfigure(ev)) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }
    api_.sendMessage(ev.chatId, L["lang_choose"], languageKeyboard(L));
}

// ---------------------------------------------------------------------------
// info
// ---------------------------------------------------------------------------

void AdminPlugins::onPing(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);

    // Latency is measured the way the Python bot does it: the round trip of the
    // "pinging" message itself.
    const auto sentAt = std::chrono::steady_clock::now();
    const std::int64_t status = api_.sendMessage(ev.chatId, L["pinging"]);
    const auto elapsed = std::chrono::steady_clock::now() - sentAt;
    const std::int64_t latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    setStatus(ev.chatId, status);

    // The support button is dropped when no support chat is configured, so the
    // card never carries a button with an empty URL.
    InlineKeyboard kb;
    if (!config_.support_chat.empty())
        kb = buttons::pingMarkup(L["support"], config_.support_chat);

    const std::string text = L.fmt("ping_pong", latencyMs, SystemInfo::formatDuration(sys_.uptimeSeconds()),
              SystemInfo::round1(sys_.cpuPercent()),
              SystemInfo::round1(sys_.ramPercent()),
              SystemInfo::round1(sys_.diskPercent()),
              SystemInfo::round1(calls_.ping()));

    if (config_.thumb_gen && !config_.ping_img.empty()) {
        const std::string thumb = thumb_.fetch(config_.ping_img);
        if (!thumb.empty()) {
            sayPhoto(ev.chatId, thumb, text, kb);
            return;
        }
    }
    say(ev.chatId, text, kb);
}

void AdminPlugins::onStats(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    setStatus(ev.chatId, api_.sendMessage(ev.chatId, L["stats_fetching"]));

    const std::vector<std::int64_t> sudoers = db_.getSudoers();
    const std::size_t sudoCount =
        sudoers.size() + (db_.isSudo(config_.owner_id) ? 0u : 1u);   // owner included

    std::string text =
        L.fmt("stats_user", api_.botName(), config_.assistantCount(),
              buttons::toggleMark(config_.auto_leave),
              db_.getBlacklistedChats().size(), db_.getBlacklistedUsers().size(),
              sudoCount, db_.chatCount(), db_.userCount());

    // The extended block is sudo-only, exactly as in the Python plugin.
    if (guards::isSudo(db_, config_, ev.fromUserId)) {
        text += L.fmt("stats_sudo", moduleCount(), sys_.platform(), sys_.ramUsedMb(),
                      SystemInfo::round1(sys_.ramTotalGb()),
                      SystemInfo::round1(sys_.cpuPercent()), sys_.cores(),
                      SystemInfo::round1(sys_.diskUsedGb()),
                      SystemInfo::round1(sys_.diskTotalGb()), sys_.toolchainVersion(),
                      sys_.telegramLibrary(), sys_.voiceLibrary());
    }
    say(ev.chatId, text);
}

void AdminPlugins::onActiveVc(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!guards::isSudo(db_, config_, ev.fromUserId)) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }

    const std::vector<std::int64_t> active = cache_.activeChats();
    if (active.empty()) {
        api_.sendMessage(ev.chatId, L["vc_empty"]);
        return;
    }

    const std::string name = ev.command.empty() ? std::string("ac") : ev.command[0];
    if (name == "ac") {                       // just the count
        api_.sendMessage(ev.chatId, L.fmt("vc_count", active.size()));
        return;
    }

    setStatus(ev.chatId, api_.sendMessage(ev.chatId, L["vc_fetching"]));
    std::string text = L["vc_list"];
    for (std::int64_t chatId : active) {
        text += "\n- <code>" + std::to_string(chatId) + "</code> | " +
                anonx::utils::htmlEscape(api_.chatTitle(chatId));
    }
    say(ev.chatId, text);
}

// ---------------------------------------------------------------------------
// menus
// ---------------------------------------------------------------------------

std::string AdminPlugins::startCard(const LangView& L, const CommandEvent& ev) const {
    if (ev.isPrivate)
        return L.fmt("start_pm", api_.userMention(ev.fromUserId), api_.botName());
    return L.fmt("start_gp", api_.botName());
}

InlineKeyboard AdminPlugins::startKeyboard(const LangView& L, bool isPrivate) const {
    const buttons::MenuText t = menuText(L);
    if (isPrivate)
        return buttons::startPrivate(t, addMeUrl(), config_.support_chat,
                                     config_.support_channel, kSourceUrl);
    return buttons::startGroup(t, addMeUrl(), config_.support_chat,
                               config_.support_channel);
}

std::string AdminPlugins::helpBody(const LangView& L) const { return L["help_menu"]; }

InlineKeyboard AdminPlugins::helpKeyboard(const LangView& L) const {
    std::vector<std::string> labels;
    labels.reserve(helpTopics().size());
    for (const auto& topic : helpTopics())
        labels.push_back(L[topic.first]);
    return buttons::helpMenu(labels, menuText(L));
}

std::string AdminPlugins::settingsCard(const LangView& L) const {
    return L.fmt("start_settings", api_.botName());
}

InlineKeyboard AdminPlugins::settingsKeyboard(const LangView& L,
                                              std::int64_t chatId) const {
    return buttons::settingsMenu(menuText(L), db_.getCmdDelete(chatId),
                                 db_.getPlayMode(chatId));
}

void AdminPlugins::onStart(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    const std::string text = startCard(L, ev);
    const InlineKeyboard kb = startKeyboard(L, ev.isPrivate);

    if (config_.thumb_gen && !config_.start_img.empty()) {
        const std::string thumb = thumb_.fetch(config_.start_img);
        if (!thumb.empty()) {
            sayPhoto(ev.chatId, thumb, text, kb);
            return;
        }
    }
    say(ev.chatId, text, kb);
}

void AdminPlugins::onHelp(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    api_.sendMessage(ev.chatId, helpBody(L), helpKeyboard(L));
}

void AdminPlugins::onSettings(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!mayConfigure(ev)) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }
    api_.sendMessage(ev.chatId, settingsCard(L), settingsKeyboard(L, ev.chatId));
}

// ---------------------------------------------------------------------------
// logger
// ---------------------------------------------------------------------------

void AdminPlugins::onLogger(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!guards::isSudo(db_, config_, ev.fromUserId)) {
        api_.sendMessage(ev.chatId, L["user_no_perms"]);
        return;
    }

    const std::string name = ev.command.empty() ? std::string("logger") : ev.command[0];
    const std::string arg = ev.command.size() >= 2 ? anonx::utils::toLower(ev.command[1]) : std::string();
    if (arg == "on" || arg == "enable") {
        db_.setLoggerEnabled(true);
        api_.sendMessage(ev.chatId, L["logger_on"]);
    } else if (arg == "off" || arg == "disable") {
        db_.setLoggerEnabled(false);
        api_.sendMessage(ev.chatId, L["logger_off"]);
    } else {
        api_.sendMessage(ev.chatId, L.fmt("logger_usage", name));
    }
}

// ---------------------------------------------------------------------------
// chat watcher
// ---------------------------------------------------------------------------

void AdminPlugins::onSeen(const CommandEvent& ev) {
    // Never account for the log group itself, and never grow the served lists
    // from a blacklisted chat or user.
    if (ev.chatId == 0 || ev.chatId == config_.logger_id)
        return;
    if (db_.isBlacklistedChat(ev.chatId))
        return;
    if (ev.fromUserId != 0 && db_.isBlacklistedUser(ev.fromUserId))
        return;

    const LangView L = lang_.view(lang_.defaultCode());   // the log group's language

    if (ev.isPrivate) {
        if (ev.fromUserId == 0 || db_.isUser(ev.fromUserId))
            return;
        db_.addUser(ev.fromUserId);
        const std::string user = api_.userUsername(ev.fromUserId);
        toLogGroup(L.fmt("log_user", ev.fromUserId, api_.userMention(ev.fromUserId),
                         user.empty() ? kNoUsername : "@" + user));
        return;
    }

    if (db_.isChat(ev.chatId))
        return;
    db_.addChat(ev.chatId);
    toLogGroup(L.fmt("log_chat", ev.chatId,
                     anonx::utils::htmlEscape(api_.chatTitle(ev.chatId)), ev.fromUserId,
                     ev.fromUserId == 0 ? kNoUsername
                                        : api_.userMention(ev.fromUserId)));
}

// ---------------------------------------------------------------------------
// inline buttons
// ---------------------------------------------------------------------------

void AdminPlugins::onMenu(const ButtonEvent& ev) {
    const LangView L = tr(ev.chatId);
    const std::vector<std::string> parts = anonx::utils::splitWs(ev.data);
    if (parts.empty())
        return;

    // Reuse the message-side permission logic by describing the presser as the
    // command sender they effectively are.
    CommandEvent as;
    as.chatId     = ev.chatId;
    as.fromUserId = ev.fromUserId;
    as.isPrivate  = ev.chatId >= 0;

    const std::string ns = parts[0];
    const std::string action = parts.size() >= 2 ? parts[1] : std::string();

    if (ns == "close") {
        api_.answerCallback(ev.queryId);
        api_.deleteMessage(ev.chatId, ev.messageId);
        return;
    }

    if (ns == "start") {                                    // "start menu"
        api_.answerCallback(ev.queryId);
        api_.editMessageText(ev.chatId, ev.messageId, startCard(L, as),
                             startKeyboard(L, as.isPrivate));
        return;
    }

    if (ns == "help") {
        api_.answerCallback(ev.queryId);
        if (action == "menu" || action.empty()) {
            api_.editMessageText(ev.chatId, ev.messageId, helpBody(L), helpKeyboard(L));
            return;
        }
        std::int64_t index = 0;
        if (!anonx::utils::parseI64(action, index) || index < 0 ||
            index >= static_cast<std::int64_t>(helpTopics().size()))
            return;
        api_.editMessageText(ev.chatId, ev.messageId,
                             L[helpTopics()[static_cast<std::size_t>(index)].second],
                             buttons::helpTopic(menuText(L)));
        return;
    }

    if (ns == "lang") {
        if (action == "menu") {
            api_.answerCallback(ev.queryId);
            api_.editMessageText(ev.chatId, ev.messageId, L["lang_choose"],
                                 languageKeyboard(L));
            return;
        }
        if (action != "set" || parts.size() < 3)
            return;
        if (!mayConfigure(as)) {
            api_.answerCallback(ev.queryId, L["user_no_perms"], true);
            return;
        }
        const std::string code = parts[2];
        // Every button carries a loaded code (see languageKeyboard), so anything
        // else is a stale or hand-made payload: ignore it rather than pointing the
        // chat at a translation that is not there.
        if (!lang_.loaded(code))
            return;
        const std::string name = Language::nameOf(code);
        if (code == db_.getLang(ev.chatId)) {
            api_.answerCallback(ev.queryId, L.fmt("lang_same", name), true);
            return;
        }
        api_.answerCallback(ev.queryId, L.fmt("lang_change", name));
        db_.setLang(ev.chatId, code);
        // The confirmation is rendered in the language just chosen.
        const LangView N = tr(ev.chatId);
        api_.editMessageText(ev.chatId, ev.messageId, N.fmt("lang_changed", name),
                             buttons::backClose(menuText(N), "settings menu"));
        return;
    }

    if (ns == "settings") {
        if (action == "menu") {
            api_.answerCallback(ev.queryId);
            api_.editMessageText(ev.chatId, ev.messageId, settingsCard(L),
                                 settingsKeyboard(L, ev.chatId));
            return;
        }
        if (action != "toggle" || parts.size() < 3)
            return;
        if (!mayConfigure(as)) {
            api_.answerCallback(ev.queryId, L["user_no_perms"], true);
            return;
        }
        const std::string& which = parts[2];
        if (which == "cmd_delete") {
            db_.setCmdDelete(ev.chatId, !db_.getCmdDelete(ev.chatId));
        } else if (which == "play_mode") {
            db_.setPlayMode(ev.chatId, !db_.getPlayMode(ev.chatId));
        } else {
            return;
        }
        api_.answerCallback(ev.queryId);
        // Only the marks changed, so the card text stays and the markup is
        // refreshed in place.
        api_.editMessageReplyMarkup(ev.chatId, ev.messageId,
                                    settingsKeyboard(L, ev.chatId));
    }
}

}  // namespace anonx
