// AnonXMusic C++ port — Phase 6a (command plugins)
// plugins.cpp — playback commands + the inline "controls" router (see plugins.hpp).
//
// Every string comes from the locale files, every keyboard from buttons.cpp, and
// every permission decision from guards.cpp, so this file is only control flow —
// which is exactly what has to match the Python plugins.

#include "anonx/plugins.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <optional>
#include <sstream>
#include <utility>

#include "anonx/buttons.hpp"
#include "anonx/guards.hpp"

namespace anonx {
namespace {

// Split a callback payload on single spaces (the format buttons.cpp emits).
std::vector<std::string> splitWs(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok)
        out.push_back(tok);
    return out;
}

// Strict "all digits" parse. Returns false on empty/overflow/garbage so callers
// can show the usage string instead of silently treating junk as 0.
bool parseU32(const std::string& text, long& out) {
    if (text.empty() || text.size() > 9)
        return false;
    long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

bool parseI64(const std::string& text, std::int64_t& out) {
    if (text.empty() || text.size() > 20)
        return false;
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        i = 1;
        if (text.size() == 1)
            return false;
    }
    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        value = value * 10 + (text[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

// A YouTube playlist link. Mirrors the `"list" in url` test in play.py.
bool isPlaylistUrl(const std::string& url) {
    return url.find("list=") != std::string::npos ||
           url.find("/playlist") != std::string::npos;
}

// Minimum seek step enforced by seek.py ("play_seek_min").
constexpr int kMinSeekSeconds = 10;

// Never seek into the last few seconds — the stream would end instantly.
constexpr int kSeekTailGuard = 10;

// Placeholder for a log-card field that could not be resolved. The same dash the
// Phase 6b notices use for a missing @username (admin_plugins.cpp).
const char kUnknownField[] = "-";

// ---- download progress (Phase 7.2) ----------------------------------------

// "dl_progress" carries a "{2:.1f}" spec, and formatStr deliberately ignores the
// spec part of a field, so the one decimal is applied here instead.
std::string percentText(const DownloadProgress& p) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", p.percent());
    return buf;
}

// One rendered progress line. Every unknown is rendered rather than guessed: the
// dash for a size yt-dlp has not announced yet, "0B" for an unknown speed and
// "--:--" for an unknown eta (both from the YouTube formatters).
std::string progressText(const LangView& L, const DownloadProgress& p) {
    return L.fmt("dl_progress", YouTube::humanBytes(p.downloaded),
                 p.total > 0 ? YouTube::humanBytes(p.total)
                             : std::string(kUnknownField),
                 percentText(p), YouTube::humanRate(p.speed),
                 YouTube::formatEta(p.eta));
}

}  // namespace

// ---------------------------------------------------------------------------
// construction / small helpers
// ---------------------------------------------------------------------------

Plugins::Plugins(const Deps& deps)
    : api_(deps.api),
      assistant_(deps.assistant),
      db_(deps.db),
      cache_(deps.cache), queue_(deps.queue),
      yt_(deps.yt), calls_(deps.calls), thumb_(deps.thumb), lang_(deps.lang), config_(deps.config),
      clock_([] {
          // steady_clock, not system_clock: only differences matter here, and a
          // wall-clock jump must not stall or flood the progress edits.
          using namespace std::chrono;
          return static_cast<std::int64_t>(
              duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                  .count());
      }) {}

void Plugins::setClock(Clock clock) {
    if (clock)
        clock_ = std::move(clock);
}

std::int64_t Plugins::nowMs() const { return clock_ ? clock_() : 0; }

bool Plugins::isSupergroupId(std::int64_t chatId) {
    return chatId <= -1000000000000LL;
}

std::string Plugins::htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            default:  out.push_back(c);
        }
    }
    return out;
}

LangView Plugins::tr(std::int64_t chatId) const {
    return lang_.view(db_.getLang(chatId));
}

void Plugins::setStatus(std::int64_t chatId, std::int64_t messageId) {
    if (messageId == 0)
        return;
    std::lock_guard<std::mutex> lk(mutex_);
    status_[chatId] = messageId;
}

std::int64_t Plugins::takeStatus(std::int64_t chatId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = status_.find(chatId);
    if (it == status_.end())
        return 0;
    const std::int64_t id = it->second;
    status_.erase(it);
    return id;
}

