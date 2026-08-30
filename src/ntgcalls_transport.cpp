// AnonXMusic C++ port — Phase 5 (voice + queue)
// ntgcalls_transport.cpp — NTgCalls-backed VoiceTransport (opt-in).
//
// Compiled only when the project is configured with -DANONX_WITH_NTGCALLS=ON.
// Offline/CI builds omit this translation unit entirely and use
// FakeVoiceTransport, so the queue/orchestration tests never need NTgCalls.
//
// API VERSION NOTE (please read before building for real):
//   This binds to the NTgCalls C++ API (namespace `ntgcalls`, header
//   <ntgcalls/ntgcalls.hpp>), targeting the ~v1.x surface. NTgCalls evolves
//   quickly and the *media-description construction*, *exception class names*,
//   and the *stream-end enum* are the parts most likely to differ between
//   versions. They are deliberately isolated in the clearly-marked helpers
//   below (buildMedia, the catch-ladder in play(), and the onStreamEnd lambda),
//   so adapting to a specific NTgCalls release means editing only those spots.
//   The control flow (create -> join over MTProto -> connect; pause/resume/
//   stop; exception -> PlayResult; stream-end -> auto-advance) is stable and
//   matches anony/core/calls.py.

#include "anonx/ntgcalls_transport.hpp"

#include <mutex>
#include <string>

#if defined(ANONX_WITH_NTGCALLS)

// ---------------------------------------------------------------------------
// Real implementation (requires the NTgCalls library + its WebRTC deps).
// ---------------------------------------------------------------------------
#include <ntgcalls/ntgcalls.hpp>

namespace anonx {
namespace {

// Map our quality presets to concrete audio parameters (Hz / bits / channels).
void audioParamsFor(AudioQuality q, int& sampleRate, int& bits, int& channels) {
    bits     = 16;
    channels = 2;
    switch (q) {
        case AudioQuality::Low:    sampleRate = 24000; break;
        case AudioQuality::Medium: sampleRate = 36000; break;
        case AudioQuality::High:   sampleRate = 48000; break;
    }
}

// Map our video presets to width/height/fps.
void videoParamsFor(VideoQuality q, int& w, int& h, int& fps) {
    fps = 30;
    switch (q) {
        case VideoQuality::SD_360p:   w = 640;  h = 360;  break;
        case VideoQuality::SD_480p:   w = 854;  h = 480;  break;
        case VideoQuality::HD_720p:   w = 1280; h = 720;  break;
        case VideoQuality::FHD_1080p: w = 1920; h = 1080; break;
    }
}

// Build the NTgCalls media description from our MediaSource, using ffmpeg to
// decode the file/URL into the raw PCM/frame streams NTgCalls expects. The seek
// offset (-ss) is placed before -i for fast seeking, matching the Python
// `ffmpeg_parameters=f"-ss {seek_time}"` (applied only when > 1 second).
//
// *** Version-sensitive: field/enum names here may need tweaks per NTgCalls. ***
ntgcalls::MediaDescription buildMedia(const MediaSource& s) {
    const std::string seek =
        s.seekSeconds > 1 ? ("-ss " + std::to_string(s.seekSeconds) + " ") : "";

    ntgcalls::MediaDescription desc;

    int aRate, aBits, aChans;
    audioParamsFor(s.audio, aRate, aBits, aChans);
    ntgcalls::AudioDescription audio;
    audio.mediaSource   = ntgcalls::BaseMediaDescription::MediaSource::Shell;
    audio.sampleRate    = static_cast<uint32_t>(aRate);
    audio.bitsPerSample = static_cast<uint8_t>(aBits);
    audio.channelCount  = static_cast<uint8_t>(aChans);
    audio.input = "ffmpeg -nostdin " + seek + "-i \"" + s.path +
                  "\" -f s16le -ac " + std::to_string(aChans) +
                  " -ar " + std::to_string(aRate) + " -loglevel quiet pipe:1";
    desc.audio = audio;

    if (s.video) {
        int vw, vh, vfps;
        videoParamsFor(s.videoQuality, vw, vh, vfps);
        ntgcalls::VideoDescription video;
        video.mediaSource = ntgcalls::BaseMediaDescription::MediaSource::Shell;
        video.width       = static_cast<uint16_t>(vw);
        video.height      = static_cast<uint16_t>(vh);
        video.fps         = static_cast<uint8_t>(vfps);
        video.input = "ffmpeg -nostdin " + seek + "-i \"" + s.path +
                      "\" -f rawvideo -pix_fmt yuv420p -vf scale=" +
                      std::to_string(vw) + ":" + std::to_string(vh) +
                      " -r " + std::to_string(vfps) + " -loglevel quiet pipe:1";
        desc.video = video;
    }
    return desc;
}

}  // namespace

struct NtgCallsTransport::Impl {
    explicit Impl(Signaling sig) : signaling(std::move(sig)) {
        // A finished stream -> auto-advance. NTgCalls reports the stream device
        // that ended; only a finished playback (audio) should advance the queue,
        // matching types.StreamEnded.Type.AUDIO in calls.py.
        // *** Version-sensitive: the enum type/paths may differ. ***
        instance.onStreamEnd(
            [this](std::int64_t chatId, ntgcalls::StreamManager::Type type) {
                StreamKind kind = (type == ntgcalls::StreamManager::Type::Playback)
                                      ? StreamKind::Audio
                                      : StreamKind::Video;
                StreamEndHandler h;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    h = onStreamEnd;
                }
                if (h && kind == StreamKind::Audio)
                    h(chatId, kind);
            });

