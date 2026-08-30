// AnonXMusic C++ port — Phase 2
// logger.hpp — thread-safe logging, ported from anony/__init__.py.
//
// Reproduces the original logging configuration:
//   format  : "[%(asctime)s - %(levelname)s] - %(name)s: %(message)s"
//   datefmt : "%d-%b-%y %H:%M:%S"          e.g. "[26-Aug-26 11:33:00 - INFO] - anonx: ..."
//   handlers: RotatingFileHandler("log.txt", maxBytes=10MB, backupCount=5)
//             + a stream handler (stderr)
//   level   : INFO
//
// A single process-wide LogSink owns the file + rotation + mutex; `Logger`
// objects are cheap named front-ends, like Python's logging.getLogger(name).

#ifndef ANONX_LOGGER_HPP
#define ANONX_LOGGER_HPP

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace anonx {

// Numeric values mirror Python's logging module so comparisons read the same.
enum class LogLevel {
    Debug = 10,
    Info = 20,
    Warning = 30,
    Error = 40,
    Critical = 50,
};

// Process-wide log sink. Thread-safe. Configure once at startup via init();
// if never configured, it lazily uses the defaults below on first write.
class LogSink {
public:
    static LogSink& instance();

    void init(const std::string& filePath = "log.txt",
              std::size_t maxBytes = 10u * 1024u * 1024u,
              int backupCount = 5,
              LogLevel minLevel = LogLevel::Info);

    void setLevel(LogLevel lvl);
    LogLevel level() const;

    // Format and emit one record from logger `name` at `lvl`. Records below the
    // configured minimum level are dropped.
    void write(LogLevel lvl, const std::string& name, const std::string& message);

    // Flush + close the file (used at shutdown). Safe to call more than once.
    void close();

private:
    LogSink() = default;
    ~LogSink();
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

    void ensureOpenLocked();
    void rotateLocked();

    mutable std::mutex mtx_;
    std::string filePath_ = "log.txt";
    std::size_t maxBytes_ = 10u * 1024u * 1024u;
    int backupCount_ = 5;
    LogLevel minLevel_ = LogLevel::Info;
    std::FILE* file_ = nullptr;   // opened lazily on first write
    std::size_t curSize_ = 0;
};

// A named logger. Copyable and cheap; forwards to the shared LogSink.
class Logger {
public:
    explicit Logger(std::string name) : name_(std::move(name)) {}

    void debug(const std::string& msg) const    { LogSink::instance().write(LogLevel::Debug, name_, msg); }
    void info(const std::string& msg) const      { LogSink::instance().write(LogLevel::Info, name_, msg); }
    void warning(const std::string& msg) const   { LogSink::instance().write(LogLevel::Warning, name_, msg); }
    void error(const std::string& msg) const      { LogSink::instance().write(LogLevel::Error, name_, msg); }
    void critical(const std::string& msg) const   { LogSink::instance().write(LogLevel::Critical, name_, msg); }

    const std::string& name() const { return name_; }

private:
    std::string name_;
};

}  // namespace anonx

#endif  // ANONX_LOGGER_HPP