std::int64_t Plugins::say(std::int64_t chatId, const std::string& html,
                          const InlineKeyboard& kb) {
    const std::int64_t pending = takeStatus(chatId);
    if (pending != 0 && api_.editMessageText(chatId, pending, html, kb))
        return pending;
    return api_.sendMessage(chatId, html, kb);
}

std::int64_t Plugins::sayPhoto(std::int64_t chatId, const std::string& photoPath,
                               const std::string& captionHtml,
                               const InlineKeyboard& kb) {
    const std::int64_t pending = takeStatus(chatId);
    if (pending != 0) {
        if (api_.editMessageMedia(chatId, pending, photoPath, captionHtml, kb))
            return pending;
        api_.deleteMessage(chatId, pending);
    }
    return api_.sendPhoto(chatId, photoPath, captionHtml, kb);
}

// ---------------------------------------------------------------------------
// cards
// ---------------------------------------------------------------------------

std::string Plugins::nowPlayingCard(const LangView& L, const MediaItem& item) const {
    return L.fmt("play_media", item.url, htmlEscape(item.title), item.duration,
                 item.user);
}

std::string Plugins::queuedCard(const LangView& L, const MediaItem& item,
                                int position) const {
    return L.fmt("play_queued", position, item.url, htmlEscape(item.title),
                 item.duration, item.user);
}

void Plugins::sendQueuedCard(std::int64_t chatId, const LangView& L, const MediaItem& item, int position) {
    const std::string text = queuedCard(L, item, position);
    const InlineKeyboard kb = buttons::playQueued(chatId, item.id, L["play_now"]);
    if (config_.thumb_gen) {
        const std::string thumb = thumb_.render(item.thumbnail, item.title, item.duration, item.channel_name, item.user);
        if (!thumb.empty()) {
            sayPhoto(chatId, thumb, text, kb);
            return;
        }
    }
    say(chatId, text, kb);
}

// ---------------------------------------------------------------------------
// the play log
// ---------------------------------------------------------------------------

void Plugins::postPlayLog(const CommandEvent& ev, const MediaItem& item) {
    // The same two-part gate AdminPlugins::toLogGroup applies — a configured log
    // group and the /logger toggle. It is repeated rather than shared because the
    // two plugin classes are deliberately independent translation units, and the
    // gate is cheaper to duplicate than a base class is to justify.
    if (config_.logger_id == 0 || !db_.getLoggerEnabled())
        return;
    // Playing inside the log group would make it talk to itself.
    if (ev.chatId == config_.logger_id)
        return;

    // The log group reads in the bot's default language, not the played chat's —
    // matching the Phase 6b "New Chat Log" / "New User Log" notices.
    const LangView L = lang_.view(lang_.defaultCode());

    // messageLink() is a live lookup, so it is only paid for when the log is on.
    // It returns "" for chats that have no public link form; the card keeps its
    // shape either way.
    std::string link = api_.messageLink(ev.chatId, ev.messageId);
    if (link.empty())
        link = kUnknownField;

    // Both a chat title and a track title are free text, so both are escaped;
    // userMention() already returns markup and must not be.
    std::string title = htmlEscape(api_.chatTitle(ev.chatId));
    if (title.empty())
        title = kUnknownField;

    api_.sendMessage(config_.logger_id,
                     L.fmt("play_log", htmlEscape(api_.botName()), ev.chatId, title,
                           ev.fromUserId, api_.userMention(ev.fromUserId), link,
                           htmlEscape(item.title), item.duration));
}

// ---------------------------------------------------------------------------
// CallManager callbacks
// ---------------------------------------------------------------------------

std::int64_t Plugins::renderNowPlaying(std::int64_t chatId, const MediaItem& item) {
    const LangView L = tr(chatId);
    const std::string text = nowPlayingCard(L, item);
    const InlineKeyboard kb = buttons::controls(chatId);

    if (config_.thumb_gen) {
        const std::string thumb = thumb_.render(item.thumbnail, item.title, item.duration, item.channel_name, item.user);
        if (!thumb.empty()) {
            return sayPhoto(chatId, thumb, text, kb);
        }
    }
    return say(chatId, text, kb);
}

