// AnonXMusic C++ port — Phase 4
// td_client.hpp — low-level RAII wrapper around the TDLib JSON interface.
//
// TDLib (https://github.com/tdlib/td) is the official Telegram client library.
// The original bot uses kurigram/Pyrogram; the closest C++ equivalent is TDLib,
// which is the only option that supports *userbot* accounts and raw MTProto for
// group calls (the Bot API / tgbot-cpp cannot join voice chats).
//
// We drive TDLib through its JSON interface (a stable C ABI):
//     td_create_client_id(), td_send(), td_receive(), td_execute()
// Requests and responses are plain JSON strings, which keeps this header free of
// any TDLib or nlohmann::json dependency — JSON is handled only in the .cpp.
//
// A single process-wide receive pump (one background thread, as TDLib requires)
// routes every incoming object to the owning client by its "@client_id". Each
// request sent via invoke() gets a unique "@extra" so its response can be matched
// back to the caller; everything else is delivered to the update handler.

#ifndef ANONX_TD_CLIENT_HPP
#define ANONX_TD_CLIENT_HPP

#include <cstdint>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace anonx {

class TdClient {
public:
    using UpdateHandler = std::function<void(const std::string& updateJson)>;

    TdClient();
    ~TdClient();

    TdClient(const TdClient&) = delete;
    TdClient& operator=(const TdClient&) = delete;

    int clientId() const { return clientId_; }

    // Set the callback for asynchronous updates (everything that is not a direct
    // response to invoke()). Updates that arrived before a handler was installed
    // are buffered and flushed here, so no early update is lost.
    void setUpdateHandler(UpdateHandler handler);

    // Send a request and block until its response arrives (matched by "@extra"),
    // or until the timeout elapses. Returns the response JSON; on timeout returns
    // a synthesized {"@type":"error","code":408,...}. requestJson must be a JSON
    // object; an "@extra" field is added automatically.
    std::string invoke(const std::string& requestJson, int timeoutMs = 30000);

    // Fire-and-forget send (no response correlation).
    void send(const std::string& requestJson);

    // Synchronous TDLib call (e.g. setLogVerbosityLevel). Static per the C ABI.
    static std::string execute(const std::string& requestJson);

    // Stop and join the shared receive pump. Call once before process exit.
    static void stopRuntime();

    // Called by the receive pump for each object addressed to this client.
    // Public only so the pump can reach it; treat as internal.
    void onIncoming(std::string json);

private:
    int clientId_ = 0;

    std::mutex pendingMutex_;
    std::unordered_map<std::string, std::shared_ptr<std::promise<std::string>>> pending_;
    std::atomic<std::uint64_t> extraSeq_{0};

    std::mutex handlerMutex_;
    UpdateHandler handler_;
    std::vector<std::string> updateBuffer_;
};

}  // namespace anonx

#endif  // ANONX_TD_CLIENT_HPP
