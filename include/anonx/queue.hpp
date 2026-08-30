// AnonXMusic C++ port — Phase 5 (voice + queue)
// queue.hpp — per-chat playback queue.
//
// A faithful port of anony/helpers/_queue.py. Each chat has its own ordered
// list of media items; index 0 is the *currently playing* track and the rest
// are queued behind it. The Python version used `defaultdict(deque)`; here we
// use one std::deque per chat, created lazily on first write.
//
// The queue item type is `Track` (from youtube.hpp). In the Python bot the
// queue held a `Union[Media, Track]`; our `Track` struct is a superset of both
// dataclasses (it carries every Media field plus channel_name/thumbnail/…), so
// a single type covers both the yt-dlp results and replied-audio ("Media")
// items without loss.
//
// Thread-safety: the bot runs several assistant accounts on separate threads
// and stream-end callbacks fire from the voice layer's own thread, so a single
// mutex guards every access. All accessors return items *by value* (copies) so
// no reference into the deque can dangle once the lock is released.
//
// Header-only: depends only on youtube.hpp (for Track) and the STL.

#ifndef ANONX_QUEUE_HPP
#define ANONX_QUEUE_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "anonx/youtube.hpp"

namespace anonx {

// The queue element type. `Track` is a superset of the Python `Media`
// dataclass, so it represents both YouTube results and replied-audio items.
using MediaItem = Track;

class Queue {
public:
    Queue() = default;

    // Non-copyable / non-movable (owns a mutex).
    Queue(const Queue&)            = delete;
    Queue& operator=(const Queue&) = delete;
    Queue(Queue&&)                 = delete;
    Queue& operator=(Queue&&)      = delete;

    // Append an item to the end of the chat's queue and return its position,
    // i.e. the 0-based index at which it now sits (== number of items ahead of
    // it). A return of 0 means the queue was empty and this item is now the
    // "current" track. Mirrors _queue.py `add` (which returns len - 1).
    int add(std::int64_t chatId, const MediaItem& item) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& dq = queues_[chatId];
        dq.push_back(item);
        return static_cast<int>(dq.size()) - 1;
    }

    // Find the first item with the given id. Returns {position, item}; if no
    // item matches, returns {-1, std::nullopt}. Mirrors _queue.py `check_item`.
    std::pair<int, std::optional<MediaItem>>
    checkItem(std::int64_t chatId, const std::string& itemId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        if (it != queues_.end()) {
            const auto& dq = it->second;
            for (std::size_t i = 0; i < dq.size(); ++i) {
                if (dq[i].id == itemId)
                    return {static_cast<int>(i), dq[i]};
            }
        }
        return {-1, std::nullopt};
    }

    // Replace the currently playing item with a new one at the front. If
    // `removeAt > 0`, the item that ends up at that index is dropped — this
    // reproduces the Python `rotate(-remove); popleft(); rotate(remove)` idiom,
    // whose net effect is "erase the element at index `remove`".
    // Mirrors _queue.py `force_add`.
    void forceAdd(std::int64_t chatId, const MediaItem& item, int removeAt = 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& dq = queues_[chatId];
        if (!dq.empty())
            dq.pop_front();               // remove_current
        dq.push_front(item);              // appendleft
        if (removeAt > 0 &&
            static_cast<std::size_t>(removeAt) < dq.size()) {
            dq.erase(dq.begin() + removeAt);
        }
    }

    // The currently playing item (front of the queue), or nullopt if idle.
    // Mirrors _queue.py `get_current`.
    std::optional<MediaItem> getCurrent(std::int64_t chatId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        if (it == queues_.end() || it->second.empty())
            return std::nullopt;
        return it->second.front();
    }

    // Advance the queue and return the new current item.
    //   * empty queue           -> nullopt (nothing changes)
    //   * check == true (peek)  -> the *next* item (index 1) without mutating,
    //                              or nullopt if there is no next item
    //   * check == false        -> pop the current item, then return the new
    //                              front (or nullopt if the queue is now empty)
    // Mirrors _queue.py `get_next`.
    std::optional<MediaItem> getNext(std::int64_t chatId, bool check = false) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        if (it == queues_.end() || it->second.empty())
            return std::nullopt;
        auto& dq = it->second;
        if (check)
            return dq.size() > 1 ? std::optional<MediaItem>(dq[1]) : std::nullopt;
        dq.pop_front();
        if (dq.empty())
            return std::nullopt;
        return dq.front();
    }

    // The full queue for a chat, current item first. Mirrors `get_queue`.
    std::vector<MediaItem> getQueue(std::int64_t chatId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        if (it == queues_.end())
            return {};
        return std::vector<MediaItem>(it->second.begin(), it->second.end());
    }

    // Remove only the currently playing item (if any). Mirrors `remove_current`.
    void removeCurrent(std::int64_t chatId) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        if (it != queues_.end() && !it->second.empty())
            it->second.pop_front();
    }

    // Clear the entire queue for a chat. Mirrors `clear`.
    void clear(std::int64_t chatId) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        if (it != queues_.end())
            it->second.clear();
    }

    // ---------------------------------------------------------------
    // C++ additions (no Python equivalent)
    //
    // The Python bot mutates the *current* MediaItem object in place (setting
    // file_path / message_id / time as playback progresses). Because this queue
    // stores items by value, callers instead fetch the current item, mutate the
    // copy, and write it back with replaceCurrent().
    // ---------------------------------------------------------------

    // Overwrite the current (front) item. Returns false if the queue is empty.
    bool replaceCurrent(std::int64_t chatId, const MediaItem& item) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        if (it == queues_.end() || it->second.empty())
            return false;
        it->second.front() = item;
        return true;
    }

    // Number of items queued for a chat (including the current one).
    std::size_t size(std::int64_t chatId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        return it == queues_.end() ? 0 : it->second.size();
    }

    // Is the chat's queue empty?
    bool empty(std::int64_t chatId) const {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = queues_.find(chatId);
        return it == queues_.end() || it->second.empty();
    }

private:
    mutable std::mutex mtx_;
    std::unordered_map<std::int64_t, std::deque<MediaItem>> queues_;
};

}  // namespace anonx

#endif  // ANONX_QUEUE_HPP