void Plugins::renderNotice(std::int64_t chatId, CallManager::Notice notice) {
    const LangView L = tr(chatId);
    std::string text;
    bool becomesCard = false;   // the upcoming "now playing" card reuses this message

    switch (notice) {
        case CallManager::Notice::PlayAgain:
            text = L["play_again"];
            becomesCard = true;
            break;
        case CallManager::Notice::PlayNext:
            text = L["play_next"];
            becomesCard = true;
            break;
        case CallManager::Notice::ErrorNoFile:
            text = L.fmt("error_no_file", config_.support_chat);
            break;
        case CallManager::Notice::ErrorNoCall:
            text = L["error_no_call"];
            break;
        case CallManager::Notice::ErrorNoAudio:
            text = L["error_no_audio"];
            break;
        case CallManager::Notice::ErrorServer:
            text = L["error_tg_server"];
            break;
        case CallManager::Notice::ErrorRtmp:
            text = L["error_rtmp"];
            break;
    }

    const std::int64_t id = say(chatId, text);
    if (becomesCard)
        setStatus(chatId, id);
}

CallManager::Callbacks Plugins::callbacks() {
    CallManager::Callbacks cb;
    cb.download = [this](std::int64_t chatId, const MediaItem& item) {
        return fetchMedia(chatId, item);
    };
    cb.onNowPlaying = [this](std::int64_t chatId, const MediaItem& item) {
        return renderNowPlaying(chatId, item);
    };
    cb.onNotice = [this](std::int64_t chatId, CallManager::Notice notice) {
        renderNotice(chatId, notice);
    };
    cb.onDeleteMessage = [this](std::int64_t chatId, std::int64_t messageId) {
        api_.deleteMessage(chatId, messageId);
    };
    return cb;
}

void Plugins::attachCallbacks() { calls_.setCallbacks(callbacks()); }

// ---------------------------------------------------------------------------
// download progress + cancellation (Phase 7.2)
// ---------------------------------------------------------------------------

bool Plugins::beginDownload(std::int64_t chatId, const std::string& videoId) {
    std::lock_guard<std::mutex> lk(mutex_);
    // The video id is claimed globally: two chats asking for the same track at
    // the same moment would otherwise race for the same output file. The chat is
    // claimed too, so one chat never shows two progress bars at once.
    if (activeIds_.count(videoId) != 0 || downloads_.count(chatId) != 0)
        return false;
    activeIds_.insert(videoId);
    ActiveDownload d;
    d.videoId = videoId;
    downloads_[chatId] = d;
    return true;
}

std::int64_t Plugins::endDownload(std::int64_t chatId, const std::string& videoId) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::int64_t cancelledBy = 0;
    auto it = downloads_.find(chatId);
    if (it != downloads_.end()) {
        cancelledBy = it->second.cancelledBy;
        downloads_.erase(it);
    }
    activeIds_.erase(videoId);
    return cancelledBy;
}

bool Plugins::requestCancel(std::int64_t chatId, std::int64_t userId) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = downloads_.find(chatId);
    if (it == downloads_.end())
        return false;
    it->second.cancel      = true;
    it->second.cancelledBy = userId;
    return true;
}

bool Plugins::onDownloadProgress(std::int64_t chatId, const DownloadProgress& p) {
    std::int64_t msgId = 0;
    bool create = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = downloads_.find(chatId);
        if (it == downloads_.end())
            return false;   // nothing tracks this fetch any more -> stop it
        if (it->second.cancel)
            return false;   // the Cancel button was pressed
        const std::int64_t now = nowMs();
        if (it->second.messageId == 0) {
            create = true;  // no message to edit yet (an auto-advance fetch)
        } else if (it->second.lastEditMs != 0 &&
                   now - it->second.lastEditMs < kProgressEditIntervalMs) {
            return true;    // too soon; keep downloading, say nothing
        }
        it->second.lastEditMs = now;
        msgId = it->second.messageId;
    }

    const LangView      L  = tr(chatId);
    const InlineKeyboard kb = buttons::cancelDl(L["cancel"]);

    if (create) {
        // First report with nowhere to put it: open the progress message with
        // "processing", and let the next tick fill in the numbers.
        const std::int64_t sent = api_.sendMessage(chatId, L["processing"], kb);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = downloads_.find(chatId);
        if (it != downloads_.end()) {
            it->second.messageId = sent;
            it->second.announced = true;
        }
        return true;
    }

    api_.editMessageText(chatId, msgId, progressText(L, p), kb);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = downloads_.find(chatId);
        if (it != downloads_.end())
            it->second.announced = true;
    }
    return true;
}

