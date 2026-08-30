// AnonXMusic C++ port — Phase 2
// app.hpp — the application skeleton.
//
// Mirrors the wiring in anony/__init__.py (global singletons) and the lifecycle
// in anony/__main__.py (connect db -> boot clients -> load -> idle -> stop).
// Telegram/voice clients arrive in later phases; boot() currently brings up the
// data layer (Database + CacheManager) and reports readiness.

#ifndef ANONX_APP_HPP
#define ANONX_APP_HPP

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "anonx/config.hpp"
#include "anonx/logger.hpp"

namespace anonx {

class Database;       // forward-declared: pulled in only by app.cpp
class CacheManager;

class App {
public:
    static constexpr const char* kVersion = "3.0.3-cpp";

    // Load config from `envFile` (default ".env"), initialise logging, and
    // validate the config. Throws ConfigError if required values are missing.
    explicit App(const std::string& envFile = ".env");
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Prepare directories, open the database, and apply config-derived defaults.
    // Throws on fatal errors (config invalid, database cannot be opened).
    void boot();

    // Block until SIGINT/SIGTERM is received, then stop() gracefully.
    // Call after boot(). Returns when shutdown is complete.
    void run();

    // Graceful shutdown. Idempotent — safe to call more than once.
    void stop();

    // Seconds elapsed since boot() completed (0 before boot).
    double uptimeSeconds() const;

    // Ask the run() loop to exit. Async-safe: only sets an atomic flag.
    void requestStop();

    // ---- accessors ----
    Config&        config()       { return config_; }
    const Config&  config() const { return config_; }
    const Logger&  log() const    { return logger_; }
    Database&      db();          // throws if called before boot()
    CacheManager&  cache();       // throws if called before boot()

private:
    void ensureDirs();       // create cache/ and downloads/
    void checkMediaTools();  // warn (not fatal in Phase 2) if ffmpeg/deno missing

    Config config_;
    Logger logger_;
    std::unique_ptr<Database>     db_;
    std::unique_ptr<CacheManager> cache_;

    std::chrono::steady_clock::time_point bootTime_{};
    std::atomic<bool> stopRequested_{false};
    bool booted_ = false;
    bool stopped_ = false;
};

}  // namespace anonx

#endif  // ANONX_APP_HPP
