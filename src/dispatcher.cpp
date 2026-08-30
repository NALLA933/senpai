// AnonXMusic C++ port — Phase 4
// dispatcher.cpp — context building, filters, and routing.

#include "anonx/dispatcher.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace anonx {
namespace {

using nlohmann::json;

std::string strField(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return std::string();
}

// Read a 64-bit integer field. With real nlohmann this preserves the full
// 64-bit range (ids/query-ids can exceed 2^53); numbers are stored as int64,
// not double.
std::int64_t intField(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) {
        return j[key].get<std::int64_t>();
    }
    return 0;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && isSpace(s[i])) ++i;
        if (i >= n) break;
        std::size_t start = i;
        while (i < n && !isSpace(s[i])) ++i;
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

// Decode standard base64 (TDLib delivers callback payloads base64-encoded).
std::string base64Decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || isSpace(c)) continue;
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

// --- context helpers ---

std::int64_t MessageContext::reply(const std::string& html) const {
    return client ? client->sendMessage(chatId, html) : 0;
}

void CallbackContext::answer(const std::string& text, bool alert) const {
    if (!client) return;
    json req;
    req["@type"] = "answerCallbackQuery";
    req["callback_query_id"] = queryId;
    if (!text.empty()) req["text"] = text;
    req["show_alert"] = alert;
    client->raw().send(req.dump());
}

// --- filters ---

namespace filters {

Filter command(std::vector<std::string> names) {
    for (auto& n : names) n = lower(n);
    return Filter([names](const MessageContext& c) {
        if (c.command.empty()) return false;
        const std::string name = lower(c.command[0]);
        for (const auto& n : names) {
            if (n == name) return true;
        }
        return false;
    });
}

Filter privateChat() {
    return Filter([](const MessageContext& c) { return c.chatType == ChatType::Private; });
}

Filter groupChat() {
    return Filter([](const MessageContext& c) { return c.chatType == ChatType::Group; });
}

Filter user(std::vector<std::int64_t> ids) {
    return Filter([ids](const MessageContext& c) {
        for (std::int64_t id : ids) {
            if (id == c.fromUserId) return true;
        }
        return false;
    });
}

Filter userWhere(std::function<bool(std::int64_t)> pred) {
    return Filter([pred](const MessageContext& c) {
        return c.fromUserId != 0 && pred && pred(c.fromUserId);
    });
}

Filter textMessage() {
    return Filter([](const MessageContext& c) { return !c.text.empty(); });
}

CallbackFilter callbackData(std::string exact) {
    return CallbackFilter([exact](const CallbackContext& c) { return c.data == exact; });
}

CallbackFilter callbackDataPrefix(std::string prefix) {
    return CallbackFilter(
        [prefix](const CallbackContext& c) { return c.data.rfind(prefix, 0) == 0; });
}

}  // namespace filters

// --- Dispatcher ---

void Dispatcher::setPrefixes(std::vector<char> prefixes) {
    std::lock_guard<std::mutex> lk(mtx_);
    prefixes_ = std::move(prefixes);
}

void Dispatcher::setBotUsername(std::string username) {
    std::lock_guard<std::mutex> lk(mtx_);
    botUsername_ = lower(std::move(username));
}

void Dispatcher::attach(TelegramClient& client) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        client_ = &client;
        if (botUsername_.empty() && !client.me().username.empty()) {
            botUsername_ = lower(client.me().username);
        }
    }
    startWorkers();
    client.setUpdateObserver([this](const std::string& u) { onUpdate(u); });
}

// --- the worker pool --------------------------------------------------------
//
// The pump thread only enqueues; these threads run the handlers, so a handler
// may block on a request without stopping TDLib from delivering its response.

Dispatcher::~Dispatcher() {
    stopWorkers();
}

void Dispatcher::setWorkers(std::size_t n) {
    std::lock_guard<std::mutex> lk(qMutex_);
    workerCount_ = n;
}

