// AnonXMusic C++ port — Phase 4
// telegram_client.hpp — high-level Telegram account on top of TdClient.
//
// One TelegramClient == one logged-in account. It can be either:
//   * a BOT   (Options::botToken set)  — the main bot, like anony/core/bot.py
//   * a USER  (Options::phoneNumber set) — an assistant userbot, like
//              anony/core/userbot.py
//
// It drives TDLib's authorization state machine to completion in boot(), then
// exposes the operations the rest of the bot needs: getMe, sending and editing
// HTML messages with inline keyboards, deleting and re-sending messages,
// answering callback queries, and chat/user lookups. Together these are exactly
// what BotApi (include/anonx/bot_api.hpp) requires — TelegramBotApi is the thin
// adapter between the two. All JSON lives in the .cpp; this header depends only
// on the standard library, TdClient and the inline-keyboard structs.
//
// IMPORTANT — sessions are NOT interchangeable with the Python bot.
// Pyrogram/kurigram SESSION strings CANNOT be imported into TDLib: the two use
// completely different session formats. The bot token works unchanged, but each
// assistant userbot must perform a one-time TDLib-native login (phone number +
// login code, and 2FA password if enabled) on first run. After that TDLib
// persists the session in its own per-account database directory and no
// interactive login is needed again.

#ifndef ANONX_TELEGRAM_CLIENT_HPP
#define ANONX_TELEGRAM_CLIENT_HPP

#include <cstdint>
#include <functional>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "anonx/inline_keyboard.hpp"
#include "anonx/td_client.hpp"
#include "anonx/youtube.hpp"  // for DownloadProgress, DownloadResult

namespace anonx {

class TelegramClient {
public:
    struct Options {
        int apiId = 0;
        std::string apiHash;

        // Where TDLib stores this account's session/database. MUST be unique
        // per account (e.g. "tdlib/bot", "tdlib/assistant1").
        std::string databaseDirectory = "tdlib";

        // Exactly one of these selects the login mode:
        std::string botToken;      // non-empty  -> log in as a bot
        std::string phoneNumber;   // non-empty  -> log in as a user (assistant)

        // Human-readable label used only in log lines ("anony", "AnonyUB1", …).
        std::string name = "account";

        // First-run interactive login for USER accounts. Called when TDLib asks
        // for the login code / 2FA password. Return the value the user typed.
        // Leave unset for bot accounts (never invoked for them).
        std::function<std::string()> codeProvider;
        std::function<std::string()> passwordProvider;

        // Presented to Telegram; harmless defaults.
        std::string deviceModel = "AnonXMusic";
        std::string applicationVersion = "1.0";
        std::string systemLanguageCode = "en";
    };

    struct Me {
        std::int64_t id = 0;
        std::string firstName;
        std::string username;   // without '@' (empty if none)
        std::string mention;    // HTML: <a href="tg://user?id=ID">first name</a>
    };

    // A user resolved with getUser — the fields the log/auth cards interpolate.
    struct UserInfo {
        std::int64_t id = 0;
        std::string firstName;
        std::string username;   // without '@' (empty if none)
        bool found = false;
    };

    explicit TelegramClient(Options opts);
    ~TelegramClient();

    TelegramClient(const TelegramClient&) = delete;
    TelegramClient& operator=(const TelegramClient&) = delete;

    // Install the update handler and drive authorization to completion.
    // Returns true once the account is authorized (analogue of Client.start()).
    bool boot(int timeoutMs = 60000);

    // Log out cleanly (TDLib "close"); safe to call more than once.
    void exit();

    bool authorized() const { return status_.load() == Status::Ready; }
    const Me& me() const { return me_; }
    const std::string& name() const { return opts_.name; }

    // Refresh and return the current user (also stored in me()).
    Me getMe();

    // Send an HTML-formatted message; returns the new message id (0 on failure).
    // An empty keyboard means reply_markup=None.
    std::int64_t sendMessage(std::int64_t chatId, const std::string& html,
                             const InlineKeyboard& kb = {});

    // Send a photo with an HTML caption.
    std::int64_t sendPhoto(std::int64_t chatId, const std::string& photoPath,
                           const std::string& captionHtml, const InlineKeyboard& kb = {});

    // Replace a message's text (and its keyboard). Mirrors message.edit_text.
    bool editMessageText(std::int64_t chatId, std::int64_t messageId,
                         const std::string& html, const InlineKeyboard& kb = {});

