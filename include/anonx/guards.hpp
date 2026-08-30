// AnonXMusic C++ port — Phase 6a (command plugins)
// guards.hpp — permission checks + the /play preflight.
//
// Ports the decorators in anony/helpers/_admins.py (admin_check, can_manage_vc,
// is_admin) and the checkUB wrapper in anony/helpers/_play.py.
//
// Two design choices keep this layer decoupled and offline-testable:
//   * The checks are PURE predicates returning bool — they do not send the
//     "you don't have permission" reply themselves (the Python decorators did).
//     The caller in plugins.cpp issues the right response (reply_text for a
//     message, answer(alert) for a callback). This makes the guards trivial to
//     unit-test.
//   * They take plain ids + flags, never Telegram/dispatcher objects, so
//     guards.hpp pulls in no transport headers.
//
// Admin status: the Python bot keeps a cached admin-id list (db.get_admins,
// refreshed by reload_admins). That cache is a Phase 6b concern; here admin
// status is resolved live via BotApi::getChatMemberStatus, which is the same
// membership test the cache memoizes.

#ifndef ANONX_GUARDS_HPP
#define ANONX_GUARDS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "anonx/bot_api.hpp"
#include "anonx/assistant_api.hpp"
#include "anonx/config.hpp"
#include "anonx/database.hpp"
#include "anonx/queue.hpp"
#include "anonx/youtube.hpp"

namespace anonx {
namespace guards {

// Is `userId` an administrator or the owner of `chatId`? Resolved live from
// membership status. Ports helpers._admins.is_admin (minus the admin cache).
bool isAdmin(BotApi& api, std::int64_t chatId, std::int64_t userId);

// Is `userId` a sudoer? The owner counts even though OWNER_ID is never written
// to the sudo table — Python's SUDOERS filter is seeded with OWNER_ID at import
// time, so every sudo check must fold the owner in. Use this rather than
// db.isSudo() wherever a command's permission is "sudo only".
bool isSudo(Database& db, const Config& config, std::int64_t userId);

// Can this user manage the voice chat here? sudo OR authorized OR admin.
// Ports helpers._admins.can_manage_vc's positive path.
bool canManageVc(BotApi& api, Database& db, std::int64_t chatId, std::int64_t userId);

// Admin-only gate (private chats always pass; else sudo OR admin). Note this
// does NOT include the per-chat auth list. Ports helpers._admins.admin_check.
bool adminCheck(BotApi& api, Database& db, bool isPrivate,
                std::int64_t chatId, std::int64_t userId);

// ------------------------------------------------------------------
// /play preflight (ports helpers._play.checkUB)
// ------------------------------------------------------------------

// Everything the preflight needs to know about the incoming command. Modelled
// as plain data so it can be built from a MessageContext (or a test).
struct PlayRequest {
    std::int64_t chatId = 0;
    std::int64_t fromUserId = 0;         // 0 == sent anonymously / by a chat
    bool isSupergroup = true;            // Python requires ChatType.SUPERGROUP
    std::vector<std::string> command;    // [name, arg1, …] (name without prefix)
    std::vector<TextEntity> entities;
    MediaDescriptor replyMedia;

    bool hasReply() const { return replyMedia.kind != ""; }
};

enum class PlayGate {
    Proceed,       // all checks passed — run the play handler
    UserInvalid,   // no from_user            -> play_user_invalid
    ChatInvalid,   // not a supergroup        -> play_chat_invalid, then leave chat
    Usage,         // nothing to play         -> play_usage
    QueueFull,     // queue at capacity       -> play_queue_full (arg: QUEUE_LIMIT/min)
    NotFound,      // url is an unhandled YT   -> play_not_found (arg: SUPPORT_CHAT)
    AdminOnly,     // play-mode/force, no perm -> play_admin
    AssistantBanned,        // -> play_banned
    BotLacksInvitePermission, // -> admin_required
    AssistantJoinFailed,    // -> play_invite_error
    UnsupportedChat,        // -> play_unsupported
};

struct PlayPreflight {
    PlayGate    gate = PlayGate::Proceed;
    bool        force = false;    // "playforce"/"vplayforce" or a "-f" flag
    bool        video = false;    // "vplay*" / a "-v" flag, and VIDEO_PLAY enabled
    bool        m3u8  = false;    // url present but not a valid YouTube link
    std::string url;              // resolved link ("" == treat args as a search)
    std::string query;            // arguments minus the flags, space-joined —
                                  // the search text when `url` is empty
    bool        cmdDelete = false;// delete the command message afterwards
};

// Is `token` one of the command flags ("-f" force / "-v" video) rather than part
// of the search text? Ports the flag handling documented in help_play.
bool isFlag(const std::string& token);

// Run the preflight. PURE except for the membership/auth lookups it performs via
// `api`/`db`/`assistant`; it neither sends messages nor leaves chats — the caller acts on
// `gate`.
PlayPreflight runPlayPreflight(BotApi& api, AssistantApi& assistant, Database& db, Queue& queue,
                               YouTube& yt, const Config& config,
                               const PlayRequest& req);

// Extract a playable URL from typed command tokens (the first http/https token),
// trimming the "&si"/"?si" tracking suffix like utils.get_url. Returns "" when
// no URL token is present. Updated in P7.7 to use TDLib entities for exact extraction.
std::string resolveUrl(const std::vector<std::string>& command, const std::vector<TextEntity>& entities);

// Extract URL from entities. Falls back to token matching if no entities found.
std::string extractUrl(const std::vector<std::string>& command, const std::vector<TextEntity>& entities);

}  // namespace guards
}  // namespace anonx

#endif  // ANONX_GUARDS_HPP