CallManager::MediaFetch Plugins::fetchMedia(std::int64_t chatId,
                                            const MediaItem& item) {
    CallManager::MediaFetch out;
    const LangView L = tr(chatId);

    // Already downloading this track -> refuse, and say so. That message is what
    // makes this an ABORTED fetch rather than a failed one: the engine must not
    // add its own "error_no_file" on top.
    if (!beginDownload(chatId, item.id)) {
        say(chatId, L["dl_active"]);
        out.aborted = true;
        return out;
    }

    // The request's status message ("Downloading…") becomes the progress bar, so
    // the whole /play flow stays inside a single message. It is TAKEN rather than
    // edited: a cached file must not be announced as a download at all.
    const std::int64_t statusMsg = takeStatus(chatId);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = downloads_.find(chatId);
        if (it != downloads_.end())
            it->second.messageId = statusMsg;
    }

    const std::int64_t startedMs = nowMs();
    
    DownloadResult res;
    if (item.url.rfind("tg://", 0) == 0) {
        // Parse the fileId from the URL or just use item.id which we set to fileId
        int32_t fileId = 0;
        try { fileId = std::stoi(item.id); } catch (...) {}
        if (fileId != 0) {
            res = api_.downloadFile(fileId, [this, chatId](const DownloadProgress& p) {
                return onDownloadProgress(chatId, p);
            });
        } else {
            res.status = DownloadStatus::Failed;
        }
    } else {
        res = yt_.downloadStream(item.id, item.video,
                               [this, chatId](const DownloadProgress& p) {
                                   return onDownloadProgress(chatId, p);
                               });
    }

    // Read the message back: onDownloadProgress may have created it, and only it
    // knows whether a progress bar was ever drawn.
    std::int64_t msgId     = 0;
    bool         announced = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = downloads_.find(chatId);
        if (it != downloads_.end()) {
            msgId     = it->second.messageId;
            announced = it->second.announced;
        }
    }
    const std::int64_t cancelledBy = endDownload(chatId, item.id);

    switch (res.status) {
        case DownloadStatus::Ok:
            out.path = res.path;
            // A progress bar was drawn -> close it with "dl_complete". A cache hit
            // drew nothing, so its message is handed straight back untouched.
            if (msgId != 0 && announced)
                api_.editMessageText(
                    chatId, msgId,
                    L.fmt("dl_complete", (nowMs() - startedMs) / 1000), {});
            setStatus(chatId, msgId);   // the now-playing card replaces it
            break;

        case DownloadStatus::Cancelled:
            out.aborted = true;
            if (msgId != 0)
                api_.editMessageText(
                    chatId, msgId,
                    L.fmt("dl_cancel", api_.userMention(cancelledBy)), {});
            break;

        case DownloadStatus::TooLarge:
            out.aborted = true;
            if (msgId != 0)
                api_.editMessageText(chatId, msgId, L["dl_limit"], {});
            break;

        case DownloadStatus::Failed:
            // An ordinary failure: hand the message back so the engine's
            // "error_no_file" notice lands in it.
            setStatus(chatId, msgId);
            break;
    }
    return out;
}

void Plugins::onCancelDownload(const ButtonEvent& ev) {
    // Inline.cancel_dl's payload is the bare word, byte-faithful to the Python
    // bot, so the chat comes from the card the button sits on. No permission gate:
    // the button only ever appears on a download the chat is waiting for, and the
    // requester is usually not an admin of it.
    const LangView L = tr(ev.chatId);
    if (!requestCancel(ev.chatId, ev.fromUserId)) {
        api_.answerCallback(ev.queryId, L["dl_not_found"], true);
        return;
    }
    // The kill happens on the downloading thread, at its next progress line.
    api_.answerCallback(ev.queryId, L["dl_cancelling"], false);
}

// ---------------------------------------------------------------------------
// command name tables
// ---------------------------------------------------------------------------

std::vector<std::string> Plugins::playCommands() {
    // The name itself carries the flags: a leading "v" means video, a trailing
    // "force" means skip the queue (see guards::runPlayPreflight).
    return {"play", "vplay", "playforce", "vplayforce"};
}
std::vector<std::string> Plugins::skipCommands()   { return {"skip"}; }
std::vector<std::string> Plugins::pauseCommands()  { return {"pause"}; }
std::vector<std::string> Plugins::resumeCommands() { return {"resume"}; }
std::vector<std::string> Plugins::stopCommands()   { return {"stop", "end"}; }
std::vector<std::string> Plugins::loopCommands()   { return {"loop"}; }
std::vector<std::string> Plugins::queueCommands()  { return {"queue"}; }
std::vector<std::string> Plugins::seekCommands()   { return {"seek", "seekback"}; }

