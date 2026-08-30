// AnonXMusic C++ port — Phase 7.5 (auto_leave / auto_end)
// timer.cpp — BackgroundTimer implementation.

#include "anonx/timer.hpp"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace anonx {

struct BackgroundTimer::Impl {
    struct Job {
        std::chrono::steady_clock::time_point deadline;
        std::function<void()> callback;
    };

    std::mutex mtx;
    std::condition_variable cv;
    std::map<std::int64_t, Job> jobs;
    bool stop = false;
    std::thread worker;

    Impl() {
        worker = std::thread([this]() {
            while (true) {
                std::function<void()> readyCallback;
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    if (stop) break;

                    if (jobs.empty()) {
                        cv.wait(lk);
                    } else {
                        // Find the earliest deadline
                        auto earliest = jobs.end();
                        auto now = std::chrono::steady_clock::now();
                        for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                            if (earliest == jobs.end() || it->second.deadline < earliest->second.deadline) {
                                earliest = it;
                            }
                        }

                        if (now >= earliest->second.deadline) {
                            readyCallback = std::move(earliest->second.callback);
                            jobs.erase(earliest);
                        } else {
                            cv.wait_until(lk, earliest->second.deadline);
                        }
                    }
                }

                if (readyCallback) {
                    // Execute outside the lock
                    readyCallback();
                }
            }
        });
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }
};

BackgroundTimer::BackgroundTimer() : impl_(new Impl()) {}

BackgroundTimer::~BackgroundTimer() {
    delete impl_;
}

void BackgroundTimer::schedule(std::int64_t chatId, int seconds, std::function<void()> callback) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->jobs[chatId] = {
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds),
        std::move(callback)
    };
    impl_->cv.notify_one();
}

void BackgroundTimer::cancel(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->jobs.erase(chatId);
    impl_->cv.notify_one();
}

}  // namespace anonx
