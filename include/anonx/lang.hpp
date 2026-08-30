// AnonXMusic C++ port — Phase 6a (command plugins)
// lang.hpp — multilingual string subsystem.
//
// A faithful port of anony/core/lang.py. The Python bot keeps one flat
// string->string dictionary per language (the files in anony/locales/*.json,
// 153 keys each) and a `language()` decorator that resolves a chat's language
// and injects the dict as `message.lang`. Plugins then do
//     m.lang["play_searching"]                       # raw string
//     m.lang["play_queued"].format(pos, url, title)  # positional .format()
//
// The C++ analogue is:
//     LangView L = languages.view(db.getLang(chatId));
//     L["play_searching"];
//     L.fmt("play_queued", pos, url, title, duration, mention);
//
// Design note — nlohmann::json is deliberately NOT included here. Following the
// project convention (JSON only ever appears inside .cpp files), the parsed
// language tables live behind a PIMPL in lang.cpp and keys are looked up on
// demand. Nothing here iterates a JSON object, so the header stays dependency
// free and the whole subsystem compiles against either the real nlohmann/json
// or the lightweight test stub.

#ifndef ANONX_LANG_HPP
#define ANONX_LANG_HPP

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace anonx {

// Render a Python-style positional format template. Supports:
//   * "{0}", "{1}", …            — positional substitution from `args`
//   * "{0:.1f}", "{2:>5}", …     — a format spec after ':' is accepted and
//                                   ignored (args are already rendered strings)
//   * "{}"                       — auto-numbered (0, 1, 2, … in order seen)
//   * "{{" and "}}"              — literal '{' and '}'
// An index with no matching arg is left as the original "{…}" placeholder so
// the bug is visible rather than silently dropped.
std::string formatStr(const std::string& tmpl, const std::vector<std::string>& args);

// ---- toArg(): convert a supported format argument to a string ----
inline std::string toArg(const std::string& s) { return s; }
inline std::string toArg(const char* s) { return s ? std::string(s) : std::string(); }
inline std::string toArg(bool b) { return b ? "true" : "false"; }
template <typename T,
          typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
inline std::string toArg(T v) { return std::to_string(v); }

class Language;

// A cheap handle bound to one loaded language. Copyable; borrows the Language
// (which must outlive every view — in practice it lives for the whole process).
class LangView {
public:
    LangView() = default;
    LangView(const Language* lang, std::string code)
        : lang_(lang), code_(std::move(code)) {}

    // Raw string for a key, with the Language's fallback chain applied. Never
    // throws; an unknown key yields "{key}" so it is obvious in output.
    std::string operator[](const std::string& key) const;
    std::string get(const std::string& key) const { return (*this)[key]; }

    // Formatted lookup: fmt("play_skipped", mention) etc. Arguments may be
    // strings, C-strings, bools, or any integral type (see toArg overloads).
    template <typename... Args>
    std::string fmt(const std::string& key, Args&&... args) const {
        return formatStr((*this)[key],
                         std::vector<std::string>{ toArg(std::forward<Args>(args))... });
    }

    const std::string& code() const { return code_; }
    bool valid() const { return lang_ != nullptr; }

private:
    const Language* lang_ = nullptr;
    std::string code_;
};

class Language {
public:
    Language();
    ~Language();

    // Non-copyable (owns the parsed tables via a unique_ptr).
    Language(const Language&)            = delete;
    Language& operator=(const Language&) = delete;

    // Load one language ("en", "hi", …) from a JSON file. Returns false if the
    // file cannot be read or parsed.
    bool loadFile(const std::string& code, const std::string& path);

    // Load one language directly from a JSON string. Used by tests (no disk).
    bool loadJsonText(const std::string& code, const std::string& jsonText);

    // Load every "<code>.json" in `dir` (POSIX directory scan). Returns the
    // number of languages successfully loaded.
    int loadDir(const std::string& dir);

    bool loaded(const std::string& code) const;

    // Sorted list of loaded language codes.
    std::vector<std::string> codes() const;

    // Fallback language used when a chat's code is missing/unloaded (def "en").
    void setDefault(const std::string& code) { defaultCode_ = code; }
    const std::string& defaultCode() const { return defaultCode_; }

    // Resolve one key. Fallback chain: `code` -> defaultCode() -> "{key}".
    std::string tr(const std::string& code, const std::string& key) const;

    // A view bound to `code`. If `code` is not loaded the view still works and
    // resolves through the default language.
    LangView view(const std::string& code) const;

    // Display name for a code, e.g. "en" -> "English"; "" if the code is not in
    // the built-in table. Mirrors anony/core/lang.py `lang_codes`.
    static std::string nameOf(const std::string& code);

    // The full built-in {code, name} table, in the original Python order.
    static const std::vector<std::pair<std::string, std::string>>& allCodes();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string defaultCode_ = "en";
};

}  // namespace anonx

#endif  // ANONX_LANG_HPP
