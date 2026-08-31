// AnonXMusic C++ port — Integration phase
// voice_signaling.cpp — see voice_signaling.hpp.

#include "anonx/voice_signaling.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "anonx/logger.hpp"

#include "anonx/utils.hpp"

namespace anonx {
namespace {

using nlohmann::json;

Logger log() { return Logger("anonx.voice.signaling"); }


bool isError(const json& j) {
    return j.is_object() && anonx::utils::strField(j, "@type") == "error";
}

// TDLib reports an unimplemented method with code 400/404 and a message that
// names it. Used to decide whether the other spelling of the call is worth a try.
bool looksLikeUnknownMethod(const json& err) {
    const std::string msg = anonx::utils::strField(err, "message");
    return msg.find("Unknown") != std::string::npos ||
           msg.find("unknown") != std::string::npos ||
           msg.find("not supported") != std::string::npos;
}

// The audio SSRC NTgCalls put in its local parameters. Telegram needs it as
// `audio_source_id`; without it the join succeeds but no audio is ever routed.
std::int32_t audioSourceId(const json& params) {
    for (const char* key : {"ssrc", "source", "audio_source"}) {
        if (params.is_object() && params.contains(key) && params[key].is_number()) {
            return static_cast<std::int32_t>(anonx::utils::intField(params, key));
        }
    }
    return 0;
}

// Per-chat state we need at leave time (TDLib leaves by group-call id, not chat).
struct CallState {
    std::mutex mtx;
    std::unordered_map<std::int64_t, std::int32_t> groupCallIdByChat;

    void remember(std::int64_t chatId, std::int32_t groupCallId) {
        std::lock_guard<std::mutex> lk(mtx);
        groupCallIdByChat[chatId] = groupCallId;
    }
    std::int32_t take(std::int64_t chatId) {
        std::lock_guard<std::mutex> lk(mtx);
        const auto it = groupCallIdByChat.find(chatId);
        if (it == groupCallIdByChat.end()) return 0;
        const std::int32_t id = it->second;
        groupCallIdByChat.erase(it);
        return id;
    }
};

// Which spelling of the TDLib methods this build accepts (see the header note).
// 0 = not decided yet, 1 = *GroupCall, 2 = *VideoChat.
std::atomic<int> g_dialect{0};

const char* joinName(int dialect) {
    return dialect == 2 ? "joinVideoChat" : "joinGroupCall";
}
const char* leaveName(int dialect) {
    return dialect == 2 ? "leaveVideoChat" : "leaveGroupCall";
}

// Read the chat's currently open voice chat. Returns 0 when there is none.
std::int32_t activeGroupCallId(TelegramClient& client, std::int64_t chatId) {
    json req;
    req["@type"] = "getChat";
    req["chat_id"] = chatId;
    json chat = json::parse(client.raw().invoke(req.dump()), nullptr, false);
    if (chat.is_discarded() || !chat.is_object() || isError(chat)) return 0;

    // TDLib >= 1.8 calls it video_chat; older builds called it voice_chat.
    for (const char* key : {"video_chat", "voice_chat"}) {
        if (chat.contains(key) && chat[key].is_object()) {
            const std::int32_t id =
                static_cast<std::int32_t>(anonx::utils::intField(chat[key], "group_call_id"));
            if (id != 0) return id;
        }
    }
    return 0;
}

}  // namespace

NtgCallsTransport::Signaling makeAssistantSignaling(TelegramClient& assistant) {
    // Shared by both callbacks, and it must outlive them: the transport may hold
    // them for the whole process lifetime.
    auto state = std::make_shared<CallState>();
    TelegramClient* client = &assistant;

    NtgCallsTransport::Signaling sig;

    sig.joinGroupCall = [client, state](std::int64_t chatId,
                                       const std::string& localParams) -> std::string {
        const std::int32_t groupCallId = activeGroupCallId(*client, chatId);
        if (groupCallId == 0) {
            throw VoiceError(PlayResult::NoActiveGroupCall,
                             "no open voice chat in " + std::to_string(chatId));
        }

        json payload = json::parse(localParams, nullptr, false);
        if (payload.is_discarded()) {
            throw VoiceError(PlayResult::ServerError,
                             "voice engine produced unparseable join parameters");
        }

        json req;
        req["chat_id"] = chatId;              // ignored by the *GroupCall spelling
        req["group_call_id"] = groupCallId;
        json participant;
        participant["@type"] = "messageSenderUser";
        participant["user_id"] = client->me().id;
        req["participant_id"] = participant;
        req["audio_source_id"] = audioSourceId(payload);
        req["payload"] = localParams;         // TDLib takes the raw JSON string
        req["is_muted"] = false;
        req["is_my_video_enabled"] = false;
        req["invite_hash"] = "";

        int dialect = g_dialect.load();
        const int first = dialect == 0 ? 1 : dialect;
        json reply;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int tryDialect = attempt == 0 ? first : (first == 1 ? 2 : 1);
            req["@type"] = joinName(tryDialect);
            reply = json::parse(client->raw().invoke(req.dump(), 30000), nullptr, false);
            if (!reply.is_discarded() && !isError(reply)) {
                g_dialect.store(tryDialect);
                break;
            }
            // Only the "no such method" error is worth retrying with the other
            // spelling; a real refusal (no rights, call full) must surface.
            if (reply.is_discarded() || !looksLikeUnknownMethod(reply) ||
                dialect != 0) {
                break;
            }
        }

        if (reply.is_discarded() || isError(reply)) {
            const std::string msg =
                reply.is_discarded() ? "unparseable reply" : anonx::utils::strField(reply, "message");
            log().warning("join failed for chat " + std::to_string(chatId) + ": " + msg);
            throw VoiceError(PlayResult::ServerError, "join rejected: " + msg);
        }

        state->remember(chatId, groupCallId);

        // TDLib answers with text{ text: <remote parameters JSON> }.
        const std::string remote = anonx::utils::strField(reply, "text");
        if (remote.empty()) {
            throw VoiceError(PlayResult::ServerError, "join returned no parameters");
        }
        return remote;
    };

