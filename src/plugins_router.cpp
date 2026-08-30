// AnonXMusic C++ port — Phase 6a (command plugins)
// plugins_router.cpp — Dispatcher wiring (see plugins_router.hpp).

#include "anonx/plugins_router.hpp"

#include <algorithm>
#include <cctype>

namespace anonx {

CommandEvent toCommandEvent(const MessageContext& ctx) {
    CommandEvent ev;
    ev.chatId     = ctx.chatId;
    ev.messageId  = ctx.messageId;
    ev.fromUserId = ctx.fromUserId;
    ev.isPrivate  = ctx.chatType == ChatType::Private;
    ev.command    = ctx.command;
    ev.replyToMessageId = ctx.replyToMessageId;
    ev.entities   = ctx.entities;
    ev.replyMedia = ctx.replyMedia;
    if (!ev.command.empty()) {
        std::string& name = ev.command[0];
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    }
    return ev;
}

ButtonEvent toButtonEvent(const CallbackContext& ctx) {
    ButtonEvent ev;
    ev.chatId     = ctx.chatId;
    ev.messageId  = ctx.messageId;
    ev.fromUserId = ctx.fromUserId;
    ev.queryId    = ctx.queryId;
    ev.data       = ctx.data;
    return ev;
}

void installPlugins(Dispatcher& disp, Plugins& plugins, AdminPlugins& admin,
                    Database& db) {
    // The engine renders its cards and notices through the plugins.
    plugins.attachCallbacks();

    // ~app.bl_users / ~app.bl_chats: evaluated per message so a /blacklist takes
    // effect immediately (the Python bot keeps in-memory sets refreshed on write).
    const Filter allowed =
        !filters::userWhere([&db](std::int64_t userId) {
            return userId != 0 && db.isBlacklistedUser(userId);
        }) &&
        !Filter([&db](const MessageContext& m) { return db.isBlacklistedChat(m.chatId); });

    // Every playback command is group-only, matching filters.group in Python.
    const Filter inGroup = filters::groupChat() && allowed;

    disp.onMessage(filters::command(Plugins::playCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onPlay(toCommandEvent(m)); });
    disp.onMessage(filters::command(Plugins::skipCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onSkip(toCommandEvent(m)); });
    disp.onMessage(filters::command(Plugins::pauseCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onPause(toCommandEvent(m)); });
    disp.onMessage(filters::command(Plugins::resumeCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onResume(toCommandEvent(m)); });
    disp.onMessage(filters::command(Plugins::stopCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onStop(toCommandEvent(m)); });
    disp.onMessage(filters::command(Plugins::loopCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onLoop(toCommandEvent(m)); });
    disp.onMessage(filters::command(Plugins::queueCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onQueue(toCommandEvent(m)); });
    disp.onMessage(filters::command(Plugins::seekCommands()) && inGroup,
                   [&plugins](MessageContext& m) { plugins.onSeek(toCommandEvent(m)); });

    // ---- Phase 6b -------------------------------------------------------
    // The auth list and the settings card are per-group state, so those two stay
    // group-only; everything else also answers in private (which is where the
    // sudo commands are normally used).
    const auto adminHandler = [&admin](void (AdminPlugins::*fn)(const CommandEvent&)) {
        return [&admin, fn](MessageContext& m) { (admin.*fn)(toCommandEvent(m)); };
    };

    disp.onMessage(filters::command(AdminPlugins::authCommands()) && inGroup,
                   adminHandler(&AdminPlugins::onAuth));
    disp.onMessage(filters::command(AdminPlugins::authListCommands()) && inGroup,
                   adminHandler(&AdminPlugins::onAuthList));
    disp.onMessage(filters::command(AdminPlugins::settingsCommands()) && inGroup,
                   adminHandler(&AdminPlugins::onSettings));

    disp.onMessage(filters::command(AdminPlugins::blacklistCommands()) && allowed,
                   adminHandler(&AdminPlugins::onBlacklist));
    disp.onMessage(filters::command(AdminPlugins::gcastCommands()) && allowed,
                   adminHandler(&AdminPlugins::onGcast));
    disp.onMessage(filters::command(AdminPlugins::sudoCommands()) && allowed,
                   adminHandler(&AdminPlugins::onSudo));
    disp.onMessage(filters::command(AdminPlugins::sudoListCommands()) && allowed,
                   adminHandler(&AdminPlugins::onSudoList));
    disp.onMessage(filters::command(AdminPlugins::langCommands()) && allowed,
                   adminHandler(&AdminPlugins::onLang));
    disp.onMessage(filters::command(AdminPlugins::pingCommands()) && allowed,
                   adminHandler(&AdminPlugins::onPing));
    disp.onMessage(filters::command(AdminPlugins::statsCommands()) && allowed,
                   adminHandler(&AdminPlugins::onStats));
    disp.onMessage(filters::command(AdminPlugins::activeVcCommands()) && allowed,
                   adminHandler(&AdminPlugins::onActiveVc));
    disp.onMessage(filters::command(AdminPlugins::startCommands()) && allowed,
                   adminHandler(&AdminPlugins::onStart));
    disp.onMessage(filters::command(AdminPlugins::helpCommands()) && allowed,
                   adminHandler(&AdminPlugins::onHelp));
    disp.onMessage(filters::command(AdminPlugins::loggerCommands()) && allowed,
                   adminHandler(&AdminPlugins::onLogger));

    // The chat watcher is its own Pyrogram handler group in Python: it sees every
    // message and never stops the command handlers from running.
    disp.onEveryMessage([&admin](MessageContext& m) { admin.onSeen(toCommandEvent(m)); });

    // filters.regex("controls") — every player button carries this prefix.
    disp.onCallback(filters::callbackDataPrefix("controls"),
                    [&plugins](CallbackContext& c) { plugins.onControls(toButtonEvent(c)); });

    // filters.regex("cancel_dl") — the Cancel button on a download in progress.
    // Its payload carries no chat id, so the handler reads it off the message.
    disp.onCallback(filters::callbackDataPrefix(Plugins::kCancelDownloadData),
                    [&plugins](CallbackContext& c) {
                        plugins.onCancelDownload(toButtonEvent(c));
                    });

    // The menu payloads documented in buttons.hpp.
    for (const char* prefix : {"help", "lang", "settings", "start", "close"}) {
        disp.onCallback(filters::callbackDataPrefix(prefix),
                        [&admin](CallbackContext& c) { admin.onMenu(toButtonEvent(c)); });
    }
}

}  // namespace anonx
