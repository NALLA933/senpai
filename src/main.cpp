// AnonXMusic C++ port — Integration phase
// main.cpp — real entrypoint. The C++ analogue of `python -m anony`
// (anony/__main__.py): load the config, boot the clients, register the plugins,
// idle until a signal arrives, shut down.
//
// Two builds, one file:
//   * with -DANONX_WITH_TDLIB=ON the full Runtime is compiled in and the bot
//     actually connects to Telegram;
//   * without it there is no Telegram layer to link, so the binary keeps the
//     Phase 2 behaviour — boot the data layer, report readiness, idle — which is
//     still useful for checking a .env, a database file and the locales.
// The default build therefore never needs TDLib installed.
//
// Usage:  anonx [path-to-.env]     (defaults to ".env" in the working directory)

#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include <sys/stat.h>   // mkdir

#include "anonx/app.hpp"

#if defined(ANONX_WITH_TDLIB)
#include <chrono>
#include <memory>
#include <thread>

#include "anonx/config.hpp"
#include "anonx/logger.hpp"
#include "anonx/runtime.hpp"
#include "anonx/userbot.hpp"

#if defined(ANONX_WITH_NTGCALLS)
#include "anonx/voice_signaling.hpp"
#else
#include "anonx/null_voice_transport.hpp"
#endif
#endif  // ANONX_WITH_TDLIB

namespace {

#if defined(ANONX_WITH_TDLIB)

// Set by the signal handler and polled by the idle loop. volatile
// sig_atomic_t is the only type it is safe to touch from a handler.
volatile std::sig_atomic_t g_stop = 0;

extern "C" void onSignal(int) { g_stop = 1; }

bool makeDir(const char* path) {
    return ::mkdir(path, 0755) == 0 || errno == EEXIST;
}

// The directories the bot writes into. App::boot() does this for the skeleton
// build; the integrated path needs the TDLib session tree as well.
bool ensureDirs(const anonx::Logger& log) {
    for (const char* dir : {"cache", "downloads", "tdlib"}) {
        if (!makeDir(dir)) {
            log.critical(std::string("cannot create directory '") + dir + "': " +
                         std::strerror(errno));
            return false;
        }
    }
    return true;
}

int runBot(const std::string& envFile) {
    anonx::LogSink::instance().init("log.txt");
    anonx::Logger log("anonx");
    log.info(std::string("AnonXMusic C++ ") + anonx::App::kVersion + " — initialising");

    anonx::Config config = anonx::Config::load(envFile);
    config.check();   // throws ConfigError, reported by main()

    if (!ensureDirs(log)) return 1;

    // Declared before the transport is used but after it is constructed: the
    // signaling provider below reads it lazily, on the first join. Declaring the
    // transport FIRST also gives the right teardown order — the Runtime borrows
    // it, so the Runtime must die first.
    std::unique_ptr<anonx::Runtime> runtime;

#if defined(ANONX_WITH_NTGCALLS)
    // The real engine. Voice chats are joined over an assistant's MTProto
    // connection, and that assistant is booted by the Runtime we have not built
    // yet — hence the deferred provider.
    anonx::NtgCallsTransport transport(anonx::makeDeferredAssistantSignaling(
        [&runtime]() -> anonx::TelegramClient* {
            if (!runtime) return nullptr;
            for (const std::unique_ptr<anonx::TelegramClient>& c :
                 runtime->userbot().clients()) {
                if (c && c->authorized()) return c.get();
            }
            return nullptr;
        }));
#else
    anonx::NullVoiceTransport transport;
    log.warning("built without NTgCalls (-DANONX_WITH_NTGCALLS=ON) — every "
                "command works, but streaming will report a server error");
#endif

    runtime = std::make_unique<anonx::Runtime>(config, transport);
    if (!runtime->start()) {
        log.critical("startup failed — see the messages above");
        return 1;
    }

    // Idle until SIGINT/SIGTERM. Updates arrive on TDLib's receive pump and are
    // handled on the Dispatcher's worker threads, so this thread has nothing to
    // do but wait.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    log.info("Press Ctrl+C to stop.");
    while (g_stop == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    runtime->stop();
    runtime.reset();
    anonx::LogSink::instance().close();
    return 0;
}

#else  // !ANONX_WITH_TDLIB

// No Telegram layer in this build: bring up the data layer and idle, exactly as
// Phase 2 did, after saying so once.
int runSkeleton(const std::string& envFile) {
    anonx::App app(envFile);
    app.log().warning("built without TDLib (-DANONX_WITH_TDLIB=ON) — running the "
                      "data-layer skeleton only; no Telegram connection");
    app.boot();
    app.run();   // blocks until SIGINT/SIGTERM, then shuts down
    return 0;
}

#endif  // ANONX_WITH_TDLIB

}  // namespace

int main(int argc, char** argv) {
    const std::string envFile = (argc > 1) ? argv[1] : ".env";
    try {
#if defined(ANONX_WITH_TDLIB)
        return runBot(envFile);
#else
        return runSkeleton(envFile);
#endif
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << "\n";
        return 1;
    }
}