    sig.leaveGroupCall = [client, state](std::int64_t chatId) {
        std::int32_t groupCallId = state->take(chatId);
        if (groupCallId == 0) groupCallId = activeGroupCallId(*client, chatId);
        if (groupCallId == 0) return;   // nothing to leave; stop() must not throw

        const int dialect = g_dialect.load();
        json req;
        req["@type"] = leaveName(dialect == 0 ? 1 : dialect);
        req["group_call_id"] = groupCallId;
        client->raw().send(req.dump());
    };

    return sig;
}

NtgCallsTransport::Signaling makeDeferredAssistantSignaling(
    std::function<TelegramClient*()> provider) {
    // One lazily-built inner signaling, shared by both callbacks and by every
    // call: building it twice would create two CallState maps, and a leave could
    // then look in the wrong one.
    struct Lazy {
        std::mutex mtx;
        std::function<TelegramClient*()> provider;
        NtgCallsTransport::Signaling inner;
        bool bound = false;

        // Returns nullptr while no assistant is up; retries on the next call.
        NtgCallsTransport::Signaling* resolve() {
            std::lock_guard<std::mutex> lk(mtx);
            if (!bound) {
                TelegramClient* client = provider ? provider() : nullptr;
                if (!client) return nullptr;
                inner = makeAssistantSignaling(*client);
                bound = true;
            }
            return &inner;
        }
    };

    auto lazy = std::make_shared<Lazy>();
    lazy->provider = std::move(provider);

    NtgCallsTransport::Signaling sig;

    sig.joinGroupCall = [lazy](std::int64_t chatId,
                               const std::string& localParams) -> std::string {
        NtgCallsTransport::Signaling* inner = lazy->resolve();
        if (!inner) {
            log().warning("join requested for chat " + std::to_string(chatId) +
                          " but no assistant account is available");
            throw VoiceError(PlayResult::NoActiveGroupCall,
                             "no assistant account is available to join with");
        }
        return inner->joinGroupCall(chatId, localParams);
    };

    sig.leaveGroupCall = [lazy](std::int64_t chatId) {
        // Never resolve here: if no join ever happened there is nothing to leave,
        // and stop() must not throw or block on a login.
        std::lock_guard<std::mutex> lk(lazy->mtx);
        if (lazy->bound && lazy->inner.leaveGroupCall) {
            lazy->inner.leaveGroupCall(chatId);
        }
    };

    return sig;
}

}  // namespace anonx
