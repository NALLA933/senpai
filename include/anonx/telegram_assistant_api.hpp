// AnonXMusic C++ port — Phase 7
// telegram_assistant_api.hpp — Real implementation of AssistantApi.

#ifndef ANONX_TELEGRAM_ASSISTANT_API_HPP
#define ANONX_TELEGRAM_ASSISTANT_API_HPP

#include "anonx/assistant_api.hpp"
#include "anonx/telegram_client.hpp"

namespace anonx {

class TelegramAssistantApi : public AssistantApi {
public:
    explicit TelegramAssistantApi(TelegramClient& client);
    ~TelegramAssistantApi() override = default;

    MemberStatus getStatus(std::int64_t chatId) override;
    bool unban(std::int64_t chatId) override;
    JoinResult joinByUsername(const std::string& username) override;
    JoinResult joinByInviteLink(const std::string& link) override;
    std::string myMention() const override;

private:
    TelegramClient& client_;
};

}  // namespace anonx

#endif  // ANONX_TELEGRAM_ASSISTANT_API_HPP
