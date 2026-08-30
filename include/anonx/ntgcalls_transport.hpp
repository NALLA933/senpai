// AnonXMusic C++ port — Phase 5 (voice + queue)
// ntgcalls_transport.hpp — real voice engine backed by NTgCalls.
//
// This is the production VoiceTransport, wrapping the NTgCalls C++/WebRTC
// library (the same engine py-tgcalls uses). It is OPT-IN: the implementation
// in ntgcalls_transport.cpp is only compiled when the project is configured
// with -DANONX_WITH_NTGCALLS=ON, which also links the NTgCalls + WebRTC
// dependencies. Offline builds and the test suite use FakeVoiceTransport
// instead, so nothing here is required to verify the queue/orchestration logic.
//
// The NTgCalls headers are confined to the .cpp via a PIMPL, so this public
// header stays dependency-free (STL only), exactly like td_client.hpp hides
// the TDLib headers in Phase 4.
//
// Signaling seam: NTgCalls only *produces* the local WebRTC parameters and
// *consumes* the remote ones — it does not talk to Telegram. The actual
// phone.joinGroupCall / phone.leaveGroupCall must be issued over the assistant
// account's MTProto connection (the TelegramClient from Phase 4). Those two
// calls are injected as `Signaling` callbacks so this transport stays decoupled
// from the Telegram client and remains unit-testable in isolation.

#ifndef ANONX_NTGCALLS_TRANSPORT_HPP
#define ANONX_NTGCALLS_TRANSPORT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "anonx/voice_transport.hpp"

namespace anonx {

class NtgCallsTransport : public VoiceTransport {
public:
    // The MTProto join/leave seam (see the header comment). Both run on the
    // owning assistant account.
    struct Signaling {
        // Send our local join parameters for `chatId` (phone.joinGroupCall) and
        // return Telegram's remote parameters. Throw VoiceError{...} to report a
        // categorized failure (e.g. NoActiveGroupCall / ServerError).
        std::function<std::string(std::int64_t chatId, const std::string& localParams)>
            joinGroupCall;

        // Leave the chat's group call (phone.leaveGroupCall). Must not throw.
        std::function<void(std::int64_t chatId)> leaveGroupCall;
    };

    explicit NtgCallsTransport(Signaling signaling);
    ~NtgCallsTransport() override;

    NtgCallsTransport(const NtgCallsTransport&)            = delete;
    NtgCallsTransport& operator=(const NtgCallsTransport&) = delete;

    // VoiceTransport interface.
    PlayResult play(std::int64_t chatId, const MediaSource& src) override;
    bool       pause(std::int64_t chatId) override;
    bool       resume(std::int64_t chatId) override;
    void       stop(std::int64_t chatId) override;
    double     ping() const override;
    void       setStreamEndHandler(StreamEndHandler handler) override;
    void       setCallClosedHandler(CallClosedHandler handler) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anonx

#endif  // ANONX_NTGCALLS_TRANSPORT_HPP
