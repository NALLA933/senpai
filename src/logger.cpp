// AnonXMusic C++ port — Phase 2
// logger.cpp — implementation of LogSink (rotation + formatting + dual sink).

#include "anonx/logger.hpp"

#include <cstdio>
#include <ctime>
#include <string>

namespace anonx {
namespace {

const char* levelName(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warning:  return "WARNING";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
    }
    return "INFO";
}

// "%d-%b-%y %H:%M:%S" in local time, e.g. "26-Aug-26 11:33:00".
std::string timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%d-%b-%y %H:%M:%S", &tmv) == 0) {
        return "??";
    }
    return std::string(buf);
}

std::string backupName(const std::string& base, int i) {
    return base + "." + std::to_string(i);
}

}  // namespace

LogSink& LogSink::instance() {
    static LogSink sink;   // Meyers singleton: thread-safe init since C++11
    return sink;
}

LogSink::~LogSink() { close(); }

void LogSink::init(const std::string& filePath, std::size_t maxBytes,
                   int backupCount, LogLevel minLevel) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Re-configuring: drop any open handle so the new path takes effect.
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    filePath_ = filePath;
    maxBytes_ = maxBytes;
    backupCount_ = backupCount;
    minLevel_ = minLevel;
    curSize_ = 0;
}

void LogSink::setLevel(LogLevel lvl) {
    std::lock_guard<std::mutex> lk(mtx_);
    minLevel_ = lvl;
}

LogLevel LogSink::level() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return minLevel_;
}

void LogSink::ensureOpenLocked() {
    if (file_) return;
    file_ = std::fopen(filePath_.c_str(), "a");
    if (file_) {
        std::fseek(file_, 0, SEEK_END);
        long pos = std::ftell(file_);
        curSize_ = (pos > 0) ? static_cast<std::size_t>(pos) : 0;
    }
    // If the file can't be opened we simply skip file output; stderr still works.
}

// Mirrors RotatingFileHandler.doRollover: shift log.txt.(i) -> log.txt.(i+1),
// discard the oldest, then start a fresh log.txt.
void LogSink::rotateLocked() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (backupCount_ > 0) {
        for (int i = backupCount_ - 1; i >= 1; --i) {
            std::string sfn = backupName(filePath_, i);
            std::string dfn = backupName(filePath_, i + 1);
            std::remove(dfn.c_str());            // no-op if absent
            std::rename(sfn.c_str(), dfn.c_str());  // no-op if src absent
        }
        std::string first = backupName(filePath_, 1);
        std::remove(first.c_str());
        std::rename(filePath_.c_str(), first.c_str());
    }
    file_ = std::fopen(filePath_.c_str(), "w");  // fresh, truncated
    curSize_ = 0;
}

void LogSink::write(LogLevel lvl, const std::string& name, const std::string& message) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (static_cast<int>(lvl) < static_cast<int>(minLevel_)) return;

    std::string line = "[" + timestamp() + " - " + levelName(lvl) + "] - " +
                       name + ": " + message + "\n";

    // ---- console (stderr, like Python's default StreamHandler) ----
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);

    // ---- rotating file ----
    ensureOpenLocked();
    if (file_) {
        if (maxBytes_ > 0 && curSize_ > 0 && curSize_ + line.size() > maxBytes_) {
            rotateLocked();
        }
        if (file_) {
            std::fwrite(line.data(), 1, line.size(), file_);
            std::fflush(file_);
            curSize_ += line.size();
        }
    }
}

void LogSink::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (file_) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}

}  // namespace anonx
