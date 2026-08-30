// AnonXMusic C++ port — Phase 6a (command plugins)
// bot_api.hpp — the Telegram operations the command plugins need.
//
// WHY THIS EXISTS
// The Phase 4 TelegramClient started out exposing only plain-text sends — no
// inline keyboard, no message editing, no bulk delete. The command plugins need
// all of those, so Phase 6a introduced this small abstract interface — the exact
// surface the plugins call — mirroring the VoiceTransport pattern from Phase 5:
//
//   * production wraps a TelegramClient behind a BotApi impl
//     (TelegramBotApi, added in the integration phase together with the missing
//     TDLib serialization);
//   * tests use a FakeBotApi that records every call and can script membership
//     / message text, so the whole plugin layer is verified offline.
//
// All operations are engine-agnostic: they take explicit chat/message/query ids
// (no Telegram objects leak in), and inline keyboards are described by the plain
// structs in inline_keyboard.hpp. HTML is the message parse mode throughout
// (matching the bot).

#ifndef ANONX_BOT_API_HPP
#define ANONX_BOT_API_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "anonx/inline_keyboard.hpp"   // InlineButton / InlineKeyboard

namespace anonx {

// The abstract Telegram surface the command plugins depend on.
class BotApi {
public:
    // Download a Telegram file and track its progress (used for replied media).
    virtual DownloadResult downloadFile(int32_t fileId, std::function<bool(const DownloadProgress&)> progressCallback) = 0;

    virtual ~BotApi() = default;

    // Send an HTML message (optionally with an inline keyboard). Returns the new
    // message id, or 0 on failure. Covers both message.reply_text and
    // app.send_message (a "reply" is just a send to the same chat).
    virtual std::int64_t sendMessage(std::int64_t chatId, const std::string& html,
                                     const InlineKeyboard& kb = {}) = 0;

    // Send a photo with an HTML caption (optionally with an inline keyboard). Returns the new
    // message id, or 0 on failure.
    virtual std::int64_t sendPhoto(std::int64_t chatId, const std::string& photoPath,
                                   const std::string& captionHtml,
                                   const InlineKeyboard& kb = {}) = 0;

    // Edit a message to change its media to a photo, along with caption and keyboard.
    virtual bool editMessageMedia(std::int64_t chatId, std::int64_t messageId,
                                  const std::string& photoPath,
                                  const std::string& captionHtml,
                                  const InlineKeyboard& kb = {}) = 0;

    // Edit a message's text and keyboard. Mirrors message.edit_text. An empty
    // keyboard clears the markup. Returns true on success.
    virtual bool editMessageText(std::int64_t chatId, std::int64_t messageId,
                                 const std::string& html,
                                 const InlineKeyboard& kb = {}) = 0;

    // Edit only a message's inline keyboard. Mirrors edit_message_reply_markup.
    virtual bool editMessageReplyMarkup(std::int64_t chatId, std::int64_t messageId,
                                        const InlineKeyboard& kb) = 0;

    // Delete a single message. Mirrors message.delete().
    virtual bool deleteMessage(std::int64_t chatId, std::int64_t messageId) = 0;

    // Delete several messages at once. Mirrors app.delete_messages(..., revoke).
    virtual bool deleteMessages(std::int64_t chatId,
                                const std::vector<std::int64_t>& messageIds) = 0;

    // Current HTML text (or caption) of a message. Used by the controls handler
    // to rebuild the "now playing" card. Empty if unavailable.
    virtual std::string getMessageText(std::int64_t chatId, std::int64_t messageId) = 0;

    // Copy a message into another chat, keeping its media/formatting but not the
    // "forwarded from" header. Mirrors message.copy(chat_id) — the operation the
    // broadcast command is built on. Returns true on success.
    virtual bool copyMessage(std::int64_t fromChatId, std::int64_t messageId,
                             std::int64_t toChatId) = 0;

    // Forward a message, keeping the "forwarded from" header. Mirrors
    // app.forward_messages — the broadcast default, which "-copy" opts out of.
    virtual bool forwardMessage(std::int64_t fromChatId, std::int64_t messageId,
                               std::int64_t toChatId) = 0;

    // Sender of a message, or 0 when unknown. Lets the commands that accept
    // "reply to a user" (auth / blacklist / sudo) resolve their target, which is
    // `message.reply_to_message.from_user.id` in Python.
    virtual std::int64_t getMessageSenderId(std::int64_t chatId,
                                            std::int64_t messageId) = 0;

    // Answer a callback query — a toast, or an alert popup when `alert` is set.
    // Mirrors query.answer(text, show_alert=...).
    virtual void answerCallback(std::int64_t queryId, const std::string& text = "",
                                bool alert = false) = 0;

    // Leave a chat. Used by the play preflight when the chat is not a supergroup.
    virtual void leaveChat(std::int64_t chatId) = 0;

    // Member status @type for a user in a chat, e.g.
    // "chatMemberStatusAdministrator" / "chatMemberStatusCreator" /
    // "chatMemberStatusMember". Empty on error. Backs the admin guards.
    virtual std::string getChatMemberStatus(std::int64_t chatId, std::int64_t userId) = 0;

    // The bot's own display name and @username (used in a few templates).
    virtual std::string botName() { return "AnonXMusic"; }
    virtual std::string botUsername() { return ""; }

    // Title of a chat, and a user's @username — used by the log-group notices
    // ("New Chat Log" / "New User Log"). Empty when unavailable.
    virtual std::string chatTitle(std::int64_t chatId) { (void)chatId; return ""; }
    virtual std::string userUsername(std::int64_t userId) { (void)userId; return ""; }
    virtual std::string chatUsername(std::int64_t chatId) { (void)chatId; return ""; }

    // An HTML mention of a user, the analogue of Pyrogram's `user.mention`
    // (which renders the user's first name as a tg://user link). Many templates
    // interpolate it — "<b>Stream paused by</b> {0}". The default builds the
    // link from the id alone; a production BotApi overrides this to resolve the
    // display name, which the id-only form cannot know.
    virtual std::string userMention(std::int64_t userId) = 0;

    // Assistant lifecycle support
    // Checks if the bot has permissions to invite users to the chat.
    virtual bool canInviteUsers(std::int64_t chatId) = 0;
    
    // Generates a new primary invite link for the chat.
    virtual std::string exportChatInviteLink(std::int64_t chatId) = 0;

    // A t.me link for a message (used by the optional play-log). May return "".
    virtual std::string messageLink(std::int64_t chatId, std::int64_t messageId) {
        (void)chatId; (void)messageId; return "";
    }
};

}  // namespace anonx

#endif  // ANONX_BOT_API_HPP
