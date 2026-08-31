#ifndef ANONX_UTILS_HPP
#define ANONX_UTILS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace anonx {
namespace utils {

// Split a string by whitespace into a vector of strings, ignoring empty tokens.
std::vector<std::string> splitWs(const std::string& text);

// Convert a string to lowercase.
std::string toLower(std::string s);

// Parse a 64-bit signed integer safely, with overflow checks.
// Returns false on empty/garbage/overflow so callers can show usage string.
bool parseI64(const std::string& text, std::int64_t& out);

// Escape a string for HTML/Telegram parsing (<, >, &).
std::string htmlEscape(const std::string& s);

// Escape a string safely for shell execution by wrapping it in single quotes.
std::string shellQuote(const std::string& s);

// Safe string-field read from a JSON object: returns "" unless the key holds a string.
std::string strField(const nlohmann::json& j, const char* key);

// Safe int64-field read from a JSON object: returns 0 unless the key holds a number.
std::int64_t intField(const nlohmann::json& j, const char* key);

}  // namespace utils
}  // namespace anonx

#endif  // ANONX_UTILS_HPP
