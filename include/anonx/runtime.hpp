// AnonXMusic C++ port — Integration phase
// runtime.hpp — the object graph that makes the bot actually run.
//
// Phases 1–6b each produced a layer that was verified in isolation: the data
// layer, the config/app skeleton, the yt-dlp service, the Telegram client and
// dispatcher, the voice orchestration, and the command plugins. Nothing wired
// them together — src/main.cpp booted the skeleton and idled. This class is that
// wiring, and it is the C++ analogue of the module-level singletons in
// anony/__init__.py plus the startup sequence in anony/__main__.py.
//
// WHAT IT OWNS, AND IN WHICH ORDER
// Construction order is the dependency order: database and caches, then the
// language tables, the queue, the YouTube service and system info, then the bot
// client, then the BotApi over it, the CallManager over the injected voice
// transport, the command plugins, and finally the Dispatcher.
// Destruction runs in reverse, which matters: the Dispatcher holds lambdas that
// capture the plugins, and CallManager holds callbacks that capture Plugins, so
// the Dispatcher must die first and the plugins before the objects they borrow.
// Declaring the members in dependency order (see below) is what guarantees that
// — no manual teardown sequence to get wrong.
//
// WHY THE VOICE TRANSPORT IS INJECTED
// NTgCalls is an opt-in dependency (-DANONX_WITH_NTGCALLS=ON). Runtime therefore
// takes a VoiceTransport& from the caller: production passes an
// NtgCallsTransport built with makeAssistantSignaling(assistant) (see
// voice_signaling.hpp), and the test passes a FakeVoiceTransport. The same trick
// Phase 5 used, applied one level up.
//
// THREADING
// Runtime adds no threads of its own, but it does not run handlers on TDLib's
// receive pump either: the pump only enqueues, and the Dispatcher's own worker
// pool (Phase 4) runs the handlers, because a handler that blocks on invoke()
// while sitting on the pump would deadlock the very thread its answer must
// arrive on. Every collaborator a handler touches was already made thread-safe
// in its own phase (Database's mutex, CallManager's per-chat recursive mutex,
// Queue's mutex).

#ifndef ANONX_RUNTIME_HPP
#define ANONX_RUNTIME_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "anonx/admin_plugins.hpp"
#include "anonx/cache_manager.hpp"
#include "anonx/config.hpp"
#include "anonx/database.hpp"
#include "anonx/dispatcher.hpp"
#include "anonx/ffmpeg_thumbnail_renderer.hpp"
#include "anonx/lang.hpp"
#include "anonx/plugins.hpp"
#include "anonx/queue.hpp"
#include "anonx/sysinfo.hpp"
#include "anonx/telegram_bot_api.hpp"
#include "anonx/telegram_client.hpp"
#include "anonx/timer.hpp"
#include "anonx/cookie_source.hpp"
#include "anonx/userbot.hpp"
#include "anonx/voice_transport.hpp"
#include "anonx/youtube.hpp"

namespace anonx {

// Runtime's knobs. Declared at namespace scope rather than nested inside Runtime
// because a nested class's default member initializers may not be used from a
// default argument of the enclosing class (the enclosing class is still
// incomplete there), which is what `Options opts = {}` would need. Runtime
// re-exports it as Runtime::Options, so callers spell it either way.
struct RuntimeOptions {
    // Directory holding the locale JSON files (13 languages).
    std::string localesDir = "locales";
    // Where TDLib keeps the bot account's session.
    std::string botSessionDir = "tdlib/bot";
    // Prompt on stdin when an assistant needs its login code / 2FA password.
    // Off for tests and for headless deployments whose sessions already exist.
    bool interactiveAssistantLogin = true;
    // Boot the assistant accounts. Off in tests, and useful for a first run
    // that only needs to verify the bot half.
    bool bootAssistants = true;
};

class Runtime {
public:
    using Options = RuntimeOptions;

    // `config` must be validated (Config::check) and both references must
    // outlive the Runtime.
    Runtime(const Config& config, VoiceTransport& transport, Options opts = {});
    ~Runtime();

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Open the database, load the locales, boot the bot (and the assistants
    // unless disabled), register every command, and start routing updates.
    // Returns false if any step fails; the reason is logged.
    bool start();

    // Announce the shutdown, close the clients and stop TDLib's receive pump.
    // Idempotent.
    void stop();

    // Number of assistant accounts that authorized successfully.
    std::size_t assistantsUp() const;

    // The graph, for the entrypoint and for tests that drive it by hand.
    Database&       db()         { return db_; }
    CacheManager&   cache()      { return cache_; }
    Language&       lang()       { return lang_; }
    Queue&          queue()      { return queue_; }
    CallManager&    calls()      { return calls_; }
    Plugins&        plugins()    { return plugins_; }
    AdminPlugins&   admin()      { return admin_; }
    Dispatcher&     dispatcher() { return *dispatcher_; }
    TelegramClient& bot()        { return bot_; }
    Userbot&        userbot()    { return userbot_; }
    BotApi&         api()        { return api_; }

private:
    void announceStartup();

    const Config& config_;
    Options       opts_;

    // ---- dependency order: EARLIER members are destroyed LAST ----
    Database     db_;
    CacheManager cache_;
    Language     lang_;
    Queue        queue_;
    YouTube      yt_;
    SystemInfo   sys_;
    FfmpegThumbnailRenderer thumb_;

    TelegramClient bot_;
    Userbot        userbot_;
    TelegramBotApi api_;

    BackgroundTimer              timer_;
    std::unique_ptr<CookieSource> cookieSrc_;
    CallManager                  calls_;
    Plugins                      plugins_;
    AdminPlugins                 admin_;

    // Held by pointer so it is destroyed first and explicitly: its handlers
    // capture plugins_ and admin_.
    std::unique_ptr<Dispatcher> dispatcher_;

    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
};

}  // namespace anonx

#endif  // ANONX_RUNTIME_HPP
