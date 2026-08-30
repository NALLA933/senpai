// AnonXMusic C++ port — Phase 7
// telegram_assistant_api.cpp — Real implementation of AssistantApi.

#include "anonx/telegram_assistant_api.hpp"
#include <nlohmann/json.hpp>

namespace anonx {
namespace {

using nlohmann::json;

std::string strField(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return std::string();
}

std::int64_t intField(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) {
        return j[key].get<std::int64_t>();
    }
    return 0;
}

}  // namespace

TelegramAssistantApi::TelegramAssistantApi(TelegramClient& client) : client_(client) {}

AssistantApi::MemberStatus TelegramAssistantApi::getStatus(std::int64_t chatId) {
    json req;
    req["@type"] = "getChatMember";
    req["chat_id"] = chatId;
    req["member_id"] = json::object({{"@type", "messageSenderUser"}, {"user_id", client_.me().id}});

    const std::string resp = client_.raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);
    if (j.is_discarded() || !j.is_object() || strField(j, "@type") == "error") {
        return MemberStatus::Unknown;
    }

    if (j.contains("status") && j["status"].is_object()) {
        const std::string type = strField(j["status"], "@type");
        if (type == "chatMemberStatusBanned") return MemberStatus::Banned;
        if (type == "chatMemberStatusLeft") return MemberStatus::Left;
        return MemberStatus::Member; // Member, Administrator, Creator, Restricted
    }
    return MemberStatus::Unknown;
}

bool TelegramAssistantApi::unban(std::int64_t chatId) {
    json req;
    req["@type"] = "setChatMemberStatus";
    req["chat_id"] = chatId;
    req["member_id"] = json::object({{"@type", "messageSenderUser"}, {"user_id", client_.me().id}});
    req["status"] = json::object({{"@type", "chatMemberStatusLeft"}});

    const std::string resp = client_.raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);
    return !j.is_discarded() && j.is_object() && strField(j, "@type") == "ok";
}

AssistantApi::JoinResult TelegramAssistantApi::joinByUsername(const std::string& username) {
    json search;
    search["@type"] = "searchPublicChat";
    search["username"] = username;
    const std::string chatResp = client_.raw().invoke(search.dump());

    json chat = json::parse(chatResp, nullptr, false);
    if (chat.is_discarded() || !chat.is_object() || strField(chat, "@type") != "chat") {
        return {false, false};
    }
    const std::int64_t chatId = intField(chat, "id");
    if (chatId == 0) return {false, false};

    json join;
    join["@type"] = "joinChat";
    join["chat_id"] = chatId;
    const std::string joinResp = client_.raw().invoke(join.dump());
    json j = json::parse(joinResp, nullptr, false);
    
    if (!j.is_discarded() && j.is_object()) {
        if (strField(j, "@type") == "ok") return {true, false};
        if (strField(j, "@type") == "error" && intField(j, "code") == 429) return {false, true};
    }
    return {false, false};
}

AssistantApi::JoinResult TelegramAssistantApi::joinByInviteLink(const std::string& link) {
    json req;
    req["@type"] = "joinChatByInviteLink";
    req["invite_link"] = link;
    const std::string resp = client_.raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);
    
    if (!j.is_discarded() && j.is_object()) {
        if (strField(j, "@type") == "chat") return {true, false}; // Returns chat on success
        if (strField(j, "@type") == "error" && intField(j, "code") == 429) return {false, true};
    }
    return {false, false};
}

std::string TelegramAssistantApi::myMention() const {
    return client_.me().mention;
}

}  // namespace anonx