// ---------------------------------------------------------------------------
// /play /vplay /playforce /vplayforce
// ---------------------------------------------------------------------------

void Plugins::onPlay(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);

    guards::PlayRequest req;
    req.chatId       = ev.chatId;
    req.fromUserId   = ev.fromUserId;
    req.isSupergroup = !ev.isPrivate && isSupergroupId(ev.chatId);
    req.hasReply     = false;   // replied media is deferred (see plugins.hpp)
    req.command      = ev.command;

    const guards::PlayPreflight pre =
        guards::runPlayPreflight(api_, assistant_, db_, queue_, yt_, config_, req);

    switch (pre.gate) {
        case guards::PlayGate::UserInvalid:
            api_.sendMessage(ev.chatId, L["play_user_invalid"]);
            return;
        case guards::PlayGate::ChatInvalid:
            api_.sendMessage(ev.chatId, L["play_chat_invalid"]);
            api_.leaveChat(ev.chatId);
            return;
        case guards::PlayGate::Usage:
            api_.sendMessage(ev.chatId, L["play_usage"]);
            return;
        case guards::PlayGate::QueueFull:
            api_.sendMessage(ev.chatId, L.fmt("play_queue_full", config_.queue_limit));
            return;
        case guards::PlayGate::NotFound:
            api_.sendMessage(ev.chatId, L.fmt("play_not_found", config_.support_chat));
            return;
        case guards::PlayGate::AdminOnly:
            api_.sendMessage(ev.chatId, L["play_admin"]);
            return;
        case guards::PlayGate::AssistantBanned:
            api_.sendMessage(ev.chatId, L["play_banned"]);
            return;
        case guards::PlayGate::BotLacksInvitePermission:
            api_.sendMessage(ev.chatId, L["admin_required"]);
            return;
        case guards::PlayGate::AssistantJoinFailed:
            api_.sendMessage(ev.chatId, L["play_invite_error"]);
            return;
        case guards::PlayGate::UnsupportedChat:
            api_.sendMessage(ev.chatId, L["play_unsupported"]);
            return;
        case guards::PlayGate::Proceed:
            break;
    }

    const std::string mention = api_.userMention(ev.fromUserId);

    // The track this request accepted, once one is known. It is logged after the
    // branches below, so every path that ends in a stream or a queue slot logs
    // exactly once — and the paths that end in an error (playlist fetch failed,
    // nothing found, over the duration limit) log nothing, because nothing was
    // played. Python calls play_logs at the same point, per command rather than
    // per track.
    std::optional<MediaItem> logged;

    // One status message carries the whole request: "Searching…" becomes
    // "Downloading…" becomes either the now-playing card (rendered from inside
    // CallManager) or the "added to queue" card.
    setStatus(ev.chatId, api_.sendMessage(ev.chatId, L["play_searching"]));

    if (pre.m3u8) {
        // A direct/HLS link: nothing to search or download — the transport
        // streams the URL itself, so file_path IS the url.
        MediaItem item;
        item.id        = pre.url;
        item.title     = pre.url;
        item.url       = pre.url;
        item.file_path = pre.url;
        item.duration  = "00:00";
        item.channel_name = mention; // fallback
        item.user      = mention;
        item.video     = pre.video;

        const CallManager::PlayDecision decision =
            calls_.play(ev.chatId, item, pre.force);
        if (decision.outcome == CallManager::PlayOutcome::Queued) {
            sendQueuedCard(ev.chatId, L, item, decision.position);
        }
        logged = item;
    } else if (req.hasReply()) {
        if (req.replyMedia.size > config_.max_download_bytes) {
            takeStatus(ev.chatId);
            api_.sendMessage(ev.chatId, L.fmt("dl_limit_size", config_.max_download_bytes / 1024 / 1024));
            return;
        }
        MediaItem item;
        item.id = req.replyMedia.fileId;
        item.title = req.replyMedia.title.empty() ? "Telegram Media" : req.replyMedia.title;
        item.url = "tg://" + req.replyMedia.fileId;
        
        int secs = req.replyMedia.duration;
        int h = secs / 3600;
        int m = (secs % 3600) / 60;
        int s = secs % 60;
        char buf[32];
        if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
        else snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
        
        item.duration = buf;
        item.duration_sec = secs;
        item.channel_name = api_.chatTitle(ev.chatId);
        if (item.channel_name.empty()) item.channel_name = "User";
        item.message_id = ev.messageId;
        item.time = nowMs();
        item.user = mention;
        item.video = (req.replyMedia.kind == "video");
        
        const CallManager::PlayDecision decision =
            calls_.play(ev.chatId, item, pre.force);
        if (decision.outcome == CallManager::PlayOutcome::Queued) {
            sendQueuedCard(ev.chatId, L, item, decision.position);
        }
        logged = item;
    } else if (!pre.url.empty() && isPlaylistUrl(pre.url)) {
        // say() consumes the slot, so re-arm it with the message it just wrote.
        setStatus(ev.chatId, say(ev.chatId, L["playlist_fetch"]));

        const std::vector<MediaItem> tracks =
            yt_.playlist(pre.url, config_.playlist_limit, mention, pre.video);
        if (tracks.empty()) {
            say(ev.chatId, L["playlist_error"]);
            return;
        }

        // The first track starts (or queues, if something is already playing);
        // the rest queue behind it, exactly as play.py loops over the results.
        for (const MediaItem& track : tracks)
            calls_.play(ev.chatId, track, false);

        std::string summary = L.fmt("playlist_queued", tracks.size());
        int index = 1;
        for (const MediaItem& track : tracks) {
            summary += L.fmt("queue_item", index++, htmlEscape(track.title),
                             track.duration);
        }
        // If the first track started streaming, the status message has already
        // become the now-playing card, so say() posts the summary separately.
        say(ev.chatId, summary);

        // One card per command, so a 20-track playlist logs its head track
        // rather than flooding the log group.
        logged = tracks.front();
    } else {
        const std::string query = pre.url.empty() ? pre.query : pre.url;
        std::optional<MediaItem> track = yt_.search(query, ev.messageId, pre.video);
        if (!track) {
            say(ev.chatId, L.fmt("play_not_found", config_.support_chat));
            return;
        }
        if (config_.duration_limit_seconds > 0 &&
            track->duration_sec > config_.duration_limit_seconds) {
            say(ev.chatId, L.fmt("play_duration_limit",
                                 config_.duration_limit_seconds / 60));
            return;
        }

        track->user  = mention;
        track->video = pre.video;

        // Re-arm the slot: the card will replace this "Downloading…" message.
        setStatus(ev.chatId, say(ev.chatId, L["play_downloading"]));

        const CallManager::PlayDecision decision =
            calls_.play(ev.chatId, *track, pre.force);
        if (decision.outcome == CallManager::PlayOutcome::Queued) {
            sendQueuedCard(ev.chatId, L, *track, decision.position);
        }
        // A cancelled / refused download never became a stream or a queue slot,
        // so there is nothing to log: the play log records what was played.
        if (decision.outcome != CallManager::PlayOutcome::Aborted)
            logged = *track;
    }

    // Logged before the command message is deleted: the card carries a link to
    // that message, and a link to something already gone is worth less.
    if (logged)
        postPlayLog(ev, *logged);

    if (pre.cmdDelete)
        api_.deleteMessage(ev.chatId, ev.messageId);
}

