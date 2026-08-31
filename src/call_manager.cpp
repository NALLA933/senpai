// AnonXMusic C++ port — Phase 5 (voice + queue)
// call_manager.cpp — implementation of the TgCall port.

#include "anonx/call_manager.hpp"

#include <utility>

namespace anonx {

CallManager::CallManager(VoiceTransport& transport, Queue& queue, CacheManager& cache,
                         Timer& timer, const Config& config)
    : transport_(transport), queue_(queue), cache_(cache), timer_(timer), config_(config) {
    // Wire the engine's update callbacks to our orchestration, mirroring
    // TgCall.decorators: a finished AUDIO stream advances the queue; being
    // kicked / leaving / a closed voice chat stops playback.
    transport_.setStreamEndHandler([this](std::int64_t chatId, StreamKind kind) {
        if (kind == StreamKind::Audio)
            playNext(chatId);
    });
    transport_.setCallClosedHandler([this](std::int64_t chatId) {
        stop(chatId);
    });
}

std::unique_lock<std::recursive_mutex> CallManager::lockFor(std::int64_t chatId) {
    std::recursive_mutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lk(locksMtx_);
        auto& slot = locks_[chatId];
        if (!slot)
            slot = std::make_unique<std::recursive_mutex>();
        m = slot.get();
    }
    return std::unique_lock<std::recursive_mutex>(*m);
}

bool CallManager::ensureFilePath(std::int64_t chatId, MediaItem& item) {
    if (!item.file_path.empty() || !cb_.download)
        return false;

    // YouTube::download already returns an existing download without
    // re-fetching, so this also covers play.py's "downloads/<id>.<ext>
    // exists" fast path.
    const MediaFetch fetch = cb_.download(chatId, item);
    if (fetch.path)
        item.file_path = *fetch.path;
    return fetch.aborted;
}

CallManager::PlayDecision
CallManager::play(std::int64_t chatId, MediaItem item, bool force) {
    auto lk = lockFor(chatId);

    if (force) {
        // forceAdd DROPS the item it displaces, so remember it: if the fetch is
        // cancelled the displaced track has to go back, because the transport is
        // still streaming it.
        const std::optional<MediaItem> displaced = queue_.getCurrent(chatId);
        queue_.forceAdd(chatId, item);
        if (ensureFilePath(chatId, item)) {
            // The user cancelled the download of the track they just forced, so
            // put the queue back exactly as it was. Whatever was streaming keeps
            // streaming — the transport was never touched.
            if (displaced)
                queue_.replaceCurrent(chatId, *displaced);
            else
                queue_.removeCurrent(chatId);
            return {PlayOutcome::Aborted, 0};
        }
        playMedia(chatId, item);
        return {PlayOutcome::StartedNow, 0};
    }

    int position = queue_.add(chatId, item);
    // play.py: queue if it isn't the head item, or a call is already running.
    if (position != 0)
        return {PlayOutcome::Queued, position};

    if (cache_.isActiveCall(chatId)) {
        // If position == 0 but we are active, we are idling! Cancel any auto-leave
        // or auto-end timer and start playback.
        timer_.cancel(chatId);
    }

    // Head of an idle (or emptied) chat -> start immediately.
    if (ensureFilePath(chatId, item)) {
        queue_.removeCurrent(chatId);
        return {PlayOutcome::Aborted, position};
    }
    playMedia(chatId, item);
    return {PlayOutcome::StartedNow, position};
}

void CallManager::playMedia(std::int64_t chatId, MediaItem media, int seekTime) {
    auto lk = lockFor(chatId);

    // Any new playback cancels pending inactivity timers.
    timer_.cancel(chatId);

    // No media file -> tell the user and skip to the next track.
    if (media.file_path.empty()) {
        if (cb_.onNotice)
            cb_.onNotice(chatId, Notice::ErrorNoFile);
        playNext(chatId);
        return;
    }

    MediaSource src;
    src.path         = media.file_path;
    src.video        = media.video;
    src.seekSeconds  = seekTime;              // transport applies "-ss" when > 1
    src.audio        = AudioQuality::High;    // types.AudioQuality.HIGH
    src.videoQuality = VideoQuality::HD_720p; // types.VideoQuality.HD_720p

    const PlayResult res = transport_.play(chatId, src);

    switch (res) {
        case PlayResult::Ok:
            // On a fresh play (not a seek) mark the call active and show the
            // now-playing card. Mirrors `if not seek_time:` in play_media.
            if (seekTime == 0) {
                media.time = 1;
                cache_.addCall(chatId);
                std::int64_t msgId = cb_.onNowPlaying ? cb_.onNowPlaying(chatId, media) : 0;
                media.message_id = msgId;
                // Persist time + message_id (and any downloaded file_path) back
                // onto the queue's current item, which we mutate in place in
                // the Python original.
                queue_.replaceCurrent(chatId, media);
            }
            break;

        case PlayResult::FileNotFound:
            if (cb_.onNotice)
                cb_.onNotice(chatId, Notice::ErrorNoFile);
            playNext(chatId);
            break;

        case PlayResult::NoActiveGroupCall:
            stop(chatId);
            if (cb_.onNotice)
                cb_.onNotice(chatId, Notice::ErrorNoCall);
            break;

        case PlayResult::NoAudioSource:
            if (cb_.onNotice)
                cb_.onNotice(chatId, Notice::ErrorNoAudio);
            playNext(chatId);
            break;

        case PlayResult::ServerError:
            stop(chatId);
            if (cb_.onNotice)
                cb_.onNotice(chatId, Notice::ErrorServer);
            break;

        case PlayResult::RtmpUnsupported:
            stop(chatId);
            if (cb_.onNotice)
                cb_.onNotice(chatId, Notice::ErrorRtmp);
            break;
    }
}

