// AnonXMusic C++ port — Phase 6a (command plugins)
// buttons.hpp — inline-keyboard builders.
//
// A port of the relevant parts of anony/helpers/_inline.py (the `Inline` class,
// exposed there as the singleton `buttons`). Each function returns an
// InlineKeyboard (see bot_api.hpp) with the SAME layout and — crucially — the
// SAME callback_data strings as the Python bot, so the callback router in
// plugins.cpp parses them identically.
//
// Phase 6a ports only the builders the playback commands + the "controls"
// callback need: controls, playQueued, queueMarkup, cancelDl, pingMarkup.
// Phase 6b adds the menu builders (start / help / language / settings).
//
// CALLBACK PAYLOADS. The playback payloads ("controls …") match `_inline.py`
// byte for byte. The Phase 6b menu payloads follow the same scheme —
// "<namespace> <action> [arg]" — and are defined here, since both the producer
// (these builders) and the consumer (AdminPlugins::onMenu) live in this port:
//   "help menu" | "help <0..8>"      the help grid and one topic page
//   "lang menu" | "lang set <code>"  the language grid and a pick
//   "settings menu" | "settings toggle <cmd_delete|play_mode>"
//   "start menu"                     back to the start card
//   "close"                          delete the menu message

#ifndef ANONX_BUTTONS_HPP
#define ANONX_BUTTONS_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "anonx/bot_api.hpp"

namespace anonx {
namespace buttons {

// The player control card. Ports Inline.controls.
//   * if `status` is non-empty  -> a first row with a single status button
//     (callback "controls status <chat_id>"); else if `timer` is non-empty the
//     same, using the timer text.
//   * unless `remove` is set     -> a row of five transport buttons
//     [▷ resume, II pause, ⥁ replay, ‣‣I skip, ▢ stop].
InlineKeyboard controls(std::int64_t chatId,
                        const std::string& status = "",
                        const std::string& timer = "",
                        bool remove = false);

// The "added to queue" card's single button. Ports Inline.play_queued.
// callback: "controls force <chat_id> <item_id>".
InlineKeyboard playQueued(std::int64_t chatId, const std::string& itemId,
                          const std::string& text);

// The queue card's single pause/resume button. Ports Inline.queue_markup.
// callback: "controls <pause|resume> <chat_id> q"  (the trailing "q" marks the
// queue-card variant, which edits the markup in place instead of the card text).
InlineKeyboard queueMarkup(std::int64_t chatId, const std::string& text, bool playing);

// A lone "cancel download" button. Ports Inline.cancel_dl. callback: "cancel_dl".
InlineKeyboard cancelDl(const std::string& text);

// A lone URL button (used by the ping card). Ports Inline.ping_markup.
InlineKeyboard pingMarkup(const std::string& text, const std::string& url);

// ---------------------------------------------------------------------
// Phase 6b — menus. Every label is passed in already translated, keeping
// these builders pure (no Language dependency), exactly like the ones above.
// ---------------------------------------------------------------------

// Labels shared by the menus, so a caller fills them once from its LangView.
struct MenuText {
    std::string help;        // "Help"
    std::string addMe;       // "Add me to your group"
    std::string support;     // "Support"
    std::string channel;     // "Channel"
    std::string source;      // "Source"
    std::string language;    // "Language"
    std::string cmdDelete;   // "Command delete"
    std::string playMode;    // "Admin only play"
    std::string back;        // "Back"
    std::string close;       // "Close"
};

// The /start card in a private chat: help + "add me", then the link row.
// `addMeUrl` is empty when the bot's username is unknown (button omitted).
InlineKeyboard startPrivate(const MenuText& t, const std::string& addMeUrl,
                            const std::string& supportChat,
                            const std::string& supportChannel,
                            const std::string& sourceUrl);

// The /start card in a group: settings + the link row.
InlineKeyboard startGroup(const MenuText& t, const std::string& addMeUrl,
                          const std::string& supportChat,
                          const std::string& supportChannel);

// The help grid: `topics` (the help_0…help_8 labels) three per row, then Close.
InlineKeyboard helpMenu(const std::vector<std::string>& topics,
                        const MenuText& t);

// One help topic page: Back to the grid, and Close.
InlineKeyboard helpTopic(const MenuText& t);

// A generic "Back + Close" row, where Back carries `backData` (e.g.
// "settings menu"). helpTopic is this with backData = "help menu".
InlineKeyboard backClose(const MenuText& t, const std::string& backData);

// The language grid: {code, name} pairs three per row, then Back + Close.
InlineKeyboard langMenu(const std::vector<std::pair<std::string, std::string>>& langs,
                        const MenuText& t);

// The per-chat settings card: two toggles showing their current state, the
// language shortcut, and Close.
InlineKeyboard settingsMenu(const MenuText& t, bool cmdDelete, bool playMode);

// The on/off marker appended to a settings toggle's label ("✅" / "❌").
const char* toggleMark(bool enabled);

}  // namespace buttons
}  // namespace anonx

#endif  // ANONX_BUTTONS_HPP