        // Connection dropped / closed remotely -> treat as a closed voice chat.
        instance.onConnectionChange(
            [this](std::int64_t chatId, ntgcalls::NetworkInfo state) {
                if (state.state == ntgcalls::ConnectionState::Closed ||
                    state.state == ntgcalls::ConnectionState::Failed ||
                    state.state == ntgcalls::ConnectionState::Timeout) {
                    CallClosedHandler h;
                    {
                        std::lock_guard<std::mutex> lk(mtx);
                        h = onCallClosed;
                    }
                    if (h)
                        h(chatId);
                }
            });
    }

    ntgcalls::NTgCalls              instance;
    Signaling                       signaling;
    mutable std::mutex              mtx;
    StreamEndHandler                onStreamEnd;
    CallClosedHandler               onCallClosed;
};

NtgCallsTransport::NtgCallsTransport(Signaling signaling)
    : impl_(std::make_unique<Impl>(std::move(signaling))) {}

NtgCallsTransport::~NtgCallsTransport() = default;

PlayResult NtgCallsTransport::play(std::int64_t chatId, const MediaSource& src) {
    try {
        auto media = buildMedia(src);
        // 1) NTgCalls produces our local WebRTC params.
        const std::string localParams = impl_->instance.createCall(chatId, std::move(media));
        // 2) Join the group call over MTProto (assistant account) -> remote params.
        //    The signaling layer throws VoiceError{NoActiveGroupCall} etc.
        const std::string remoteParams =
            impl_->signaling.joinGroupCall
                ? impl_->signaling.joinGroupCall(chatId, localParams)
                : throw VoiceError(PlayResult::ServerError, "no join signaling wired");
        // 3) Feed the remote params back to NTgCalls to finish the handshake.
        impl_->instance.connect(chatId, remoteParams);
        return PlayResult::Ok;
    }
    // Our categorized failures (thrown by the signaling callback) pass through.
    catch (const VoiceError& e) {
        return e.category;
    }
    // *** Version-sensitive: NTgCalls exception class names may differ. Map each
    // to the same PlayResult branch calls.py used; the final catch is the
    // catch-all "server error" fallback. ***
    catch (const ntgcalls::RTMPNeeded&)         { return PlayResult::RtmpUnsupported; }
    catch (const ntgcalls::FileError&)          { return PlayResult::FileNotFound; }
    catch (const ntgcalls::TelegramServerError&){ return PlayResult::ServerError; }
    catch (const ntgcalls::ConnectionError&)    { return PlayResult::ServerError; }
    catch (const std::exception&)               { return PlayResult::ServerError; }
}

bool NtgCallsTransport::pause(std::int64_t chatId) {
    try {
        return impl_->instance.pause(chatId);
    } catch (const std::exception&) {
        return false;  // not-in-call / connection-not-found -> caller stops
    }
}

bool NtgCallsTransport::resume(std::int64_t chatId) {
    try {
        return impl_->instance.resume(chatId);
    } catch (const std::exception&) {
        return false;
    }
}

void NtgCallsTransport::stop(std::int64_t chatId) {
    // Leave over MTProto first, then tear down the local NTgCalls state. Both
    // are best-effort (the Python stop() swallows leave_call errors).
    try {
        if (impl_->signaling.leaveGroupCall)
            impl_->signaling.leaveGroupCall(chatId);
    } catch (const std::exception&) {
    }
    try {
        impl_->instance.stop(chatId);
    } catch (const std::exception&) {
    }
}

double NtgCallsTransport::ping() const {
    // NTgCalls exposes per-connection stats that vary by version; wiring a real
    // average is left to the Phase 6 stats integration. Returns 0 when idle.
    return 0.0;
}

void NtgCallsTransport::setStreamEndHandler(StreamEndHandler handler) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->onStreamEnd = std::move(handler);
}

void NtgCallsTransport::setCallClosedHandler(CallClosedHandler handler) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->onCallClosed = std::move(handler);
}

}  // namespace anonx

#else  // !ANONX_WITH_NTGCALLS

// ---------------------------------------------------------------------------
// Stub used if this file is ever compiled without the NTgCalls dependency.
// It keeps the symbol defined but makes any attempt to actually stream fail
// loudly, so a misconfiguration is obvious rather than silent. (In the normal
// build the CMake target simply omits this .cpp when the option is OFF.)
// ---------------------------------------------------------------------------

namespace anonx {

struct NtgCallsTransport::Impl {
    explicit Impl(Signaling sig) : signaling(std::move(sig)) {}
    Signaling         signaling;
    StreamEndHandler  onStreamEnd;
    CallClosedHandler onCallClosed;
};

NtgCallsTransport::NtgCallsTransport(Signaling signaling)
    : impl_(std::make_unique<Impl>(std::move(signaling))) {}
NtgCallsTransport::~NtgCallsTransport() = default;

PlayResult NtgCallsTransport::play(std::int64_t, const MediaSource&) {
    // Built without NTgCalls: report a server error rather than pretending.
    return PlayResult::ServerError;
}
bool   NtgCallsTransport::pause(std::int64_t)  { return false; }
bool   NtgCallsTransport::resume(std::int64_t) { return false; }
void   NtgCallsTransport::stop(std::int64_t)   {}
double NtgCallsTransport::ping() const         { return 0.0; }
void   NtgCallsTransport::setStreamEndHandler(StreamEndHandler h)  { impl_->onStreamEnd = std::move(h); }
void   NtgCallsTransport::setCallClosedHandler(CallClosedHandler h){ impl_->onCallClosed = std::move(h); }

}  // namespace anonx

#endif  // ANONX_WITH_NTGCALLS