void CallManager::replay(std::int64_t chatId) {
    auto lk = lockFor(chatId);

    // Only replay if we still hold an active call (db.get_call check).
    if (!cache_.isActiveCall(chatId))
        return;

    auto media = queue_.getCurrent(chatId);
    if (!media)
        return;  // guard (the Python original would raise here; we no-op)

    if (cb_.onNotice)
        cb_.onNotice(chatId, Notice::PlayAgain);
    playMedia(chatId, *media);
}

void CallManager::playNext(std::int64_t chatId) {
    auto lk = lockFor(chatId);

    // Looping: consume one loop and replay the current track.
    if (int loop = cache_.getLoop(chatId); loop > 0) {
        cache_.setLoop(chatId, loop - 1);
        replay(chatId);
        return;
    }

    // Advance: pop the finished track, get the new head.
    auto next = queue_.getNext(chatId);

    // Delete a stale card carried on the new head, matching play_next's
    // best-effort delete of media.message_id.
    if (next && next->message_id != 0) {
        if (cb_.onDeleteMessage)
            cb_.onDeleteMessage(chatId, next->message_id);
        next->message_id = 0;
        queue_.replaceCurrent(chatId, *next);
    }

    // Queue exhausted -> check auto_end.
    if (!next) {
        if (config_.auto_end) {
            // Keep call active, but schedule a timer to leave.
            // 3 minutes (180s) is typical for auto_end.
            timer_.schedule(chatId, 180, [this, chatId]() {
                auto innerLk = lockFor(chatId);
                // Only stop if the queue is still empty (timer wasn't cancelled).
                if (!queue_.getCurrent(chatId)) {
                    stop(chatId);
                    if (cb_.onNotice)
                        cb_.onNotice(chatId, Notice::AutoLeft);
                }
            });
        } else {
            stop(chatId);
        }
        return;
    }

    if (cb_.onNotice)
        cb_.onNotice(chatId, Notice::PlayNext);

    // Download on demand; if it fails, skip this track and try the following.
    if (next->file_path.empty()) {
        const bool aborted = ensureFilePath(chatId, *next);
        if (next->file_path.empty()) {
            playNext(chatId);
            // A cancelled / refused fetch is NOT an error: the Telegram layer has
            // already told the user why, so stay quiet and just move on.
            if (!aborted && cb_.onNotice)
                cb_.onNotice(chatId, Notice::ErrorNoFile);
            return;
        }
        queue_.replaceCurrent(chatId, *next);
    }

    playMedia(chatId, *next);
}

bool CallManager::pause(std::int64_t chatId) {
    auto lk = lockFor(chatId);
    cache_.setPaused(chatId, true);
    const bool ok = transport_.pause(chatId);
    if (!ok) {
        stop(chatId);  // ConnectionNotFound / NotInCallError -> stop
    } else if (config_.auto_leave) {
        // Schedule auto-leave timer. 15 minutes (900s) is typical.
        timer_.schedule(chatId, 900, [this, chatId]() {
            auto innerLk = lockFor(chatId);
            // Only stop if still paused and active.
            if (cache_.isActiveCall(chatId) && !cache_.isPlaying(chatId)) {
                stop(chatId);
                if (cb_.onNotice)
                    cb_.onNotice(chatId, Notice::AutoLeft);
            }
        });
    }
    return ok;
}

bool CallManager::resume(std::int64_t chatId) {
    auto lk = lockFor(chatId);
    cache_.setPaused(chatId, false);
    timer_.cancel(chatId);
    const bool ok = transport_.resume(chatId);
    if (!ok)
        stop(chatId);
    return ok;
}

void CallManager::stop(std::int64_t chatId) {
    auto lk = lockFor(chatId);
    timer_.cancel(chatId);
    // Order mirrors TgCall.stop: clear queue, drop call + loop, then leave.
    queue_.clear(chatId);
    cache_.removeCall(chatId);
    cache_.setLoop(chatId, 0);
    transport_.stop(chatId);  // idempotent; swallows "not in call"
}

}  // namespace anonx
