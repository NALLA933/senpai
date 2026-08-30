// AnonXMusic C++ port — Phase 6a (command plugins)
// guards.cpp — permission checks + the /play preflight (see guards.hpp).

#include "anonx/guards.hpp"

namespace anonx {
namespace guards {

bool isAdmin(BotApi& api, std::int64_t chatId, std::int64_t userId) {
    const std::string status = api.getChatMemberStatus(chatId, userId);
    return status == "chatMemberStatusAdministrator" ||
           status == "chatMemberStatusCreator";
}

bool isSudo(Database& db, const Config& config, std::int64_t userId) {
    return userId == config.owner_id || db.isSudo(userId);
}

bool canManageVc(BotApi& api, Database& db, std::int64_t chatId, std::int64_t userId) {
    // Python order: sudoers -> per-chat auth -> chat admins.
    if (db.isSudo(userId))
        return true;
    if (db.isAuth(chatId, userId))
        return true;
    return isAdmin(api, chatId, userId);
}

bool adminCheck(BotApi& api, Database& db, bool isPrivate,
                std::int64_t chatId, std::int64_t userId) {
    if (isPrivate)
        return true;                 // no permissions needed in PMs
    if (db.isSudo(userId))
        return true;
    return isAdmin(api, chatId, userId);
}

bool isFlag(const std::string& token) {
    // help_play documents two flags: "-v" (play the music video) and "-f"
    // (force play, skipping the queue). Anything else is search text.
    return token == "-f" || token == "-force" || token == "-v" || token == "-video";
}

std::string extractUrl(const std::vector<std::string>& command, const std::vector<TextEntity>& entities) {
    for (const auto& ent : entities) {
        if (ent.type == "textEntityTypeTextUrl" && !ent.url.empty()) {
            return ent.url;
        }
    }
    for (std::size_t i = 1; i < command.size(); ++i) {
        const std::string& tok = command[i];
        if (tok.rfind("http://", 0) == 0 || tok.rfind("https://", 0) == 0) {
            std::string link = tok;
            std::size_t p = link.find("&si");
            if (p != std::string::npos) link = link.substr(0, p);
            p = link.find("?si");
            if (p != std::string::npos) link = link.substr(0, p);
            return link;
        }
    }
    return "";
}

std::string resolveUrl(const std::vector<std::string>& command, const std::vector<TextEntity>& entities) {
    return extractUrl(command, entities);
}

PlayPreflight runPlayPreflight(BotApi& api, AssistantApi& assistant, Database& db, Queue& queue,
                               YouTube& yt, const Config& config,
                               const PlayRequest& req) {
    PlayPreflight r;

    // 1) must be sent by a real user.
    if (req.fromUserId == 0) {
        r.gate = PlayGate::UserInvalid;
        return r;
    }

    // 2) must be a supergroup (else the bot replies and leaves).
    if (!req.isSupergroup) {
        r.gate = PlayGate::ChatInvalid;
        return r;
    }

    const auto& cmd = req.command;

    // 3) split the arguments into flags and search text. The flags may appear in
    //    any position ("/play -f song", "/play song -v"), so both the gate below
    //    and the query are computed from the flag-free remainder.
    bool argForce = false, argVideo = false;
    std::string query;
    for (std::size_t i = 1; i < cmd.size(); ++i) {
        if (isFlag(cmd[i])) {
            if (cmd[i][1] == 'f') argForce = true; else argVideo = true;
            continue;
        }
        if (!query.empty()) query.push_back(' ');
        query += cmd[i];
    }
    r.query = query;

    // 4) something must be requested: a reply, or an argument that isn't a flag.
    if (!req.hasReply() && query.empty()) {
        r.gate = PlayGate::Usage;
        return r;
    }

    // 5) queue capacity.
    if (static_cast<int>(queue.size(req.chatId)) >= config.queue_limit) {
        r.gate = PlayGate::QueueFull;
        return r;
    }

    // 6) flags. force: a "…force" command name, or a "-f" argument.
    const std::string& name = cmd.empty() ? std::string() : cmd[0];
    const bool nameForce = name.size() >= 5 &&
                           name.compare(name.size() - 5, 5, "force") == 0;
    r.force = nameForce || argForce;

    // 7) video: a "v…" command name or a "-v" argument, gated by VIDEO_PLAY.
    r.video = ((!name.empty() && name[0] == 'v') || argVideo) && config.video_play;

    // 8) resolve any URL; reject unhandled YouTube links.
    r.url = resolveUrl(cmd, req.entities);
    if (!r.url.empty() && yt.invalid(r.url)) {
        r.gate = PlayGate::NotFound;
        return r;
    }
    // 9) an m3u8/direct link is a URL that isn't a valid YouTube link.
    r.m3u8 = !r.url.empty() && !yt.valid(r.url);

    // 10) admin-only gate when play-mode is on, or when forcing.
    if (db.getPlayMode(req.chatId) || r.force) {
        const bool allowed = isAdmin(api, req.chatId, req.fromUserId) ||
                             db.isAuth(req.chatId, req.fromUserId) ||
                             db.isSudo(req.fromUserId);
        if (!allowed) {
            r.gate = PlayGate::AdminOnly;
            return r;
        }
    }

    // 11) Assistant lifecycle: check membership, unban, join.
    const auto status = assistant.getStatus(req.chatId);
    if (status == AssistantApi::MemberStatus::Banned) {
        if (!assistant.unban(req.chatId)) {
            r.gate = PlayGate::AssistantBanned;
            return r;
        }
    }
    
    if (status == AssistantApi::MemberStatus::Left || status == AssistantApi::MemberStatus::Banned) {
        // Must join. If the chat has a public username, we can join directly.
        const std::string username = api.chatUsername(req.chatId);
        if (!username.empty()) {
            auto joinRes = assistant.joinByUsername(username);
            if (!joinRes.success) {
                r.gate = PlayGate::AssistantJoinFailed;
                return r;
            }
        } else {
            // No username, we need an invite link.
            if (!api.canInviteUsers(req.chatId)) {
                r.gate = PlayGate::BotLacksInvitePermission;
                return r;
            }
            std::string link = api.exportChatInviteLink(req.chatId);
            if (link.empty()) {
                r.gate = PlayGate::BotLacksInvitePermission;
                return r;
            }
            
            auto joinRes = assistant.joinByInviteLink(link);
            if (!joinRes.success) {
                r.gate = PlayGate::AssistantJoinFailed;
                return r;
            }
        }
    }

    // 12) whether the command message should be deleted afterwards.
    r.cmdDelete = db.getCmdDelete(req.chatId);

    r.gate = PlayGate::Proceed;
    return r;
}

}  // namespace guards
}  // namespace anonx