    // Replace a message's media with a photo and a caption.
    bool editMessageMedia(std::int64_t chatId, std::int64_t messageId,
                          const std::string& photoPath, const std::string& captionHtml,
                          const InlineKeyboard& kb = {});

    // Replace only a message's keyboard. Mirrors edit_message_reply_markup.
    bool editMessageReplyMarkup(std::int64_t chatId, std::int64_t messageId,
                                const InlineKeyboard& kb);

    // Delete messages for everyone (revoke=true), like app.delete_messages.
    bool deleteMessages(std::int64_t chatId,
                        const std::vector<std::int64_t>& messageIds,
                        bool revoke = true);

    // A message's text or caption, re-rendered as HTML from TDLib's
    // formattedText entities so an edit can round-trip it without losing markup.
    // Empty when the message is gone or carries no text.
    std::string getMessageText(std::int64_t chatId, std::int64_t messageId);

    // Synchronously fetches a message object from TDLib as raw JSON.
    std::string getMessageJson(std::int64_t chatId, std::int64_t messageId);

    // Synchronously download a Telegram file by file_id, tracking progress.
    DownloadResult downloadFile(int32_t fileId, std::function<bool(const DownloadProgress&)> progressCallback);

    // Sender of a message (user id), or 0 when sent anonymously / on behalf of a
    // chat. Backs the "reply to a user" form of /auth, /blacklist and /addsudo.
    std::int64_t getMessageSenderId(std::int64_t chatId, std::int64_t messageId);

    // Re-send messages into another chat. sendCopy=true drops the "forwarded
    // from" header (message.copy); false keeps it (app.forward_messages).
    bool forwardMessages(std::int64_t fromChatId,
                         const std::vector<std::int64_t>& messageIds,
                         std::int64_t toChatId, bool sendCopy);

    // Acknowledge a callback query: a toast, or an alert popup when `alert` is
    // set. Fire-and-forget, like Pyrogram's query.answer().
    void answerCallbackQuery(std::int64_t queryId, const std::string& text,
                             bool alert);

    // Leave a chat (the play preflight uses it when the chat is not a supergroup).
    void leaveChat(std::int64_t chatId);

    // Chat title, or "" when unavailable.
    std::string chatTitle(std::int64_t chatId);

    // Resolve a user. `found` is false when TDLib has no such user cached.
    UserInfo getUser(std::int64_t userId);

    // A t.me link to a message. Uses TDLib's getMessageLink (which knows about
    // public usernames and topic threads) and falls back to the private-
    // supergroup form t.me/c/<internal id>/<message id>. Empty for chats that
    // cannot be linked, e.g. basic groups.
    std::string messageLink(std::int64_t chatId, std::int64_t messageId);

    // Return the member's status @type for a chat, e.g.
    // "chatMemberStatusAdministrator", "chatMemberStatusCreator",
    // "chatMemberStatusMember", "chatMemberStatusLeft". Empty on error.
    std::string getChatMemberStatus(std::int64_t chatId, std::int64_t userId);

    // Access the underlying transport (updates, raw requests).
    TdClient& raw() { return client_; }

    // Register an extra observer for non-auth updates (e.g. the Dispatcher).
    // The auth state machine keeps working regardless.
    void setUpdateObserver(TdClient::UpdateHandler observer);

private:
    enum class Status { Pending, Ready, Closed, Error };

    struct PendingDownload {
        std::int64_t downloaded = 0;
        std::int64_t total = 0;
        std::string localPath;
        bool completed = false;
        bool failed = false;
    };
    std::mutex dlMutex_;
    std::condition_variable dlCv_;
    std::unordered_map<int32_t, std::shared_ptr<PendingDownload>> downloads_;

    void onUpdate(const std::string& updateJson);   // parses JSON (in .cpp)
    void setStatus(Status s);

    Options opts_;
    TdClient client_;
    Me me_;

    std::atomic<Status> status_{Status::Pending};
    std::mutex cvMutex_;
    std::condition_variable cv_;

    std::mutex observerMutex_;
    TdClient::UpdateHandler observer_;

    std::atomic<bool> closeRequested_{false};
};

}  // namespace anonx

#endif  // ANONX_TELEGRAM_CLIENT_HPP
