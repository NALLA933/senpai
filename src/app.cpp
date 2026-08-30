// AnonXMusic C++ port — Phase 2
// app.cpp — implementation of the application skeleton.

#include "anonx/app.hpp"

#include "anonx/cache_manager.hpp"
#include "anonx/database.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>   // mkdir
#include <unistd.h>     // access

namespace anonx {
namespace {

// Set by the signal handler; polled by run(). volatile sig_atomic_t is the only
// type safe to touch from an async signal handler.
volatile std::sig_atomic_t g_stopFlag = 0;

extern "C" void onSignal(int) { g_stopFlag = 1; }

// Is `tool` an executable found on any PATH directory?
bool onPath(const std::string& tool) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return false;
    std::istringstream iss(pathEnv);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
        if (dir.empty()) continue;
        std::string full = dir + "/" + tool;
        if (::access(full.c_str(), X_OK) == 0) return true;
    }
    return false;
}

}  // namespace

App::App(const std::string& envFile)
    : config_(Config::load(envFile)), logger_("anonx") {
    // Match the Python root logger: log.txt (rotating) + stderr, INFO level.
    LogSink::instance().init("log.txt");
    logger_.info(std::string("AnonXMusic C++ ") + kVersion + " — initialising");

    try {
        config_.check();
    } catch (const ConfigError& e) {
        logger_.critical(e.what());
        throw;  // let main() report and exit non-zero
    }
}

App::~App() {
    stop();
    LogSink::instance().close();
}

void App::ensureDirs() {
    auto make = [&](const char* dir) {
        if (::mkdir(dir, 0755) != 0 && errno != EEXIST) {
            throw std::runtime_error(std::string("cannot create directory '") +
                                     dir + "': " + std::strerror(errno));
        }
    };
    make("cache");
    make("downloads");
    logger_.info("Cache directories ready.");
}

void App::checkMediaTools() {
    // The Python original hard-requires deno + ffmpeg. Voice streaming lands in
    // Phase 5, so here we only warn — the skeleton still boots on a dev box.
    std::vector<std::string> missing;
    if (!onPath("ffmpeg")) missing.push_back("ffmpeg");
    if (!onPath("deno"))   missing.push_back("deno");

    if (!missing.empty()) {
        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i) list += ", ";
            list += missing[i];
        }
        logger_.warning("Missing media tools on PATH: " + list +
                        " — required for voice streaming (Phase 5).");
    }
}

void App::boot() {
    if (booted_) return;
    logger_.info("Booting...");

    ensureDirs();
    checkMediaTools();

    // ---- data layer ----
    db_ = std::make_unique<Database>(config_.db_path);
    db_->setDefaultLang(config_.lang_code);
    db_->setAssistantCount(config_.assistantCount());
    cache_ = std::make_unique<CacheManager>();

    // Report loaded persistent state, echoing __main__.py's boot logging.
    const std::size_t sudo = db_->getSudoers().size();
    const std::size_t blU  = db_->getBlacklistedUsers().size();
    const std::size_t blC  = db_->getBlacklistedChats().size();

    logger_.info(config_.redactedSummary());
    logger_.info("Loaded " + std::to_string(sudo) + " sudo user(s), " +
                 std::to_string(blU) + " blacklisted user(s), " +
                 std::to_string(blC) + " blacklisted chat(s).");

    bootTime_ = std::chrono::steady_clock::now();
    booted_ = true;

    logger_.info("Ready. " + std::to_string(config_.assistantCount()) +
                 " assistant(s) configured.");
}

void App::run() {
    // Install SIGINT/SIGTERM handlers (POSIX sigaction), then idle until asked
    // to stop — the C++ analogue of __main__.py's idle()/stop_event.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    logger_.info("Running. Press Ctrl+C to stop.");
    while (g_stopFlag == 0 && !stopRequested_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    stop();
}

void App::stop() {
    if (stopped_) return;
    stopped_ = true;
    logger_.info("Stopping...");

    // Later phases stop Telegram/voice clients here. For now, release the data
    // layer (Database's destructor closes the SQLite handle cleanly).
    cache_.reset();
    db_.reset();

    logger_.info("Stopped.");
}

void App::requestStop() { stopRequested_.store(true, std::memory_order_relaxed); }

double App::uptimeSeconds() const {
    if (!booted_) return 0.0;
    std::chrono::duration<double> d = std::chrono::steady_clock::now() - bootTime_;
    return d.count();
}

Database& App::db() {
    if (!db_) throw std::runtime_error("App::db() called before boot()");
    return *db_;
}

CacheManager& App::cache() {
    if (!cache_) throw std::runtime_error("App::cache() called before boot()");
    return *cache_;
}

}  // namespace anonx
