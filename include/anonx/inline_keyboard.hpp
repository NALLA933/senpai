// AnonXMusic C++ port — Integration phase
// inline_keyboard.hpp — the inline-keyboard vocabulary types.
//
// These used to live in bot_api.hpp, which was fine while only the Phase 6a/6b
// command layer built keyboards. The integration phase adds a second producer —
// the Telegram layer, which has to SERIALIZE a keyboard into TDLib's
// replyMarkupInlineKeyboard — so the types moved into this leaf header rather
// than being duplicated (a duplicate struct is a silent drift waiting to
// happen: add a button kind on one side only and the other quietly drops it).
//
// Depends on nothing but the standard library, so both layers can include it
// without gaining a link dependency.

#ifndef ANONX_INLINE_KEYBOARD_HPP
#define ANONX_INLINE_KEYBOARD_HPP

#include <string>
#include <utility>
#include <vector>

namespace anonx {

// One inline-keyboard button. Exactly one action kind is used per button.
struct InlineButton {
    enum class Kind { Callback, Url, Copy };

    std::string text;
    Kind        kind = Kind::Callback;
    std::string data;   // callback payload (Kind::Callback) — matches Pyrogram callback_data
    std::string url;    // link target       (Kind::Url)
    std::string copy;   // text to copy       (Kind::Copy)

    static InlineButton callback(std::string t, std::string d) {
        InlineButton b; b.text = std::move(t); b.kind = Kind::Callback; b.data = std::move(d); return b;
    }
    static InlineButton link(std::string t, std::string u) {
        InlineButton b; b.text = std::move(t); b.kind = Kind::Url; b.url = std::move(u); return b;
    }
    static InlineButton copyText(std::string t, std::string c) {
        InlineButton b; b.text = std::move(t); b.kind = Kind::Copy; b.copy = std::move(c); return b;
    }
};

// Rows of buttons. Empty == no keyboard (like reply_markup=None).
using InlineKeyboard = std::vector<std::vector<InlineButton>>;

}  // namespace anonx

#endif  // ANONX_INLINE_KEYBOARD_HPP