// ---------------------------------------------------------------------------
// the admin-only playback commands
// ---------------------------------------------------------------------------

bool Plugins::requireControl(const CommandEvent& ev, const LangView& L) {
    if (!guards::canManageVc(api_, db_, ev.chatId, ev.fromUserId)) {
        api_.sendMessage(ev.chatId, L["user_not_admin"]);
        return false;
    }
    if (!cache_.isActiveCall(ev.chatId)) {
        api_.sendMessage(ev.chatId, L["not_playing"]);
        return false;
    }
    return true;
}

void Plugins::onSkip(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!requireControl(ev, L))
        return;

    api_.sendMessage(ev.chatId, L.fmt("play_skipped", api_.userMention(ev.fromUserId)));
    // Note: playNext honours the loop counter, so /skip on a looping track
    // replays it — the same behaviour as the Python skip handler, which also
    // just delegates to play_next.
    calls_.playNext(ev.chatId);
}

void Plugins::onPause(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!requireControl(ev, L))
        return;
    if (!cache_.isPlaying(ev.chatId)) {
        api_.sendMessage(ev.chatId, L["play_already_paused"]);
        return;
    }
    if (!calls_.pause(ev.chatId)) {
        // pause() already stopped the chat on failure; the only useful thing
        // left to say is that there is no live call any more.
        api_.sendMessage(ev.chatId, L["error_no_call"]);
        return;
    }
    api_.sendMessage(ev.chatId, L.fmt("play_paused", api_.userMention(ev.fromUserId)));
}

