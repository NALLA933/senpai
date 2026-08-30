// AnonXMusic C++ port — Integration phase
// telegram_bot_api.hpp — the production BotApi, backed by a TelegramClient.
//
// Phase 6a introduced BotApi (include/anonx/bot_api.hpp) as the exact Telegram
// surface the command plugins need, and Phase 6a/6b tested the whole command
// layer against a FakeBotApi. This is the other implementation: the one that
// actually talks to Telegram, by forwarding each call to the bot account's
// TelegramClient.
//
// WHERE IT LIVES IN THE BUILD
// Like plugins_router.cpp, this file is compiled into the Telegram targets, not
// into anonx_plugins — that is what keeps TDLib out of the command library and
// out of its offline tests.
//
// WHAT IT ADDS OVER PLAIN FORWARDING
//   * botName/botUsername come from the client's cached me();
//   * userMention resolves the user's first name (the id-only default in BotApi
//     cannot), so "<b>Stream paused by</b> {0}" renders a real name;
//   * user and chat-title lookups are memoised, because userMention/chatTitle are
//     called on nearly every command and each miss is a round trip.

#ifndef ANONX_TELEGRAM_BOT_API_HPP
#define ANONX_TELEGRAM_BOT_API_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "anonx/bot_api.hpp"
#include "anonx/telegram_client.hpp"

namespace anonx {

class TelegramBotApi : public BotApi {
public:
    // `bot` must be a booted bot account and must outlive this object.
    explicit TelegramBotApi(TelegramClient& bot);

    std::int64_t sendMessage(std::int64_t chatId, const std::string& html,
                             const InlineKeyboard& kb = {}) override;

    DownloadResult downloadFile(int32_t fileId, std::function<bool(const DownloadProgress&)> progressCallback) override;

    bool editMessageText(std::int64_t chatId, std::int64_t messageId,
                         const std::string& html,
                         const InlineKeyboard& kb = {}) override;
    bool editMessageReplyMarkup(std::int64_t chatId, std::int64_t messageId,
                                const InlineKeyboard& kb) override;
    bool deleteMessage(std::int64_t chatId, std::int64_t messageId) override;
    bool deleteMessages(std::int64_t chatId,
                        const std::vector<std::int64_t>& messageIds) override;
    std::string getMessageText(std::int64_t chatId, std::int64_t messageId) override;
    bool copyMessage(std::int64_t fromChatId, std::int64_t messageId,
                     std::int64_t toChatId) override;
    bool forwardMessage(std::int64_t fromChatId, std::int64_t messageId,
                        std::int64_t toChatId) override;
    std::int64_t getMessageSenderId(std::int64_t chatId,
                                    std::int64_t messageId) override;
    void answerCallback(std::int64_t queryId, const std::string& text = "",
                        bool alert = false) override;
    void leaveChat(std::int64_t chatId) override;
    std::string getChatMemberStatus(std::int64_t chatId, std::int64_t userId) override;

    std::string botName() override;
    std::string botUsername() override;
    std::string chatTitle(std::int64_t chatId) override;
    std::string userUsername(std::int64_t userId) override;
    std::string userMention(std::int64_t userId) override;
    std::string messageLink(std::int64_t chatId, std::int64_t messageId) override;

    // Drop the memoised titles/users (e.g. after a rename). Also called
    // automatically once a cache grows past kCacheLimit entries.
    void clearCaches();

private:
    static constexpr std::size_t kCacheLimit = 4096;

    TelegramClient::UserInfo user(std::int64_t userId);

    TelegramClient& bot_;

    std::mutex cacheMutex_;
    std::unordered_map<std::int64_t, TelegramClient::UserInfo> users_;
    std::unordered_map<std::int64_t, std::string> chatTitles_;
};

}  // namespace anonx

#endif  // ANONX_TELEGRAM_BOT_API_HPP
