// AnonXMusic C++ port — Integration phase
// null_voice_transport.hpp — a VoiceTransport that cannot stream.
//
// The build matrix has four corners, and one of them needs an answer: TDLib is
// available (so the bot really connects to Telegram) but NTgCalls is not, because
// it is opt-in and pulls in WebRTC. Runtime takes a VoiceTransport& and there is
// no third implementation for production — the fake belongs to the tests.
//
// This fills that corner honestly. Every non-voice command works; a /play
// reports ServerError, which CallManager already handles by stopping the chat and
// showing the "error_tg_server" notice. That is a visible, localized failure
// rather than a crash or a silent no-op, and the log line at boot says why.
//
// Header-only and dependency-free, so it costs a default build nothing.

#ifndef ANONX_NULL_VOICE_TRANSPORT_HPP
#define ANONX_NULL_VOICE_TRANSPORT_HPP

#include <cstdint>
#include <utility>

#include "anonx/voice_transport.hpp"

namespace anonx {

class NullVoiceTransport : public VoiceTransport {
public:
    PlayResult play(std::int64_t, const MediaSource&) override {
        return PlayResult::ServerError;
    }

    // "Not in call" is the truthful answer, and it is what makes CallManager
    // clear the chat's state instead of leaving it stuck as active.
    bool pause(std::int64_t) override { return false; }
    bool resume(std::int64_t) override { return false; }

    void stop(std::int64_t) override {}
    double ping() const override { return 0.0; }

    // Stored but never invoked: nothing can start, so no stream can end.
    void setStreamEndHandler(StreamEndHandler handler) override {
        onStreamEnd_ = std::move(handler);
    }
    void setCallClosedHandler(CallClosedHandler handler) override {
        onCallClosed_ = std::move(handler);
    }

private:
    StreamEndHandler  onStreamEnd_;
    CallClosedHandler onCallClosed_;
};

}  // namespace anonx

#endif  // ANONX_NULL_VOICE_TRANSPORT_HPP