void Plugins::onResume(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!requireControl(ev, L))
        return;
    if (cache_.isPlaying(ev.chatId)) {
        api_.sendMessage(ev.chatId, L["play_not_paused"]);
        return;
    }
    if (!calls_.resume(ev.chatId)) {
        api_.sendMessage(ev.chatId, L["error_no_call"]);
        return;
    }
    api_.sendMessage(ev.chatId, L.fmt("play_resumed", api_.userMention(ev.fromUserId)));
}

void Plugins::onStop(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!requireControl(ev, L))
        return;
    calls_.stop(ev.chatId);
    api_.sendMessage(ev.chatId, L.fmt("play_stopped", api_.userMention(ev.fromUserId)));
}

void Plugins::onLoop(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    if (!requireControl(ev, L))
        return;

    if (ev.command.size() < 2) {
        api_.sendMessage(ev.chatId, L.fmt("loop_count", cache_.getLoop(ev.chatId)));
        return;
    }

    const std::string arg = toLower(ev.command[1]);
    if (arg == "off" || arg == "disable" || arg == "0") {
        cache_.setLoop(ev.chatId, 0);
        api_.sendMessage(ev.chatId, L["loop_off"]);
        return;
    }

    long count = 0;
    if (!parseU32(arg, count)) {
        api_.sendMessage(ev.chatId, L["loop_usage"]);
        return;
    }
    cache_.setLoop(ev.chatId, static_cast<int>(count));
    api_.sendMessage(ev.chatId, L.fmt("loop_set", count));
}

void Plugins::onQueue(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    // Reading the queue needs no permissions — only an active stream.
    if (!cache_.isActiveCall(ev.chatId) || queue_.empty(ev.chatId)) {
        api_.sendMessage(ev.chatId, L["not_playing"]);
        return;
    }

    setStatus(ev.chatId, api_.sendMessage(ev.chatId, L["queue_fetching"]));

    const std::vector<MediaItem> items = queue_.getQueue(ev.chatId);
    std::string text = L.fmt("queue_curr", items[0].url, htmlEscape(items[0].title),
                             items[0].duration, items[0].user);
    for (std::size_t i = 1; i < items.size(); ++i) {
        text += L.fmt("queue_item", i, htmlEscape(items[i].title), items[i].duration);
    }

    // The button label shows the current state; pressing it toggles.
    const bool playing = cache_.isPlaying(ev.chatId);
    say(ev.chatId, text,
        buttons::queueMarkup(ev.chatId, playing ? L["playing"] : L["paused"], playing));
}

void Plugins::onSeek(const CommandEvent& ev) {
    const LangView L = tr(ev.chatId);
    const std::string name = ev.command.empty() ? std::string("seek") : ev.command[0];
    const bool backward = name == "seekback";

    if (!requireControl(ev, L))
        return;

    long requested = 0;
    if (ev.command.size() < 2 || !parseU32(ev.command[1], requested)) {
        api_.sendMessage(ev.chatId, L.fmt("play_seek_usage", name));
        return;
    }
    if (requested < kMinSeekSeconds) {
        api_.sendMessage(ev.chatId, L["play_seek_min"]);
        return;
    }

    std::optional<MediaItem> current = queue_.getCurrent(ev.chatId);
    if (!current) {
        api_.sendMessage(ev.chatId, L["not_playing"]);
        return;
    }
    if (current->duration_sec <= 0) {
        // A live stream or a direct link — nothing to seek within.
        api_.sendMessage(ev.chatId, L["play_seek_no_dur"]);
        return;
    }

    // MediaItem::time holds the offset the stream was last started from. (The
    // Python bot asks pytgcalls for played_time; the abstract transport has no
    // such query, so the applied offset is tracked here instead.)
    const long long from = current->time > 0 ? current->time : 0;
    long long target = backward ? from - requested : from + requested;
    const long long last = current->duration_sec > kSeekTailGuard
                               ? current->duration_sec - kSeekTailGuard
                               : 0;
    target = std::max<long long>(0, std::min<long long>(target, last));

    setStatus(ev.chatId, api_.sendMessage(ev.chatId, L["play_seeking"]));

    current->time = static_cast<int>(target);
    queue_.replaceCurrent(ev.chatId, *current);
    // seekTime != 0 makes playMedia restart the stream at the offset without
    // re-rendering the now-playing card.
    calls_.playMedia(ev.chatId, *current, static_cast<int>(target));

    say(ev.chatId, L.fmt("play_seeked", backward ? L["backward"] : L["forward"],
                         target, api_.userMention(ev.fromUserId)));
}

