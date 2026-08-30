// AnonXMusic C++ port — Phase 4
// dispatcher.hpp — message/callback routing and composable filters.
//
// This is the C++ analogue of Pyrogram's handler decorators, e.g.
//     @app.on_message(filters.command(["play"]) & filters.group & ~app.bl_users)
// Here that becomes:
//     disp.onMessage(filters::command({"play"}) && filters::groupChat()
//                        && !filters::userWhere(isBlacklisted),
//                    [](MessageContext& m){ ... });
//
// A Dispatcher attaches to a TelegramClient as its update observer, parses
// updateNewMessage / updateNewCallbackQuery, builds a small typed context, and
// runs the first handler whose filter matches. All JSON parsing is in the .cpp.
//
// WHY HANDLERS RUN ON WORKER THREADS
// TDLib requires that td_receive() be called from a single thread, so there is
// exactly one receive pump (see td_client.cpp). Running handlers directly on it
// would deadlock every handler that waits for a reply: a blocking request cannot
// complete while the only thread able to deliver its response is busy inside the
// handler. Pyrogram has the same constraint and solves it the same way — its
// Dispatcher hands each update to one of `workers` tasks (4 by default) — so this
// class keeps a small pool of worker threads, and the pump only enqueues.

#ifndef ANONX_DISPATCHER_HPP
#define ANONX_DISPATCHER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anonx/telegram_client.hpp"

namespace anonx {

// Simplified chat classification (from the TDLib chat-id sign convention):
// positive ids are private chats, negative ids are groups/supergroups.
enum class ChatType { Private, Group };

struct MediaDescriptor {
    std::string kind;     // e.g., "audio", "video", "document", "" if none
    std::string fileId;   // TDLib file ID or remote ID
    int duration = 0;     // seconds
    std::string title;    // file name or title
    std::int64_t size = 0;
};

struct TextEntity {
    std::string type;     // e.g., "textEntityTypeUrl", "textEntityTypeTextUrl"
    int offset = 0;
    int length = 0;
    std::string url;      // Only for textEntityTypeTextUrl
};

struct MessageContext {
    std::int64_t chatId = 0;
    ChatType chatType = ChatType::Group;
    std::int64_t messageId = 0;
    std::int64_t fromUserId = 0;    // 0 when sent anonymously / by a chat
    std::string text;               // plain text of a messageText (no entities)

    // Id of the message this one replies to in the SAME chat, else 0. Several
    // admin commands are addressed by reply ("/auth" on a member's message),
    // which is why the raw id is carried here rather than the whole message.
    std::int64_t replyToMessageId = 0;

    // If the text began with a command (prefix + name), this holds
    // [name, arg1, arg2, …] — name is without the prefix and without @bot.
    std::vector<std::string> command;

    // Entities in the message (parsed from text/caption)
    std::vector<TextEntity> entities;

    // Media properties of the message this one replies to (if any)
    MediaDescriptor replyMedia;

    TelegramClient* client = nullptr;

    bool isCommand() const { return !command.empty(); }

    // Reply in the same chat (HTML). Returns the new message id (0 on failure).
    std::int64_t reply(const std::string& html) const;
};

struct CallbackContext {
    std::int64_t chatId = 0;
    std::int64_t messageId = 0;
    std::int64_t fromUserId = 0;
    std::string data;               // decoded button payload
    std::int64_t queryId = 0;
    TelegramClient* client = nullptr;

    // Acknowledge the query; optionally show text (toast or alert popup).
    void answer(const std::string& text = "", bool alert = false) const;
};

// A composable predicate over a context type. A default-constructed filter
// matches everything. Combine with &&, ||, and ! just like Pyrogram filters.
template <typename Ctx>
class BasicFilter {
public:
    using Predicate = std::function<bool(const Ctx&)>;

    BasicFilter() = default;
    explicit BasicFilter(Predicate p) : pred_(std::move(p)) {}

