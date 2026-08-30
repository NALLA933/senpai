// AnonXMusic C++ port — Phase 5 (voice + queue)
// call_manager.hpp — playback orchestration.
//
// A faithful port of the `TgCall` class in anony/core/calls.py. CallManager
// ties together three collaborators, all injected so the whole thing is
// testable offline:
//
//   * VoiceTransport — the voice-chat engine (real NTgCalls, or a fake in
//     tests). CallManager only ever talks to this abstract interface.
//   * Queue          — the per-chat playlist (Phase 5).
//   * CacheManager   — ephemeral call state: is-active / playing-paused / loop
//     count (Phase 1; already models MongoDB.active_calls + MongoDB.loop).
//
// Everything Telegram-specific (downloading media, editing the "now playing"
// card, deleting old messages) is delegated through a small set of callbacks
// in `Callbacks`, so this class carries no dependency on the Telegram layer and
// can be driven by a test harness with plain lambdas. Phase 6's command plugins
// will supply the real implementations.
//
// Concurrency: methods are guarded by a *per-chat* recursive mutex. Different
// chats proceed fully in parallel (important at 100k-group scale), while all
// operations for a single chat are serialized. The mutex is recursive because
// the auto-advance path legitimately re-enters (playNext -> replay -> playMedia
// -> playNext, and skip-on-download-failure recurses into playNext).

#ifndef ANONX_CALL_MANAGER_HPP
#define ANONX_CALL_MANAGER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "anonx/cache_manager.hpp"
#include "anonx/config.hpp"
#include "anonx/queue.hpp"
#include "anonx/timer.hpp"
#include "anonx/voice_transport.hpp"

namespace anonx {

class CallManager {
public:
    // Stable identifiers for the transient / status / error messages the bot
    // shows. Each maps to a language key in the Python bot; the Phase 6 wiring
    // translates a Notice into the localized text for the chat.
    enum class Notice {
        PlayAgain,     // "play_again"       — shown before a looped replay
        PlayNext,      // "play_next"        — shown before advancing the queue
        ErrorNoFile,   // "error_no_file"    — media file missing / undownloadable
        ErrorNoCall,   // "error_no_call"    — no active voice chat
        ErrorNoAudio,  // "error_no_audio"   — no decodable audio in the media
        ErrorServer,   // "error_tg_server"  — connection/timeout/parse failure
        ErrorRtmp,     // "error_rtmp"       — RTMP streaming unsupported
        AutoLeft,      // "auto_left"        — bot left the chat due to inactivity
    };

    // How a play() request was handled.
    enum class PlayOutcome {
        StartedNow,  // playback of this item began immediately
        Queued,      // a call was already active; the item was queued behind it
        Aborted,     // the download was cancelled or refused (Phase 7.2); the
                     // item was dropped again and the user has already been told
    };

    struct PlayDecision {
        PlayOutcome outcome;
        int         position;  // 0-based index in the queue (0 == current)
    };

    // What a fetch attempt produced (Phase 7.2). `aborted` means the user
    // cancelled the download, or it was refused before it started (too large,
    // already running) — and that the Telegram layer has ALREADY said so, so the
    // engine must stay quiet instead of adding its own "no file" error on top.
    struct MediaFetch {
        std::optional<std::string> path;
        bool                       aborted = false;
    };

    // Telegram-layer hooks. All are optional; a null callback is simply skipped
    // (handy in tests). Supplied by Phase 6 in production.
    struct Callbacks {
        // Fetch a media file for one queued item. Returns its local path, or an
        // empty path on failure. Mirrors yt.download — but takes the chat as well,
        // because Phase 7.2 reports the download's progress into it.
        std::function<MediaFetch(std::int64_t chatId, const MediaItem& item)> download;