// ---------------------------------------------------------------------------
// the "controls …" inline keyboard
// ---------------------------------------------------------------------------

void Plugins::onControls(const ButtonEvent& ev) {
    // "controls <action> <chatId> [itemId|q]" — the exact payloads buttons.cpp
    // emits. Anything else means the card predates a restart/upgrade.
    const std::vector<std::string> parts = splitWs(ev.data);
    std::int64_t chatId = 0;
    if (parts.size() < 3 || parts[0] != "controls" || !parseI64(parts[2], chatId)) {
        api_.answerCallback(ev.queryId, lang_.view(config_.lang_code)["play_expired"],
                            true);
        return;
    }

    const std::string& action = parts[1];
    const std::string  extra  = parts.size() > 3 ? parts[3] : std::string();
    const bool queueCard = extra == "q";
    const LangView L = tr(chatId);

    // The status/timer button is a label, not an action: report the live state.
    if (action == "status") {
        api_.answerCallback(ev.queryId,
                            cache_.isPlaying(chatId) ? L["playing"] : L["paused"],
                            false);
        return;
    }

    if (!guards::canManageVc(api_, db_, chatId, ev.fromUserId)) {
        api_.answerCallback(ev.queryId, L["user_not_admin"], true);
        return;
    }

    if (action == "force") {
        // "Play Now" on an "added to queue" card.
        const std::pair<int, std::optional<MediaItem>> found =
            queue_.checkItem(chatId, extra);
        if (found.first < 0 || !found.second) {
            api_.answerCallback(ev.queryId, L["play_expired"], true);
            return;
        }
        // CallManager::play(force) promotes the item with forceAdd(removeAt = 0),
        // which does not drop the copy still sitting at its old position — so the
        // duplicate is erased here first (forceAdd's removeAt does exactly that).
        if (found.first > 0)
            queue_.forceAdd(chatId, *found.second, found.first);
        calls_.play(chatId, *found.second, true);

        api_.editMessageReplyMarkup(chatId, ev.messageId, {});  // button is spent
        api_.answerCallback(ev.queryId, L["play_now"], false);
        return;
    }

    if (!cache_.isActiveCall(chatId)) {
        api_.answerCallback(ev.queryId, L["not_playing"], true);
        return;
    }

    std::string status;
    bool removeTransport = false;   // terminal action -> drop the transport row

    if (action == "pause") {
        if (!cache_.isPlaying(chatId)) {
            api_.answerCallback(ev.queryId, L["play_already_paused"], true);
            return;
        }
        calls_.pause(chatId);
        status = L["paused"];
    } else if (action == "resume") {
        if (cache_.isPlaying(chatId)) {
            api_.answerCallback(ev.queryId, L["play_not_paused"], true);
            return;
        }
        calls_.resume(chatId);
        status = L["playing"];
    } else if (action == "replay") {
        calls_.replay(chatId);
        status = L["replayed"];
    } else if (action == "skip") {
        calls_.playNext(chatId);
        status = L["skipped"];
        removeTransport = true;
    } else if (action == "stop") {
        calls_.stop(chatId);
        status = L["stopped"];
        removeTransport = true;
    } else {
        api_.answerCallback(ev.queryId, L["play_expired"], true);
        return;
    }

    if (queueCard) {
        // The queue card keeps its text; only its toggle button flips.
        api_.editMessageReplyMarkup(
            chatId, ev.messageId,
            buttons::queueMarkup(chatId, status, cache_.isPlaying(chatId)));
    } else {
        // The now-playing card keeps its text too, but gains a status label. Its
        // text has to be read back because the card may have been rendered by a
        // previous process (the callback carries no message text).
        const std::string text = api_.getMessageText(chatId, ev.messageId);
        const InlineKeyboard kb = buttons::controls(chatId, status, "", removeTransport);
        if (text.empty())
            api_.editMessageReplyMarkup(chatId, ev.messageId, kb);
        else
            api_.editMessageText(chatId, ev.messageId, text, kb);
    }

    api_.answerCallback(ev.queryId, status, false);
}

}  // namespace anonx
