// AnonXMusic C++ port — Phase 4
// userbot.cpp — assistant account manager.

#include "anonx/userbot.hpp"

#include <nlohmann/json.hpp>

#include <iostream>

#include "anonx/logger.hpp"

namespace anonx {
namespace {

using nlohmann::json;

Logger log() { return Logger("anonx.userbot"); }

// Prompt on stderr and read one line from stdin (used for first-run login).
std::function<std::string()> stdinPrompt(const std::string& prompt) {
    return [prompt]() -> std::string {
        std::fprintf(stderr, "\n\n=======================================================\n");
        std::fprintf(stderr, ">> %s", prompt.c_str());
        std::fprintf(stderr, "\n=======================================================\n\n");
        std::fflush(stderr);
        std::string line;
        if (!std::getline(std::cin, line)) return std::string();
        return line;
    };
}

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

Userbot::Userbot(int apiId, std::string apiHash)
    : apiId_(apiId), apiHash_(std::move(apiHash)) {}

Userbot::~Userbot() {
    exitAll();
}

void Userbot::addAssistant(AssistantSpec spec) {
    if (spec.sessionDirectory.empty()) {
        spec.sessionDirectory = "tdlib/" + spec.name;
    }
    specs_.push_back(std::move(spec));
}

void Userbot::setLoggerChatId(std::int64_t id) { loggerChatId_ = id; }
void Userbot::setSupportChat(std::string username) { supportChat_ = std::move(username); }
void Userbot::setInteractiveLogin(bool on) { interactive_ = on; }

bool Userbot::bootAll(int timeoutMs) {
    if (clients_.empty() && !specs_.empty()) {
        for (const auto& spec : specs_) {
            TelegramClient::Options opts;
            opts.apiId = apiId_;
            opts.apiHash = apiHash_;
            opts.databaseDirectory = spec.sessionDirectory;
            opts.phoneNumber = spec.phoneNumber;
            opts.name = spec.name;

            if (spec.codeProvider) {
                opts.codeProvider = spec.codeProvider;
            } else if (interactive_) {
                opts.codeProvider = stdinPrompt("Enter login code for " + spec.name + ": ");
            }
            if (spec.passwordProvider) {
                opts.passwordProvider = spec.passwordProvider;
            } else if (interactive_) {
                opts.passwordProvider =
                    stdinPrompt("Enter 2FA password for " + spec.name + " (blank if none): ");
            }

            clients_.push_back(std::unique_ptr<TelegramClient>(new TelegramClient(std::move(opts))));
        }
    }

    bool allOk = true;
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        TelegramClient& c = *clients_[i];
        if (!c.boot(timeoutMs)) {
            log().error(specs_[i].name + " failed to start");
            allOk = false;
            continue;
        }
        announce(c);
        if (!supportChat_.empty()) joinSupport(c);
        log().info("Assistant " + std::to_string(i + 1) + " started");
    }
    return allOk;
}

void Userbot::exitAll() {
    for (auto& c : clients_) {
        if (c) c->exit();
    }
}

TelegramClient* Userbot::at(std::size_t index) {
    return index < clients_.size() ? clients_[index].get() : nullptr;
}

void Userbot::announce(TelegramClient& client) {
    if (loggerChatId_ != 0) {
        client.sendMessage(loggerChatId_, "<b>Assistant Started</b>");
    }
}

void Userbot::joinSupport(TelegramClient& client) {
    // Resolve the public username to a chat, then join it.
    json search;
    search["@type"] = "searchPublicChat";
    search["username"] = supportChat_;
    const std::string resp = client.raw().invoke(search.dump());

    json chat = json::parse(resp, nullptr, false);
    if (chat.is_discarded() || !chat.is_object() || strField(chat, "@type") != "chat") {
        log().warning(client.name() + ": could not resolve support chat @" + supportChat_);
        return;
    }
    const std::int64_t chatId = intField(chat, "id");
    if (chatId == 0) return;

    json join;
    join["@type"] = "joinChat";
    join["chat_id"] = chatId;
    client.raw().invoke(join.dump());
}

}  // namespace anonx
