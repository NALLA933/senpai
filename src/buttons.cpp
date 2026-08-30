// AnonXMusic C++ port — Phase 6a (command plugins)
// buttons.cpp — inline-keyboard builders (see buttons.hpp).
//
// Layouts and callback_data strings mirror anony/helpers/_inline.py exactly.
// The transport glyphs are written as explicit UTF-8 byte escapes so the exact
// bytes are guaranteed regardless of source-file encoding:
//   resume "\xE2\x96\xB7" = U+25B7  ▷
//   pause  "II"           = ASCII
//   replay "\xE2\xA5\x81" = U+2941  ⥁
//   skip   "\xE2\x80\xA3\xE2\x80\xA3I" = U+2023 U+2023 'I'  ‣‣I
//   stop   "\xE2\x96\xA2" = U+25A2  ▢

#include "anonx/buttons.hpp"

namespace anonx {
namespace buttons {

InlineKeyboard controls(std::int64_t chatId, const std::string& status,
                        const std::string& timer, bool remove) {
    InlineKeyboard kb;
    const std::string cid = std::to_string(chatId);

    if (!status.empty()) {
        kb.push_back({ InlineButton::callback(status, "controls status " + cid) });
    } else if (!timer.empty()) {
        kb.push_back({ InlineButton::callback(timer, "controls status " + cid) });
    }

    if (!remove) {
        kb.push_back({
            InlineButton::callback("\xE2\x96\xB7",             "controls resume " + cid),
            InlineButton::callback("II",                       "controls pause "  + cid),
            InlineButton::callback("\xE2\xA5\x81",             "controls replay " + cid),
            InlineButton::callback("\xE2\x80\xA3\xE2\x80\xA3I", "controls skip "   + cid),
            InlineButton::callback("\xE2\x96\xA2",             "controls stop "   + cid),
        });
    }
    return kb;
}

InlineKeyboard playQueued(std::int64_t chatId, const std::string& itemId,
                          const std::string& text) {
    const std::string cid = std::to_string(chatId);
    return { { InlineButton::callback(text, "controls force " + cid + " " + itemId) } };
}

InlineKeyboard queueMarkup(std::int64_t chatId, const std::string& text, bool playing) {
    const std::string action = playing ? "pause" : "resume";
    const std::string cid = std::to_string(chatId);
    return { { InlineButton::callback(text, "controls " + action + " " + cid + " q") } };
}

InlineKeyboard cancelDl(const std::string& text) {
    return { { InlineButton::callback(text, "cancel_dl") } };
}

InlineKeyboard pingMarkup(const std::string& text, const std::string& url) {
    return { { InlineButton::link(text, url) } };
}

// ---------------------------------------------------------------------
// Phase 6b — menus
// ---------------------------------------------------------------------

const char* toggleMark(bool enabled) {
    // U+2705 white heavy check mark / U+274C cross mark, as explicit UTF-8.
    return enabled ? "\xE2\x9C\x85" : "\xE2\x9D\x8C";
}

namespace {

// The row of external links every start card carries. Buttons whose target is
// empty are skipped, so a partly configured bot still renders a valid keyboard.
std::vector<InlineButton> linkRow(const MenuText& t, const std::string& supportChat,
                                  const std::string& supportChannel) {
    std::vector<InlineButton> row;
    if (!supportChat.empty())
        row.push_back(InlineButton::link(t.support, supportChat));
    if (!supportChannel.empty())
        row.push_back(InlineButton::link(t.channel, supportChannel));
    return row;
}

void pushIfAny(InlineKeyboard& kb, std::vector<InlineButton> row) {
    if (!row.empty())
        kb.push_back(std::move(row));
}

}  // namespace

InlineKeyboard startPrivate(const MenuText& t, const std::string& addMeUrl,
                            const std::string& supportChat,
                            const std::string& supportChannel,
                            const std::string& sourceUrl) {
    InlineKeyboard kb;
    kb.push_back({ InlineButton::callback(t.help, "help menu") });
    if (!addMeUrl.empty())
        kb.push_back({ InlineButton::link(t.addMe, addMeUrl) });
    pushIfAny(kb, linkRow(t, supportChat, supportChannel));
    if (!sourceUrl.empty())
        kb.push_back({ InlineButton::link(t.source, sourceUrl) });
    return kb;
}

InlineKeyboard startGroup(const MenuText& t, const std::string& addMeUrl,
                          const std::string& supportChat,
                          const std::string& supportChannel) {
    InlineKeyboard kb;
    kb.push_back({ InlineButton::callback(t.help, "help menu") });
    if (!addMeUrl.empty())
        kb.push_back({ InlineButton::link(t.addMe, addMeUrl) });
    pushIfAny(kb, linkRow(t, supportChat, supportChannel));
    return kb;
}

InlineKeyboard helpMenu(const std::vector<std::string>& topics, const MenuText& t) {
    InlineKeyboard kb;
    std::vector<InlineButton> row;
    for (std::size_t i = 0; i < topics.size(); ++i) {
        row.push_back(InlineButton::callback(topics[i], "help " + std::to_string(i)));
        if (row.size() == 3) {
            kb.push_back(std::move(row));
            row.clear();
        }
    }
    pushIfAny(kb, std::move(row));
    kb.push_back({ InlineButton::callback(t.close, "close") });
    return kb;
}

InlineKeyboard backClose(const MenuText& t, const std::string& backData) {
    return { { InlineButton::callback(t.back,  backData),
               InlineButton::callback(t.close, "close") } };
}

InlineKeyboard helpTopic(const MenuText& t) {
    return backClose(t, "help menu");
}

InlineKeyboard langMenu(const std::vector<std::pair<std::string, std::string>>& langs,
                        const MenuText& t) {
    InlineKeyboard kb;
    std::vector<InlineButton> row;
    for (const auto& l : langs) {
        row.push_back(InlineButton::callback(l.second, "lang set " + l.first));
        if (row.size() == 3) {
            kb.push_back(std::move(row));
            row.clear();
        }
    }
    pushIfAny(kb, std::move(row));
    kb.push_back({ InlineButton::callback(t.back,  "settings menu"),
                   InlineButton::callback(t.close, "close") });
    return kb;
}

InlineKeyboard settingsMenu(const MenuText& t, bool cmdDelete, bool playMode) {
    return {
        { InlineButton::callback(t.cmdDelete + " " + toggleMark(cmdDelete),
                                 "settings toggle cmd_delete") },
        { InlineButton::callback(t.playMode + " " + toggleMark(playMode),
                                 "settings toggle play_mode") },
        { InlineButton::callback(t.language, "lang menu") },
        { InlineButton::callback(t.close,    "close") },
    };
}

}  // namespace buttons
}  // namespace anonx
