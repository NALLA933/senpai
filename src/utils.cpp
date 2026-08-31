#include "anonx/utils.hpp"

#include <algorithm>
#include <cctype>

namespace anonx {
namespace utils {

std::vector<std::string> splitWs(const std::string& text) {
    std::vector<std::string> out;
    std::size_t b = 0;
    const std::size_t e = text.size();
    auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (b < e) {
        while (b < e && isws(static_cast<unsigned char>(text[b]))) ++b;
        if (b == e) break;
        std::size_t p = b;
        while (p < e && !isws(static_cast<unsigned char>(text[p]))) ++p;
        out.push_back(text.substr(b, p - b));
        b = p;
    }
    return out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
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
        if (value > (9223372036854775807LL - (text[i] - '0')) / 10)
            return false;
        value = value * 10 + (text[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;";  break;
            case '>': out += "&gt;";  break;
            default:  out.push_back(c); break;
        }
    }
    return out;
}

std::string shellQuote(const std::string& s) {
    std::string r = "'";
    for (const char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += "'";
    return r;
}

std::string strField(const nlohmann::json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return std::string();
}

std::int64_t intField(const nlohmann::json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) {
        return j[key].get<std::int64_t>();
    }
    return 0;
}

}  // namespace utils
}  // namespace anonx
