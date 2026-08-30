#ifndef ANONX_COOKIE_SOURCE_HPP
#define ANONX_COOKIE_SOURCE_HPP

#include <string>
#include <vector>

namespace anonx {

// Fetches cookie files from remote URLs to local directories.
// Called once at boot time.
class CookieSource {
public:
    virtual ~CookieSource() = default;

    // Fetches the specified URLs and writes valid cookies to the `cookies/`
    // directory. This function must handle failures gracefully.
    virtual void fetch(const std::vector<std::string>& urls) = 0;
};

// Real implementation that uses `curl` to fetch the cookies.
class CurlCookieSource : public CookieSource {
public:
    explicit CurlCookieSource(std::string curlBin = "curl");
    void fetch(const std::vector<std::string>& urls) override;

private:
    std::string curlBin_;
};

} // namespace anonx

#endif // ANONX_COOKIE_SOURCE_HPP
