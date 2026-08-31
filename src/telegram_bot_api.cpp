// AnonXMusic C++ port — Integration phase
// telegram_bot_api.cpp — see telegram_bot_api.hpp.

#include "anonx/telegram_bot_api.hpp"
#include <nlohmann/json.hpp>

namespace anonx {
namespace {

using nlohmann::json;

// Safe string-field read: returns "" unless the key holds a string.
std::string strField(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return std::string();
}

// Same escaping the command layer uses (Plugins::htmlEscape), duplicated here as
// a three-line local rather than pulling the plugin header into the Telegram
// layer: a display name can contain '<' or '&' and would otherwise break the
// surrounding markup.
std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            default:  out.push_back(c); break;
        }
    }
    return out;
}

}  // namespace

TelegramBotApi::TelegramBotApi(TelegramClient& bot) : bot_(bot) {}

std::int64_t TelegramBotApi::sendMessage(std::int64_t chatId, const std::string& html,
                                         const InlineKeyboard& kb) {
    return bot_.sendMessage(chatId, html, kb);
}

std::int64_t TelegramBotApi::sendPhoto(std::int64_t chatId, const std::string& photoPath,
                                       const std::string& captionHtml,
                                       const InlineKeyboard& kb) {
    return bot_.sendPhoto(chatId, photoPath, captionHtml, kb);
}

bool TelegramBotApi::editMessageText(std::int64_t chatId, std::int64_t messageId,
                                     const std::string& html, const InlineKeyboard& kb) {
    return bot_.editMessageText(chatId, messageId, html, kb);
}

bool TelegramBotApi::editMessageMedia(std::int64_t chatId, std::int64_t messageId,
                                      const std::string& photoPath,
                                      const std::string& captionHtml,
                                      const InlineKeyboard& kb) {
    return bot_.editMessageMedia(chatId, messageId, photoPath, captionHtml, kb);
}

bool TelegramBotApi::editMessageReplyMarkup(std::int64_t chatId, std::int64_t messageId,
                                            const InlineKeyboard& kb) {
    return bot_.editMessageReplyMarkup(chatId, messageId, kb);
}

bool TelegramBotApi::deleteMessage(std::int64_t chatId, std::int64_t messageId) {
    return bot_.deleteMessages(chatId, {messageId});
}

bool TelegramBotApi::deleteMessages(std::int64_t chatId,
                                    const std::vector<std::int64_t>& messageIds) {
    return bot_.deleteMessages(chatId, messageIds);
}

std::string TelegramBotApi::getMessageText(std::int64_t chatId, std::int64_t messageId) {
    return bot_.getMessageText(chatId, messageId);
}

bool TelegramBotApi::copyMessage(std::int64_t fromChatId, std::int64_t messageId,
                                 std::int64_t toChatId) {
    // send_copy=true — no "forwarded from" header, matching message.copy().
    return bot_.forwardMessages(fromChatId, {messageId}, toChatId, true);
}

bool TelegramBotApi::forwardMessage(std::int64_t fromChatId, std::int64_t messageId,
                                    std::int64_t toChatId) {
    return bot_.forwardMessages(fromChatId, {messageId}, toChatId, false);
}

std::int64_t TelegramBotApi::getMessageSenderId(std::int64_t chatId,
                                                std::int64_t messageId) {
    return bot_.getMessageSenderId(chatId, messageId);
}

void TelegramBotApi::answerCallback(std::int64_t queryId, const std::string& text,
                                    bool alert) {
    bot_.answerCallbackQuery(queryId, text, alert);
}

void TelegramBotApi::leaveChat(std::int64_t chatId) {
    bot_.leaveChat(chatId);
}

std::string TelegramBotApi::getChatMemberStatus(std::int64_t chatId,
                                                std::int64_t userId) {
    return bot_.getChatMemberStatus(chatId, userId);
}

std::string TelegramBotApi::botName() {
    const std::string name = bot_.me().firstName;
    return name.empty() ? BotApi::botName() : name;
}

std::string TelegramBotApi::botUsername() {
    return bot_.me().username;
}

