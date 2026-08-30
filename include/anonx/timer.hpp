// AnonXMusic C++ port — Phase 7.5 (auto_leave / auto_end)
// timer.hpp — seam for scheduling delayed actions.

#ifndef ANONX_TIMER_HPP
#define ANONX_TIMER_HPP

#include <cstdint>
#include <functional>

namespace anonx {

class Timer {
public:
    virtual ~Timer() = default;

    // Schedule `callback` to be invoked after `seconds`. If a timer for `chatId`
    // is already running, it is replaced. The callback executes on the timer's
    // background thread; CallManager must lock the chat internally.
    virtual void schedule(std::int64_t chatId, int seconds, std::function<void()> callback) = 0;

    // Cancel any pending timer for `chatId`.
    virtual void cancel(std::int64_t chatId) = 0;
};

// Real implementation that runs a background thread.
class BackgroundTimer : public Timer {
public:
    BackgroundTimer();
    ~BackgroundTimer() override;

    // Non-copyable/non-movable
    BackgroundTimer(const BackgroundTimer&) = delete;
    BackgroundTimer& operator=(const BackgroundTimer&) = delete;

    void schedule(std::int64_t chatId, int seconds, std::function<void()> callback) override;
    void cancel(std::int64_t chatId) override;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace anonx

#endif  // ANONX_TIMER_HPP
