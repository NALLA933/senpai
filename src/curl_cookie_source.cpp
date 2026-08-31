#include "anonx/cookie_source.hpp"
#include "anonx/logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "anonx/utils.hpp"

namespace anonx {


CurlCookieSource::CurlCookieSource(std::string curlBin) : curlBin_(std::move(curlBin)) {}

void CurlCookieSource::fetch(const std::vector<std::string>& urls) {
    if (urls.empty()) return;

    // Ensure the cookies directory exists
    ::mkdir("cookies", 0700);

    for (std::size_t i = 0; i < urls.size(); ++i) {
        const std::string& url = urls[i];
        const std::string tmp_path = "cookies/tmp_" + std::to_string(i) + ".txt";
        const std::string final_path = "cookies/fetched_" + std::to_string(i) + ".txt";

        std::remove(tmp_path.c_str());

        // Curl command: silent, fail on HTTP error, follow redirects, limit size to 1MB, output to temp
        std::string cmd = curlBin_ + " -s -f -L --max-filesize 1000000 -o " + tmp_path + " " + anonx::utils::shellQuote(url);
        int ret = std::system(cmd.c_str());
        
        if (ret != 0) {
            Logger("anonx.cookies").warning("Failed to fetch cookie from URL index " + std::to_string(i));
            std::remove(tmp_path.c_str());
            continue;
        }

        struct stat st;
        if (::stat(tmp_path.c_str(), &st) != 0 || st.st_size == 0 || st.st_size > 1000000) {
            Logger("anonx.cookies").warning("Invalid cookie file size from URL index " + std::to_string(i));
            std::remove(tmp_path.c_str());
            continue;
        }

        std::ifstream f(tmp_path);
        std::string line;
        if (!std::getline(f, line) || (line.find("# Netscape HTTP Cookie File") == std::string::npos && line.find("# HTTP Cookie File") == std::string::npos)) {
            Logger("anonx.cookies").warning("Invalid cookie file format from URL index " + std::to_string(i));
            std::remove(tmp_path.c_str());
            continue;
        }
        f.close();

        // Set mode 600
        ::chmod(tmp_path.c_str(), 0600);

        // Rename to final
        if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
            Logger("anonx.cookies").warning("Failed to save cookie file from URL index " + std::to_string(i));
            std::remove(tmp_path.c_str());
        } else {
            Logger("anonx.cookies").info("Successfully rotated cookie from URL index " + std::to_string(i));
        }
    }
}

} // namespace anonx
