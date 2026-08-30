// AnonXMusic C++ port — Phase 1 data layer
// cache_manager.hpp
//
// RAM-ONLY, ephemeral runtime state. Nothing in here is ever written to disk.
// These are high-frequency, short-lived values that are meaningless after a
// restart, so persisting them would only add I/O for no benefit:
//
//   * active_calls : is the bot currently in a voice chat for a given chat,
//                    and is that stream playing or paused.
//   * loop         : remaining loop count for the current track in a chat.
//
// Mirrors MongoDB.active_calls / MongoDB.loop from the original Python bot,
// which were plain in-memory dicts (never stored in Mongo).
//
// Thread-safety: a single mutex guards all maps. The bot runs multiple
// assistant accounts on separate threads, so every access is locked.
//
// Header-only: this class has no external dependencies.

#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace anonx {

class CacheManager {
public:
    CacheManager() = default;

    // Non-copyable / non-movable (holds a mutex).
    CacheManager(const CacheManager&)            = delete;
    CacheManager& operator=(const CacheManager&) = delete;
    CacheManager(CacheManager&&)                 = delete;
    CacheManager& operator=(CacheManager&&)      = delete;

    // ---------------------------------------------------------------
    // Active calls
    //   Presence of chat_id in the map  => bot is in a call there.
    //   Stored value: 1 = playing, 0 = paused.
    // ---------------------------------------------------------------

    // Is the bot currently joined to a voice chat for this chat?
    bool isActiveCall(std::int64_t chatId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return activeCalls_.find(chatId) != activeCalls_.end();
    }

    // Mark a call as started (playing).
    void addCall(std::int64_t chatId) {
        std::lock_guard<std::mutex> lk(mtx_);
        activeCalls_[chatId] = 1;
    }

    // Drop the call entirely (left / stopped).
    void removeCall(std::int64_t chatId) {
        std::lock_guard<std::mutex> lk(mtx_);
        activeCalls_.erase(chatId);
    }

    // Set paused/resumed state. Returns the new "is playing" state.
    // (Equivalent to MongoDB.playing(chat_id, paused=...).)
    bool setPaused(std::int64_t chatId, bool paused) {
        std::lock_guard<std::mutex> lk(mtx_);
        activeCalls_[chatId] = paused ? 0 : 1;
        return !paused;
    }

    // Is the stream currently playing (i.e. active and not paused)?
    bool isPlaying(std::int64_t chatId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = activeCalls_.find(chatId);
        return it != activeCalls_.end() && it->second != 0;
    }

    // ---------------------------------------------------------------
    // Loop counter (remaining repeats of the current track).
    // ---------------------------------------------------------------

    // Remaining loop count (0 when not looping / unset).
    int getLoop(std::int64_t chatId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = loop_.find(chatId);
        return it == loop_.end() ? 0 : it->second;
    }

    // Set the loop count. A value <= 0 clears the entry.
    void setLoop(std::int64_t chatId, int count) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (count <= 0)
            loop_.erase(chatId);
        else
            loop_[chatId] = count;
    }

    // ---------------------------------------------------------------
    // Housekeeping
    // ---------------------------------------------------------------

    // Drop all ephemeral state for a chat (call this on stop / leave).
    void clearChat(std::int64_t chatId) {
        std::lock_guard<std::mutex> lk(mtx_);
        activeCalls_.erase(chatId);
        loop_.erase(chatId);
    }

    // Number of chats the bot is currently streaming in (for stats/ping).
    std::size_t activeCallCount() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return activeCalls_.size();
    }

    // Every chat with an active call, ascending by id (deterministic order, so
    // the /activevc listing is stable). Ports MongoDB.get_active_chats().
    std::vector<std::int64_t> activeChats() const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<std::int64_t> out;
        out.reserve(activeCalls_.size());
        for (const auto& kv : activeCalls_)
            out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::int64_t, int> activeCalls_;  // chat_id -> 1 playing / 0 paused
    std::unordered_map<std::int64_t, int> loop_;         // chat_id -> remaining loops
};

}  // namespace anonx