        // Render/update the "now playing" card for the current track and return
        // its message id (0 if none). Mirrors the edit_media/edit_text block in
        // TgCall.play_media. Called once when a track actually starts.
        std::function<std::int64_t(std::int64_t chatId, const MediaItem& media)> onNowPlaying;

        // Show a transient status or error message. Mirrors the various
        // message.edit_text(_lang[...]) / send_message calls.
        std::function<void(std::int64_t chatId, Notice notice)> onNotice;

        // Delete a previously sent message (app.delete_messages).
        std::function<void(std::int64_t chatId, std::int64_t messageId)> onDeleteMessage;
    };

    // The collaborators are borrowed references and must outlive the
    // CallManager. Wiring the transport's update handlers happens here.
    CallManager(VoiceTransport& transport, Queue& queue, CacheManager& cache,
                Timer& timer, const Config& config);

    // Non-copyable / non-movable (holds mutexes, registers callbacks with the
    // transport that capture `this`).
    CallManager(const CallManager&)            = delete;
    CallManager& operator=(const CallManager&) = delete;
    CallManager(CallManager&&)                 = delete;
    CallManager& operator=(CallManager&&)      = delete;

    // Install the Telegram-layer callbacks (Phase 6). Safe to call once at boot.
    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // -----------------------------------------------------------------
    // Public playback API (ports of the TgCall methods + the play.py
    // enqueue-vs-start decision).
    // -----------------------------------------------------------------

    // Add `item` to the chat and either start it now or queue it, reproducing
    // the decision in plugins/play.py:
    //   * force            -> force_add (replace current) and start immediately
    //   * position 0 & idle -> start immediately
    //   * otherwise         -> queued (returns its position)
    // On a start, the media file is fetched on demand if not already present; a
    // fetch the user cancelled drops the item again and returns Aborted.
    PlayDecision play(std::int64_t chatId, MediaItem item, bool force = false);

    // Start streaming `media` in the chat. `seekTime` > 1 seeks that many
    // seconds into the track (ffmpeg -ss); a fresh play uses seekTime == 0.
    // Port of TgCall.play_media (minus the Telegram plumbing).
    void playMedia(std::int64_t chatId, MediaItem media, int seekTime = 0);

    // Replay the current track (used by looping). Port of TgCall.replay.
    void replay(std::int64_t chatId);

    // Advance to the next track, honoring the loop counter and downloading on
    // demand; stops the chat when the queue is exhausted. Port of
    // TgCall.play_next.
    void playNext(std::int64_t chatId);

    // Pause / resume the current stream. Updates the cached playing state and,
    // if the engine reports it is no longer in the call, stops the chat.
    // Return the engine's success flag. Ports of TgCall.pause / TgCall.resume.
    bool pause(std::int64_t chatId);
    bool resume(std::int64_t chatId);

    // Stop playback: clear the queue, drop the call + loop state, and leave the
    // voice chat (errors swallowed). Port of TgCall.stop.
    void stop(std::int64_t chatId);

    // Average ping across assistant connections. Port of TgCall.ping.
    double ping() const { return transport_.ping(); }

private:
    // Ensure `item.file_path` is populated, downloading on demand. Mirrors the
    // cache-file/yt.download step play.py runs before the first play_media.
    // Returns true when the user aborted the fetch, in which case the caller must
    // stay silent (the Telegram layer has already explained itself).
    bool ensureFilePath(std::int64_t chatId, MediaItem& item);

    // Per-chat recursive lock (see the class comment on concurrency).
    std::unique_lock<std::recursive_mutex> lockFor(std::int64_t chatId);

    VoiceTransport& transport_;
    Queue&          queue_;
    CacheManager&   cache_;
    Timer&          timer_;
    const Config&   config_;
    Callbacks       cb_;

    mutable std::mutex locksMtx_;
    std::unordered_map<std::int64_t, std::unique_ptr<std::recursive_mutex>> locks_;
};

}  // namespace anonx

#endif  // ANONX_CALL_MANAGER_HPP
