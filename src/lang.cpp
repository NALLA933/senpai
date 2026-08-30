// AnonXMusic C++ port — Phase 6a (command plugins)
// lang.cpp — Language subsystem implementation.
//
// nlohmann::json is confined to this translation unit (project convention).
// The parsed language tables are stored as json objects behind a PIMPL and
// keys are resolved on demand with contains()/operator[]; nothing iterates a
// json object, so this compiles unchanged against the lightweight test stub.

#include "anonx/lang.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace anonx {

// ------------------------------------------------------------------
// formatStr — Python str.format() with positional args (rendered strings)
// ------------------------------------------------------------------
std::string formatStr(const std::string& tmpl, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(tmpl.size() + 16);
    std::size_t autoIndex = 0;

    for (std::size_t i = 0; i < tmpl.size();) {
        char c = tmpl[i];

        if (c == '{') {
            // Escaped "{{" -> "{"
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
                out.push_back('{');
                i += 2;
                continue;
            }
            // Find the closing '}'.
            std::size_t close = tmpl.find('}', i + 1);
            if (close == std::string::npos) {
                // Unterminated field — copy verbatim and stop scanning fields.
                out.append(tmpl, i, std::string::npos);
                break;
            }
            std::string field = tmpl.substr(i + 1, close - i - 1);   // e.g. "0" or "2:.1f"
            std::string indexPart = field;
            std::size_t colon = field.find(':');
            if (colon != std::string::npos)
                indexPart = field.substr(0, colon);   // drop the (ignored) spec

            // Trim surrounding whitespace from the index part.
            std::size_t a = indexPart.find_first_not_of(" \t");
            std::size_t b = indexPart.find_last_not_of(" \t");
            if (a == std::string::npos) indexPart.clear();
            else indexPart = indexPart.substr(a, b - a + 1);

            std::size_t idx = 0;
            bool haveIdx = false;
            if (indexPart.empty()) {
                idx = autoIndex++;               // "{}" auto-numbering
                haveIdx = true;
            } else {
                bool allDigits = true;
                for (char d : indexPart)
                    if (!std::isdigit(static_cast<unsigned char>(d))) { allDigits = false; break; }
                if (allDigits) {
                    idx = static_cast<std::size_t>(std::stoul(indexPart));
                    haveIdx = true;
                }
            }

            if (haveIdx && idx < args.size()) {
                out += args[idx];
            } else {
                // Unknown / out-of-range: keep the placeholder so it is visible.
                out.push_back('{');
                out += field;
                out.push_back('}');
            }
            i = close + 1;
            continue;
        }

        if (c == '}') {
            // Escaped "}}" -> "}"
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
                out.push_back('}');
                i += 2;
                continue;
            }
            out.push_back('}');   // stray '}' — copy verbatim
            ++i;
            continue;
        }

        out.push_back(c);
        ++i;
    }
    return out;
}

// ------------------------------------------------------------------
// Language::Impl — parsed tables (json confined here)
// ------------------------------------------------------------------
struct Language::Impl {
    std::unordered_map<std::string, nlohmann::json> langs_;

    // On-demand key lookup. Returns true + value if `code` is loaded and holds
    // `key`. No iteration; safe against the test stub.
    bool lookup(const std::string& code, const std::string& key, std::string& out) {
        auto it = langs_.find(code);
        if (it == langs_.end())
            return false;
        nlohmann::json& j = it->second;          // non-const: stub operator[] ok
        if (!j.contains(key))
            return false;
        out = j[key].get<std::string>();
        return true;
    }
};

Language::Language() : impl_(new Impl) {}
Language::~Language() = default;

bool Language::loadJsonText(const std::string& code, const std::string& jsonText) {
    try {
        nlohmann::json j = nlohmann::json::parse(jsonText);
        if (!j.is_object())
            return false;
        impl_->langs_[code] = std::move(j);
        return true;
    } catch (...) {
        return false;   // malformed JSON — skip this language
    }
}

bool Language::loadFile(const std::string& code, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadJsonText(code, ss.str());
}

int Language::loadDir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d)
        return 0;
    const std::string ext = ".json";
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() > ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            std::string code = name.substr(0, name.size() - ext.size());
            std::string path = dir;
            if (!path.empty() && path.back() != '/')
                path.push_back('/');
            path += name;
            if (loadFile(code, path))
                ++n;
        }
    }
    closedir(d);
    return n;
}

bool Language::loaded(const std::string& code) const {
    return impl_->langs_.find(code) != impl_->langs_.end();
}

std::vector<std::string> Language::codes() const {
    std::vector<std::string> out;
    out.reserve(impl_->langs_.size());
    for (const auto& kv : impl_->langs_)
        out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

std::string Language::tr(const std::string& code, const std::string& key) const {
    std::string value;
    if (impl_->lookup(code, key, value))
        return value;
    if (code != defaultCode_ && impl_->lookup(defaultCode_, key, value))
        return value;
    return "{" + key + "}";   // last resort: visible missing-key marker
}

LangView Language::view(const std::string& code) const {
    // Bind to the requested code if loaded, otherwise to the default.
    if (loaded(code))
        return LangView(this, code);
    return LangView(this, defaultCode_);
}

std::string Language::nameOf(const std::string& code) {
    for (const auto& kv : allCodes())
        if (kv.first == code)
            return kv.second;
    return "";
}

const std::vector<std::pair<std::string, std::string>>& Language::allCodes() {
    // Same set/order as anony/core/lang.py `lang_codes` (UTF-8 names).
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"ar", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9"},          // العربية
        {"de", "Deutsch"},
        {"en", "English"},
        {"es", "Espa\xC3\xB1ol"},                                                   // Español
        {"fr", "Fran\xC3\xA7" "ais"},                                               // Français
        {"hi", "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80"},  // हिन्दी
        {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"},                             // 日本語
        {"my", "\xE1\x80\x99\xE1\x80\xBC\xE1\x80\x94\xE1\x80\xBA\xE1\x80\x99\xE1\x80\xAC\xE1\x80\x98\xE1\x80\xAC\xE1\x80\x9E\xE1\x80\xAC"},  // မြန်မာဘာသာ
        {"pa", "\xE0\xA8\xAA\xE0\xA9\xB0\xE0\xA8\x9C\xE0\xA8\xBE\xE0\xA8\xAC\xE0\xA9\x80"},   // ਪੰਜਾਬੀ
        {"pt", "Portugu\xC3\xAAs"},                                                 // Português
        {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"},         // Русский
        {"tr", "T\xC3\xBCrk\xC3\xA7""e"},                                           // Türkçe
        {"zh", "\xE4\xB8\xAD\xE6\x96\x87"},                                         // 中文
    };
    return table;
}

std::string LangView::operator[](const std::string& key) const {
    if (!lang_)
        return "{" + key + "}";
    return lang_->tr(code_, key);
}

}  // namespace anonx