void Dispatcher::startWorkers() {
    std::lock_guard<std::mutex> lk(qMutex_);
    if (running_.load() || workerCount_ == 0) return;
    running_.store(true);
    workers_.reserve(workerCount_);
    for (std::size_t i = 0; i < workerCount_; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

void Dispatcher::stopWorkers() {
    if (!running_.exchange(false)) {
        // Never started (or already stopped): still drop anything queued.
        std::lock_guard<std::mutex> lk(qMutex_);
        queue_.clear();
        return;
    }
    qCv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
    std::lock_guard<std::mutex> lk(qMutex_);
    workers_.clear();
    queue_.clear();
}

void Dispatcher::workerLoop() {
    for (;;) {
        std::string update;
        {
            std::unique_lock<std::mutex> lk(qMutex_);
            qCv_.wait(lk, [this] { return !queue_.empty() || !running_.load(); });
            if (queue_.empty()) {
                if (!running_.load()) return;
                continue;
            }
            update = std::move(queue_.front());
            queue_.pop_front();
            ++busy_;
        }

        // A throwing handler must not take the pool (or the process) down.
        try {
            handleUpdate(update);
        } catch (...) {
        }

        {
            std::lock_guard<std::mutex> lk(qMutex_);
            --busy_;
        }
        qCv_.notify_all();   // wakes anyone waiting in idle()
    }
}

bool Dispatcher::idle() const {
    std::lock_guard<std::mutex> lk(qMutex_);
    return queue_.empty() && busy_ == 0;
}

void Dispatcher::onUpdate(const std::string& updateJson) {
    if (running_.load()) {
        {
            std::lock_guard<std::mutex> lk(qMutex_);
            queue_.push_back(updateJson);
        }
        qCv_.notify_one();
        return;
    }
    // No pool (tests, or after stopWorkers()): route inline.
    handleUpdate(updateJson);
}

void Dispatcher::onMessage(Filter filter, MessageHandler handler) {
    std::lock_guard<std::mutex> lk(mtx_);
    messageHandlers_.push_back(MEntry{std::move(filter), std::move(handler)});
}

void Dispatcher::onCallback(CallbackFilter filter, CallbackHandler handler) {
    std::lock_guard<std::mutex> lk(mtx_);
    callbackHandlers_.push_back(CEntry{std::move(filter), std::move(handler)});
}

void Dispatcher::onEveryMessage(MessageHandler handler) {
    std::lock_guard<std::mutex> lk(mtx_);
    watchers_.push_back(std::move(handler));
}

std::vector<std::string> Dispatcher::parseCommand(const std::string& text) const {
    std::vector<std::string> out;
    if (text.empty()) return out;

    bool okPrefix = false;
    for (char p : prefixes_) {
        if (p == text[0]) { okPrefix = true; break; }
    }
    if (!okPrefix) return out;

    std::vector<std::string> tokens = tokenize(text);
    if (tokens.empty()) return out;

    std::string head = tokens[0].substr(1);   // drop the prefix character
    const auto at = head.find('@');
    if (at != std::string::npos) {
        const std::string uname = head.substr(at + 1);
        head = head.substr(0, at);
        // "/cmd@otherbot" is meant for a different bot — ignore it.
        if (!uname.empty() && !botUsername_.empty() && lower(uname) != botUsername_) {
            return {};
        }
    }
    if (head.empty()) return {};

    out.push_back(head);
    for (std::size_t i = 1; i < tokens.size(); ++i) out.push_back(tokens[i]);
    return out;
}

bool Dispatcher::dispatchMessage(MessageContext& ctx) {
    std::vector<MEntry> handlers;
    std::vector<MessageHandler> watchers;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        handlers = messageHandlers_;
        watchers = watchers_;
    }
    // Watchers run for every message and never stop propagation — Pyrogram's
    // separate handler groups, which is how the chat watcher coexists with the
    // command handlers in the Python bot.
    for (const auto& w : watchers)
        w(ctx);
    for (const auto& e : handlers) {
        if (e.filter(ctx)) {
            e.handler(ctx);
            return true;   // first match wins (stop propagation)
        }
    }
    return false;
}

bool Dispatcher::dispatchCallback(CallbackContext& ctx) {
    std::vector<CEntry> handlers;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        handlers = callbackHandlers_;
    }
    for (const auto& e : handlers) {
        if (e.filter(ctx)) {
            e.handler(ctx);
            return true;
        }
    }
    return false;
}

