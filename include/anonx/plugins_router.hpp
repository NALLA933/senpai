// AnonXMusic C++ port — Phase 6a/6b (command plugins)
// plugins_router.hpp — Dispatcher wiring for every command.
//
// Kept separate from plugins.cpp on purpose: this is the only file in the
// command layer that knows about the Dispatcher (and therefore about
// TelegramClient and TDLib). Because of that split, plugins.cpp links into the
// offline test binary with no transport at all, while production simply compiles
// this file too and calls installPlugins() once at startup.
//
// Ports the decorator lines at the top of each Python plugin, e.g.
//     @app.on_message(filters.command(["play"]) & filters.group & ~app.bl_users)
//     @app.on_callback_query(filters.regex("controls"))

#ifndef ANONX_PLUGINS_ROUTER_HPP
#define ANONX_PLUGINS_ROUTER_HPP

#include "anonx/admin_plugins.hpp"
#include "anonx/database.hpp"
#include "anonx/dispatcher.hpp"
#include "anonx/plugins.hpp"

namespace anonx {

// Adapters between the dispatcher's contexts and the transport-free events the
// handlers take. The command name is lowercased here so the handlers can compare
// it directly ("seekback", a leading "v", a trailing "force").
CommandEvent toCommandEvent(const MessageContext& ctx);
ButtonEvent  toButtonEvent(const CallbackContext& ctx);

// Register every handler on `disp` and give `calls` its callbacks: the Phase 6a
// playback commands from `plugins`, the Phase 6b admin/menu commands and the
// chat watcher from `admin`. Both (and everything they borrow) must outlive the
// dispatcher.
void installPlugins(Dispatcher& disp, Plugins& plugins, AdminPlugins& admin,
                    Database& db);

}  // namespace anonx

#endif  // ANONX_PLUGINS_ROUTER_HPP
