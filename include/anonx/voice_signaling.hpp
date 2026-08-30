// AnonXMusic C++ port — Integration phase
// voice_signaling.hpp — the MTProto half of joining a voice chat.
//
// NTgCalls only produces and consumes WebRTC parameters; it never talks to
// Telegram. Somebody has to carry those parameters over an *assistant account's*
// MTProto connection (phone.joinGroupCall / phone.leaveGroupCall) — that is the
// seam NtgCallsTransport::Signaling describes, and this file fills it using the
// Phase 4 TelegramClient.
//
// It lives in the Telegram layer, not the voice layer, so anonx_voice keeps its
// property of linking nothing but headers and Threads.
//
// TDLib naming, and why both names are tried: the function is `joinGroupCall` in
// TDLib <= 1.8.14 and was renamed `joinVideoChat` in later builds (with
// `leaveGroupCall` -> `leaveVideoChat`). Rather than pin one and fail obscurely
// on the other, the implementation sends the first name and retries with the
// second when TDLib reports the method as unknown. The chosen name is then
// remembered for the process.

#ifndef ANONX_VOICE_SIGNALING_HPP
#define ANONX_VOICE_SIGNALING_HPP

#include <functional>

#include "anonx/ntgcalls_transport.hpp"
#include "anonx/telegram_client.hpp"

namespace anonx {

// Build join/leave callbacks bound to `assistant`, which must be a booted USER
// account (a bot cannot join a voice chat) and must outlive the transport that
// holds the callbacks.
//
// joinGroupCall throws VoiceError to report a category CallManager understands:
//   * NoActiveGroupCall — the chat has no open voice chat, or the assistant is
//     not in the chat at all;
//   * ServerError       — TDLib refused the join, or its reply was unparseable.
NtgCallsTransport::Signaling makeAssistantSignaling(TelegramClient& assistant);

// Same thing, one level of indirection later — and the entrypoint needs it,
// because the dependency is circular: NtgCallsTransport is constructed WITH the
// signaling, Runtime is constructed with the transport, and the assistant the
// signaling runs on is booted BY the Runtime. `provider` breaks the cycle: it is
// called on the first join (never during construction) and must return the
// booted assistant, or nullptr when none came up.
//
// The resolved assistant is remembered, so a chat's join and its later leave
// always run on the same account. Until one is available, joins fail with
// VoiceError(NoActiveGroupCall) — the same category a chat with no open voice
// chat produces, which is the closest honest answer: the bot cannot join.
NtgCallsTransport::Signaling makeDeferredAssistantSignaling(
    std::function<TelegramClient*()> provider);

}  // namespace anonx

#endif  // ANONX_VOICE_SIGNALING_HPP