void Dispatcher::handleUpdate(const std::string& updateJson) {
    TelegramClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        client = client_;
    }

    json j = json::parse(updateJson, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    const std::string type = strField(j, "@type");

    if (type == "updateNewMessage" && j.contains("message") && j["message"].is_object()) {
        const json& m = j["message"];
        MessageContext ctx;
        ctx.client = client;
        ctx.chatId = intField(m, "chat_id");
        ctx.chatType = ctx.chatId >= 0 ? ChatType::Private : ChatType::Group;
        ctx.messageId = intField(m, "id");

        if (m.contains("sender_id") && m["sender_id"].is_object()) {
            const json& s = m["sender_id"];
            if (strField(s, "@type") == "messageSenderUser") {
                ctx.fromUserId = intField(s, "user_id");
            }
        }
        // Reply target. Current TDLib nests it as
        //   "reply_to": {"@type":"messageReplyToMessage","chat_id":…,"message_id":…}
        // while older builds exposed a flat "reply_to_message_id"; accept both,
        // and only when the reply points into this same chat.
        if (m.contains("reply_to") && m["reply_to"].is_object()) {
            const json& r = m["reply_to"];
            if (strField(r, "@type") == "messageReplyToMessage") {
                const std::int64_t rChat = intField(r, "chat_id");
                if (rChat == 0 || rChat == ctx.chatId)
                    ctx.replyToMessageId = intField(r, "message_id");
            }
        }
        
        auto parseEntities = [](const json& formattedText) -> std::vector<TextEntity> {
            std::vector<TextEntity> ents;
            if (formattedText.contains("entities") && formattedText["entities"].is_array()) {
                for (const auto& e : formattedText["entities"]) {
                    if (!e.is_object()) continue;
                    const json& typeObj = e["type"];
                    if (!typeObj.is_object()) continue;
                    
                    TextEntity ent;
                    ent.type = strField(typeObj, "@type");
                    ent.offset = intField(e, "offset");
                    ent.length = intField(e, "length");
                    if (ent.type == "textEntityTypeTextUrl") {
                        ent.url = strField(typeObj, "url");
                    }
                    ents.push_back(ent);
                }
            }
            return ents;
        };

        if (m.contains("content") && m["content"].is_object()) {
            const json& content = m["content"];
            const std::string cType = strField(content, "@type");
            if (cType == "messageText" && content.contains("text") && content["text"].is_object()) {
                ctx.text = strField(content["text"], "text");
                ctx.entities = parseEntities(content["text"]);
            } else if ((cType == "messagePhoto" || cType == "messageVideo" || cType == "messageAudio" || cType == "messageDocument" || cType == "messageVoiceNote") 
                       && content.contains("caption") && content["caption"].is_object()) {
                ctx.text = strField(content["caption"], "text");
                ctx.entities = parseEntities(content["caption"]);
            }
        }
        ctx.command = parseCommand(ctx.text);
        
        // Fetch replied media properties
        if (ctx.replyToMessageId != 0 && ctx.client) {
            std::string replyJsonStr = ctx.client->getMessageJson(ctx.chatId, ctx.replyToMessageId);
            if (!replyJsonStr.empty()) {
                json rj = json::parse(replyJsonStr, nullptr, false);
                if (!rj.is_discarded() && rj.is_object() && rj.contains("content") && rj["content"].is_object()) {
                    const json& rc = rj["content"];
                    const std::string rcType = strField(rc, "@type");
                    if (rcType == "messageAudio" && rc.contains("audio")) {
                        const json& aud = rc["audio"];
                        ctx.replyMedia.kind = "audio";
                        ctx.replyMedia.duration = intField(aud, "duration");
                        ctx.replyMedia.title = strField(aud, "title");
                        if (ctx.replyMedia.title.empty()) ctx.replyMedia.title = strField(aud, "file_name");
                        if (aud.contains("audio")) {
                            ctx.replyMedia.fileId = strField(aud["audio"], "id");
                            ctx.replyMedia.size = intField(aud["audio"], "expected_size");
                            if (ctx.replyMedia.fileId.empty()) { // older TDLib or fallback
                                ctx.replyMedia.fileId = std::to_string(intField(aud["audio"], "id"));
                            }
                        }
                    } else if (rcType == "messageVideo" && rc.contains("video")) {
                        const json& vid = rc["video"];
                        ctx.replyMedia.kind = "video";
                        ctx.replyMedia.duration = intField(vid, "duration");
                        ctx.replyMedia.title = strField(vid, "file_name");
                        if (vid.contains("video")) {
                            ctx.replyMedia.fileId = strField(vid["video"], "id");
                            ctx.replyMedia.size = intField(vid["video"], "expected_size");
                            if (ctx.replyMedia.fileId.empty()) {
                                ctx.replyMedia.fileId = std::to_string(intField(vid["video"], "id"));
                            }
                        }
                    } else if (rcType == "messageDocument" && rc.contains("document")) {
                        const json& doc = rc["document"];
                        ctx.replyMedia.kind = "document";
                        ctx.replyMedia.title = strField(doc, "file_name");
                        if (doc.contains("document")) {
                            ctx.replyMedia.fileId = strField(doc["document"], "id");
                            ctx.replyMedia.size = intField(doc["document"], "expected_size");
                            if (ctx.replyMedia.fileId.empty()) {
                                ctx.replyMedia.fileId = std::to_string(intField(doc["document"], "id"));
                            }
                        }
                    } else if (rcType == "messageVoiceNote" && rc.contains("voice_note")) {
                        const json& vn = rc["voice_note"];
                        ctx.replyMedia.kind = "voice";
                        ctx.replyMedia.duration = intField(vn, "duration");
                        ctx.replyMedia.title = "Voice Message";
                        if (vn.contains("voice")) {
                            ctx.replyMedia.fileId = strField(vn["voice"], "id");
                            ctx.replyMedia.size = intField(vn["voice"], "expected_size");
                            if (ctx.replyMedia.fileId.empty()) {
                                ctx.replyMedia.fileId = std::to_string(intField(vn["voice"], "id"));
                            }
                        }
                    }
                }
            }
        }
        
        dispatchMessage(ctx);

    } else if (type == "updateNewCallbackQuery") {
        CallbackContext ctx;
        ctx.client = client;
        ctx.queryId = intField(j, "id");
        ctx.fromUserId = intField(j, "sender_user_id");
        ctx.chatId = intField(j, "chat_id");
        ctx.messageId = intField(j, "message_id");
        if (j.contains("payload") && j["payload"].is_object()) {
            const json& p = j["payload"];
            if (strField(p, "@type") == "callbackQueryPayloadData") {
                ctx.data = base64Decode(strField(p, "data"));
            }
        }
        dispatchCallback(ctx);
    }
}

}  // namespace anonx