    bool operator()(const Ctx& c) const { return pred_ ? pred_(c) : true; }

private:
    Predicate pred_;
};

template <typename Ctx>
BasicFilter<Ctx> operator&&(BasicFilter<Ctx> a, BasicFilter<Ctx> b) {
    return BasicFilter<Ctx>([a, b](const Ctx& c) { return a(c) && b(c); });
}

template <typename Ctx>
BasicFilter<Ctx> operator||(BasicFilter<Ctx> a, BasicFilter<Ctx> b) {
    return BasicFilter<Ctx>([a, b](const Ctx& c) { return a(c) || b(c); });
}

template <typename Ctx>
BasicFilter<Ctx> operator!(BasicFilter<Ctx> a) {
    return BasicFilter<Ctx>([a](const Ctx& c) { return !a(c); });
}

using Filter = BasicFilter<MessageContext>;
using CallbackFilter = BasicFilter<CallbackContext>;

namespace filters {

// Matches a leading command whose name is in `names` (case-insensitive).
Filter command(std::vector<std::string> names);

Filter privateChat();
Filter groupChat();

// Matches when the sender's id is in the given set.
Filter user(std::vector<std::int64_t> ids);

// Matches when the sender satisfies a runtime predicate — use for dynamic
// sets such as blacklisted users or sudoers that change while running.
Filter userWhere(std::function<bool(std::int64_t)> pred);

// Matches any message that carries non-empty text.
Filter textMessage();

// --- callback-query filters ---
CallbackFilter callbackData(std::string exact);
CallbackFilter callbackDataPrefix(std::string prefix);

}  // namespace filters

class Dispatcher {
public:
    using MessageHandler = std::function<void(MessageContext&)>;
    using CallbackHandler = std::function<void(CallbackContext&)>;

    Dispatcher() = default;
    ~Dispatcher();

    Dispatcher(const Dispatcher&)            = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Command prefixes to recognise (default: '/').
    void setPrefixes(std::vector<char> prefixes);
    // Bot username (without '@'); lets "/cmd@thisbot" match and rejects
    // "/cmd@otherbot". Optional but recommended in groups.
    void setBotUsername(std::string username);

    // How many threads run handlers (default 4, matching Pyrogram's default).
    // Must be called before attach(); 0 means "handle updates inline on the
    // calling thread", which only makes sense for tests that never block.
    void setWorkers(std::size_t n);

    // Route this client's non-auth updates through the dispatcher and use it
    // for replies. Typically called once after the client is booted. Starts the
    // worker pool.
    void attach(TelegramClient& client);

    // Stop accepting updates and join the worker threads, waiting for the
    // handler currently running (if any) to return. Idempotent, and called by
    // the destructor — but call it explicitly before tearing down anything the
    // handlers captured.
    void stopWorkers();

    void onMessage(Filter filter, MessageHandler handler);
    void onCallback(CallbackFilter filter, CallbackHandler handler);

    // Register a watcher that sees EVERY message (commands included) and never
    // stops propagation — the analogue of a second Pyrogram handler group, which
    // is how the Python bot's chat watcher runs alongside the command handlers.
    // Watchers run before routing, in registration order.
    void onEveryMessage(MessageHandler handler);

    // Entry point used as the client's update observer. Hands the update to a
    // worker (or routes it inline when the pool is not running, which is what
    // tests calling this directly rely on).
    void onUpdate(const std::string& updateJson);

    // True when no update is queued and no handler is running — the hook a test
    // needs to know that everything an injected update triggered has happened.
    bool idle() const;

    // Dispatch a pre-built context; returns true if a handler ran. Exposed so
    // tests can drive routing without a live TDLib connection.
    bool dispatchMessage(MessageContext& ctx);
    bool dispatchCallback(CallbackContext& ctx);

    // Parse a command out of `text` using the configured prefixes/username.
    // Returns [name, args…] or empty if `text` is not a command for us.
    std::vector<std::string> parseCommand(const std::string& text) const;

private:
    // The routing itself: parse the update, build the context, run the watchers
    // and then the first matching handler. Runs on a worker thread.
    void handleUpdate(const std::string& updateJson);
    void startWorkers();
    void workerLoop();

    std::mutex mtx_;

    struct MEntry { Filter filter; MessageHandler handler; };
    struct CEntry { CallbackFilter filter; CallbackHandler handler; };
    std::vector<MEntry> messageHandlers_;
    std::vector<CEntry> callbackHandlers_;
    std::vector<MessageHandler> watchers_;

    std::vector<char> prefixes_{'/'};
    std::string botUsername_;   // stored lowercased
    TelegramClient* client_ = nullptr;

    // ---- the worker pool ----
    mutable std::mutex       qMutex_;
    std::condition_variable  qCv_;
    std::deque<std::string>  queue_;
    std::vector<std::thread> workers_;
    std::size_t              workerCount_ = 4;
    std::size_t              busy_ = 0;          // handlers currently running
    std::atomic<bool>        running_{false};
};

}  // namespace anonx

#endif  // ANONX_DISPATCHER_HPP
