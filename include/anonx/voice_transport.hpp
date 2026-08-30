// AnonXMusic C++ port — Phase 5 (voice + queue)
// voice_transport.hpp — abstract voice-chat transport.
//
// This is the seam between the bot's playback orchestration (CallManager) and
// the actual WebRTC/voice-chat engine (NTgCalls, driven through an assistant
// account's MTProto connection). In the Python bot, `TgCall` called into
// pytgcalls/PyTgCalls directly; here we hide that behind a small abstract
// interface so that:
//
//   * the real engine (NtgCallsTransport) can be linked in only when built
//     with -DANONX_WITH_NTGCALLS=ON (it pulls in the NTgCalls + WebRTC deps),
//   * and the queue / loop / auto-advance logic in CallManager can be unit
//     tested completely offline against a scripted FakeVoiceTransport — no
//     network, no native libraries.
//
// The interface deliberately mirrors only what `anony/core/calls.py` actually
// uses: play / pause / resume / stop / ping, plus the two update callbacks
// (stream-ended and voice-chat-closed). Seek is expressed by re-issuing play()
// with a seek offset, exactly as the Python `play_media(..., seek_time=...)`
// does — so no separate "change stream" call is needed.
//
// Header-only, dependency-free (STL + <functional> only). Real engine headers
// never appear here; they are confined to ntgcalls_transport.cpp.

#ifndef ANONX_VOICE_TRANSPORT_HPP
#define ANONX_VOICE_TRANSPORT_HPP

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace anonx {

// Audio quality presets (subset of pytgcalls types.AudioQuality actually used).
enum class AudioQuality { Low, Medium, High };

// Video quality presets (subset of pytgcalls types.VideoQuality actually used).
enum class VideoQuality { SD_360p, SD_480p, HD_720p, FHD_1080p };

// Describes one stream to play. Mirrors pytgcalls `types.MediaStream` fields as
// used by TgCall.play_media: a local media path (or URL for m3u8), whether to
// enable video, an optional ffmpeg seek offset, and the quality presets.
struct MediaSource {
    std::string  path;                              // media_path (file or URL)
    bool         video = false;                     // enable a video stream
    int          seekSeconds = 0;                   // ffmpeg "-ss N" when > 1
    AudioQuality audio = AudioQuality::High;         // audio_parameters
    VideoQuality videoQuality = VideoQuality::HD_720p;  // video_parameters
};

// Which kind of stream ended. Only AUDIO triggers auto-advance in the Python
// bot (types.StreamEnded.Type.AUDIO -> play_next).
enum class StreamKind { Audio, Video };

// Outcome of a play() attempt. Each value corresponds to one branch of the
// exception handling in TgCall.play_media, so CallManager can react exactly as
// the Python bot did without the transport leaking engine-specific exceptions.
enum class PlayResult {
    Ok,                 // stream started
    FileNotFound,       // media file missing         -> notice + play_next
    NoActiveGroupCall,  // no voice chat is open       -> stop + notice
    NoAudioSource,      // no decodable audio in media -> notice + play_next
    ServerError,        // connection/timeout/parse    -> stop + notice
    RtmpUnsupported,    // RTMP stream not supported   -> stop + notice
};

// A categorized voice failure. The real engine and its MTProto join-signaling
// callback throw this to report a specific PlayResult category (e.g. the
// signaling layer throws VoiceError{NoActiveGroupCall} when Telegram reports no
// open voice chat); the transport's play() catches it and returns the category.
// This keeps engine/network exceptions from leaking past the VoiceTransport
// boundary, mirroring how TgCall.play_media mapped each exception to a branch.
struct VoiceError : std::runtime_error {
    PlayResult category;
    explicit VoiceError(PlayResult cat, const std::string& what = "voice error")
        : std::runtime_error(what), category(cat) {}
};

// Abstract voice-chat transport. One instance manages every chat the bot is
// streaming in (the underlying engine keeps per-chat call state, keyed by the
// Telegram chat id). Implementations must be safe to call from multiple
// threads and may invoke the update handlers from their own internal thread.
class VoiceTransport {
public:
    using StreamEndHandler  = std::function<void(std::int64_t chatId, StreamKind kind)>;
    using CallClosedHandler = std::function<void(std::int64_t chatId)>;

    virtual ~VoiceTransport() = default;

    // Join (if needed) the chat's voice chat and begin streaming `src`.
    // Corresponds to PyTgCalls.play(..., GroupCallConfig(auto_start=False)).
    // Returns a PlayResult describing success or the failure category.
    virtual PlayResult play(std::int64_t chatId, const MediaSource& src) = 0;

    // Pause / resume the current stream. Returns true on success; false means
    // the engine reported "not in call" / "connection not found", which the
    // caller treats as a signal to stop the chat entirely.
    virtual bool pause(std::int64_t chatId)  = 0;
    virtual bool resume(std::int64_t chatId) = 0;

    // Leave the chat's voice chat. Must be idempotent and must not throw;
    // implementations swallow "not in call" style errors (matches the Python
    // stop(), which wraps leave_call in a bare try/except).
    virtual void stop(std::int64_t chatId) = 0;

    // Average round-trip ping (ms) across all assistant connections, mirroring
    // TgCall.ping (mean of each client's ping). Returns 0 when idle.
    virtual double ping() const = 0;

    // Register the callback fired when a stream finishes on its own. The engine
    // calls this with StreamKind::Audio when a track plays to completion; the
    // orchestrator responds by advancing the queue.
    virtual void setStreamEndHandler(StreamEndHandler handler) = 0;

    // Register the callback fired when the bot is removed from a chat's voice
    // chat externally (kicked, left, or the voice chat was closed). The
    // orchestrator responds by stopping playback for that chat.
    virtual void setCallClosedHandler(CallClosedHandler handler) = 0;
};

}  // namespace anonx

#endif  // ANONX_VOICE_TRANSPORT_HPP
