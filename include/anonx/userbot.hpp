// AnonXMusic C++ port — Phase 4
// userbot.hpp — manager for the assistant userbot accounts.
//
// The C++ analogue of anony/core/userbot.py. The original builds up to three
// assistant Clients from Pyrogram SESSION strings. TDLib cannot use those
// strings, so each assistant here logs in with its phone number on first run
// and TDLib then persists the session in its own database directory. On later
// runs no interactive login is needed.

#ifndef ANONX_USERBOT_HPP
#define ANONX_USERBOT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "anonx/telegram_client.hpp"

namespace anonx {

class Userbot {
public:
    struct AssistantSpec {
        std::string name = "assistant";   // label, e.g. "AnonyUB1"
        std::string phoneNumber;          // needed for first-run login
        std::string sessionDirectory;     // TDLib db dir; unique per assistant
        // Optional overrides for the interactive login prompts.
        std::function<std::string()> codeProvider;
        std::function<std::string()> passwordProvider;
    };

    Userbot(int apiId, std::string apiHash);
    ~Userbot();

    Userbot(const Userbot&) = delete;
    Userbot& operator=(const Userbot&) = delete;

    void addAssistant(AssistantSpec spec);
    std::size_t count() const { return specs_.size(); }

    // Where "Assistant Started" is announced (0 = don't announce).
    void setLoggerChatId(std::int64_t id);
    // Public @username the assistants should join on boot (empty = skip).
    void setSupportChat(std::string username);
    // Prompt on stdin for login code / 2FA when TDLib asks (default: true).
    void setInteractiveLogin(bool on);

    // Construct and boot every assistant. Returns true only if all authorized.
    bool bootAll(int timeoutMs = 180000);
    void exitAll();

    // 0-based access to a booted assistant (nullptr if out of range).
    TelegramClient* at(std::size_t index);
    const std::vector<std::unique_ptr<TelegramClient>>& clients() const { return clients_; }

private:
    void announce(TelegramClient& client);
    void joinSupport(TelegramClient& client);

    int apiId_;
    std::string apiHash_;
    std::vector<AssistantSpec> specs_;
    std::vector<std::unique_ptr<TelegramClient>> clients_;

    std::int64_t loggerChatId_ = 0;
    std::string supportChat_;
    bool interactive_ = true;
};

}  // namespace anonx

#endif  // ANONX_USERBOT_HPP
