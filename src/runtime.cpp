// AnonXMusic C++ port — Integration phase
// runtime.cpp — implementation of the object graph. See runtime.hpp.

#include "anonx/runtime.hpp"

#include <string>
#include <utility>

#include "anonx/logger.hpp"
#include "anonx/plugins_router.hpp"

namespace anonx {
namespace {

Logger log() { return Logger("anonx.runtime"); }

// "https://t.me/DevilsHeavenMF" / "@DevilsHeavenMF" / "DevilsHeavenMF" -> the
// bare username. Returns "" when the link is not a t.me user/channel link (an
// invite link, for instance), in which case the assistants simply skip joining.
std::string usernameFromLink(const std::string& link) {
    std::string s = link;

    const std::size_t scheme = s.find("//");
    if (scheme != std::string::npos) s = s.substr(scheme + 2);

    const std::size_t slash = s.find('/');
    if (slash != std::string::npos) {
        // Drop the host part ("t.me/foo" -> "foo").
        s = s.substr(slash + 1);
    }
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
    if (!s.empty() && s.front() == '@') s.erase(s.begin());

    // Private invite links (t.me/+abc, t.me/joinchat/abc) cannot be resolved by
    // username, and a nested path is not a username either.
    if (s.empty() || s.front() == '+' || s.find('/') != std::string::npos) return {};
    return s;
}

// Options for the bot account. The bot half never needs an interactive login:
// a bot token authorizes in one step.
TelegramClient::Options botOptions(const Config& config,
                                  const Runtime::Options& opts) {
    TelegramClient::Options o;
    o.apiId = static_cast<int>(config.api_id);
    o.apiHash = config.api_hash;
    o.databaseDirectory = opts.botSessionDir;
    o.botToken = config.bot_token;
    o.name = "anonx";
    return o;
}

}  // namespace

Runtime::Runtime(const Config& config, VoiceTransport& transport, Options opts)
    : config_(config),
      opts_(std::move(opts)),
      db_(config.db_path),
      cache_(),
      lang_(),
      queue_(),
      yt_(),
      sys_(),
      bot_(botOptions(config, opts_)),
      api_(bot_),
      userbot_(static_cast<int>(config.api_id), config.api_hash),
      assistantApi_(userbot_),
      timer_(),
      cookieSrc_(std::make_unique<CurlCookieSource>()),
      calls_(transport, queue_, cache_, timer_, config_),
      plugins_(Plugins::Deps{api_, assistantApi_, db_, cache_, queue_, yt_, calls_, thumb_, lang_, config_}),
      admin_(AdminPlugins::Deps{api_, db_, cache_, calls_, sys_, thumb_, lang_, config_}) {}

Runtime::~Runtime() { stop(); }

bool Runtime::start() {
    if (started_.load()) return true;

    log().info(config_.redactedSummary());

    // ---- data layer -------------------------------------------------------
    // The database itself was opened by the constructor (Database's ctor is its
    // own connect step and throws on failure); here it only learns the two
    // config-derived defaults it needs before serving.
    db_.setDefaultLang(config_.lang_code);
    db_.setAssistantCount(config_.assistantCount());

    // ---- cookies ----------------------------------------------------------
    if (!config_.cookies_url.empty()) {
        log().info("Fetching " + std::to_string(config_.cookies_url.size()) + " cookie file(s) from COOKIES_URL...");
        cookieSrc_->fetch(config_.cookies_url);
    }

    // ---- language tables --------------------------------------------------
    const int loaded = lang_.loadDir(opts_.localesDir);
    if (loaded <= 0) {
        log().critical("no locale files found in '" + opts_.localesDir +
                       "' — every command would answer with placeholder keys");
        return false;
    }
    if (lang_.loaded(config_.lang_code)) {
        lang_.setDefault(config_.lang_code);
    } else {
        log().warning("LANG_CODE '" + config_.lang_code +
                      "' is not among the loaded locales; falling back to 'en'");
        lang_.setDefault("en");
    }
    log().info("Loaded " + std::to_string(loaded) + " language(s); default '" +
               lang_.defaultCode() + "'");

    // ---- the bot account --------------------------------------------------
    if (!bot_.boot()) {
        log().critical("bot account failed to authorize — check BOT_TOKEN");
        return false;
    }
    const TelegramClient::Me& me = bot_.me();
    log().info("Bot authorized: " + me.firstName +
               (me.username.empty() ? "" : " (@" + me.username + ")"));

    // ---- the assistant accounts ------------------------------------------
    // Non-fatal by design: without an assistant the bot cannot stream, but every
    // non-voice command still works, and this is exactly the state of a fresh
    // deployment whose PHONE_NUMBER logins have not been done yet.
    if (opts_.bootAssistants) {
        const std::vector<std::string> phones = config_.assistantPhones();
        if (phones.empty()) {
            log().warning("no PHONE_NUMBER* configured — assistants not booted, "
                          "so voice chats cannot be joined");
        } else {
            for (std::size_t i = 0; i < phones.size(); ++i) {
                Userbot::AssistantSpec spec;
                spec.name = "AnonyUB" + std::to_string(i + 1);
                spec.phoneNumber = phones[i];
                spec.sessionDirectory = "tdlib/assistant" + std::to_string(i + 1);
                userbot_.addAssistant(std::move(spec));
            }
            userbot_.setLoggerChatId(config_.logger_id);
            userbot_.setSupportChat(usernameFromLink(config_.support_chat));
            userbot_.setInteractiveLogin(opts_.interactiveAssistantLogin);

            if (!userbot_.bootAll()) {
                log().warning("not every assistant authorized (" +
                              std::to_string(assistantsUp()) + "/" +
                              std::to_string(phones.size()) + " up)");
            } else {
                log().info(std::to_string(assistantsUp()) + " assistant(s) up");
            }
        }
    }

    // ---- commands and routing --------------------------------------------
    // Created last and destroyed first: its handlers capture plugins_/admin_.
    // The cost of being last is a short deaf window — until attach() installs the
    // observer, TelegramClient::onUpdate() has nobody to hand a message to and
    // drops it. That is deliberate: attaching earlier would let a handler run
    // while an assistant's interactive login is blocking the receive pump, and
    // every invoke() it made would sit there until it timed out.
    dispatcher_ = std::make_unique<Dispatcher>();
    installPlugins(*dispatcher_, plugins_, admin_, db_);
    if (!me.username.empty()) dispatcher_->setBotUsername(me.username);
    dispatcher_->attach(bot_);

    started_.store(true);
    announceStartup();
    log().info("Running.");
    return true;
}

void Runtime::stop() {
    if (stopped_.exchange(true)) return;
    if (!started_.load()) return;   // nothing was ever wired up

    log().info("Stopping...");
    if (config_.logger_id != 0 && bot_.authorized()) {
        bot_.sendMessage(config_.logger_id, "<b>Bot Stopped</b>");
    }

    // Order matters. Detach the observer so no new update is queued, drain the
    // handler pool, close the accounts, then JOIN the receive pump — only after
    // all of that can no handler still be running, which is what makes
    // destroying the dispatcher (whose lambdas capture the plugins) safe.
    // Resetting it earlier would race with an update in flight.
    bot_.setUpdateObserver(nullptr);
    if (dispatcher_) dispatcher_->stopWorkers();
    userbot_.exitAll();
    bot_.exit();
    TdClient::stopRuntime();
    dispatcher_.reset();

    log().info("Stopped.");
}

std::size_t Runtime::assistantsUp() const {
    std::size_t n = 0;
    for (const std::unique_ptr<TelegramClient>& c : userbot_.clients()) {
        if (c && c->authorized()) ++n;
    }
    return n;
}

void Runtime::announceStartup() {
    if (config_.logger_id == 0) return;

    // Deliberately not a locale string: the log group is the operator's, not a
    // chat whose language the database knows, and the Python original posts a
    // fixed English notice here too (as Userbot::announce already does).
    // Secret-free: names and counts only, never a token, session or phone.
    const TelegramClient::Me& me = bot_.me();
    std::string card = "<b>Bot Started</b>\n\n";
    card += "<b>Bot:</b> " + me.firstName;
    if (!me.username.empty()) card += " | @" + me.username;
    card += "\n<b>Assistants:</b> " + std::to_string(assistantsUp()) + "/" +
            std::to_string(config_.assistantCount());
    card += "\n<b>Languages:</b> " + std::to_string(lang_.codes().size());
    card += "\n<b>Modules:</b> " + std::to_string(AdminPlugins::moduleCount());
    card += "\n<b>Chats served:</b> " + std::to_string(db_.chatCount());

    bot_.sendMessage(config_.logger_id, card);
}

}  // namespace anonx
