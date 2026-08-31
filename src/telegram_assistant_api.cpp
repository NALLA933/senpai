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

TelegramAssistantApi::TelegramAssistantApi(Userbot& userbot) : userbot_(userbot) {}

AssistantApi::MemberStatus TelegramAssistantApi::getStatus(std::int64_t chatId) {
    auto* client = userbot_.at(0);
    if (!client) return MemberStatus::Unknown;
    json req;
    req["@type"] = "getChatMember";
    req["chat_id"] = chatId;
    req["member_id"] = json::object({{"@type", "messageSenderUser"}, {"user_id", client->me().id}});

    std::string resp = client->raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);

    if (j.is_object() && j.value("@type", "") == "error") {
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
    auto* client = userbot_.at(0);
    if (!client) return false;
    json req;
    req["@type"] = "setChatMemberStatus";
    req["chat_id"] = chatId;
    req["member_id"] = json::object({{"@type", "messageSenderUser"}, {"user_id", client->me().id}});
    req["status"] = json::object({{"@type", "chatMemberStatusLeft"}});

    std::string resp = client->raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);
    return j.is_object() && j.value("@type", "") == "ok";
}

AssistantApi::JoinResult TelegramAssistantApi::joinByUsername(const std::string& username) {
    auto* client = userbot_.at(0);
    if (!client) return {false, false};
    json searchReq;
    searchReq["@type"] = "searchPublicChat";
    searchReq["username"] = username;
    std::string searchResp = client->raw().invoke(searchReq.dump());
    json searchJ = json::parse(searchResp, nullptr, false);

    if (!searchJ.is_object() || searchJ.value("@type", "") == "error") {
        return {false, false};
    }
    std::int64_t chatId = intField(searchJ, "id");

    json joinReq;
    joinReq["@type"] = "joinChat";
    joinReq["chat_id"] = chatId;
    std::string joinResp = client->raw().invoke(joinReq.dump());
    json j = json::parse(joinResp, nullptr, false);
    
    if (!j.is_discarded() && j.is_object()) {
        if (strField(j, "@type") == "ok") return {true, false};
        if (strField(j, "@type") == "error" && intField(j, "code") == 429) return {false, true};
    }
    return {false, false};
}

AssistantApi::JoinResult TelegramAssistantApi::joinByInviteLink(const std::string& link) {
    auto* client = userbot_.at(0);
    if (!client) return {false, false};
    json req;
    req["@type"] = "joinChatByInviteLink";
    req["invite_link"] = link;
    std::string resp = client->raw().invoke(req.dump());
    json j = json::parse(resp, nullptr, false);
    
    if (!j.is_discarded() && j.is_object()) {
        if (strField(j, "@type") == "chat") return {true, false}; // Returns chat on success
        if (strField(j, "@type") == "error" && intField(j, "code") == 429) return {false, true};
    }
    return {false, false};
}

std::string TelegramAssistantApi::myMention() const {
    auto* client = userbot_.at(0);
    if (!client) return "Assistant";
    const auto& m = client->me();
    if (m.id == 0) return "Assistant";
    std::string name = m.firstName.empty() ? "Assistant" : m.firstName;
    return "<a href=\"tg://user?id=" + std::to_string(m.id) + "\">" + name + "</a>";
}

}  // namespace anonx