void TelegramBotApi::clearCaches() {
    std::lock_guard<std::mutex> lk(cacheMutex_);
    users_.clear();
    chatTitles_.clear();
}

std::string TelegramBotApi::chatTitle(std::int64_t chatId) {
    if (chatId == 0) return std::string();
    json j = json::parse(bot_.raw().invoke(json::object({
        {"@type", "getChat"}, {"chat_id", chatId}
    }).dump()), nullptr, false);
    return strField(j, "title");
}

std::string TelegramBotApi::chatUsername(std::int64_t chatId) {
    if (chatId == 0) return std::string();
    json j = json::parse(bot_.raw().invoke(json::object({
        {"@type", "getChat"}, {"chat_id", chatId}
    }).dump()), nullptr, false);
    
    if (j.is_object() && j.contains("type") && j["type"].is_object()) {
        const std::string type = strField(j["type"], "@type");
        if (type == "chatTypeSupergroup") {
            json sg = json::parse(bot_.raw().invoke(json::object({
                {"@type", "getSupergroup"}, 
                {"supergroup_id", j["type"]["supergroup_id"]}
            }).dump()), nullptr, false);
            if (sg.is_object() && sg.contains("usernames") && sg["usernames"].is_object()) {
                auto active = sg["usernames"]["active_usernames"];
                if (active.is_array() && !active.empty()) {
                    return active[0].get<std::string>();
                }
            }
        }
    }
    return std::string();
}

TelegramClient::UserInfo TelegramBotApi::user(std::int64_t userId) {
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        const auto it = users_.find(userId);
        if (it != users_.end()) return it->second;
    }
    const TelegramClient::UserInfo info = bot_.getUser(userId);
    if (!info.found) return info;      // don't memoise a failed lookup

    std::lock_guard<std::mutex> lk(cacheMutex_);
    if (users_.size() >= kCacheLimit) users_.clear();
    users_[userId] = info;
    return info;
}

std::string TelegramBotApi::userUsername(std::int64_t userId) {
    return user(userId).username;
}

std::string TelegramBotApi::userMention(std::int64_t userId) {
    const TelegramClient::UserInfo info = user(userId);
    if (!info.found || info.firstName.empty()) {
        // Fall back to the id-only mention.
        return "<a href=\"tg://user?id=" + std::to_string(userId) + "\">" + std::to_string(userId) + "</a>";
    }
    return "<a href=\"tg://user?id=" + std::to_string(userId) + "\">" +
           escape(info.firstName) + "</a>";
}

bool TelegramBotApi::canInviteUsers(std::int64_t chatId) {
    json req;
    req["@type"] = "getChatMember";
    req["chat_id"] = chatId;
    req["member_id"] = json::object({{"@type", "messageSenderUser"}, {"user_id", bot_.me().id}});

    const std::string resp = bot_.raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded() || !j.is_object() || strField(j, "@type") != "chatMember") {
        return false;
    }

    if (j.contains("status") && j["status"].is_object()) {
        const std::string type = strField(j["status"], "@type");
        if (type == "chatMemberStatusAdministrator" || type == "chatMemberStatusCreator") {
            if (j["status"].contains("can_invite_users")) {
                return j["status"]["can_invite_users"].get<bool>();
            }
            return type == "chatMemberStatusCreator";
        }
    }
    return false;
}

std::string TelegramBotApi::exportChatInviteLink(std::int64_t chatId) {
    json req;
    req["@type"] = "createChatInviteLink";
    req["chat_id"] = chatId;
    
    const std::string resp = bot_.raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);
    if (!j.is_discarded() && j.is_object() && strField(j, "@type") == "chatInviteLink") {
        return strField(j, "invite_link");
    }
    return std::string();
}

std::string TelegramBotApi::messageLink(std::int64_t chatId, std::int64_t messageId) {
    return bot_.messageLink(chatId, messageId);
}

DownloadResult TelegramBotApi::downloadFile(int32_t fileId, std::function<bool(const DownloadProgress&)> progressCallback) {
    return bot_.downloadFile(fileId, std::move(progressCallback));
}

}  // namespace anonx
