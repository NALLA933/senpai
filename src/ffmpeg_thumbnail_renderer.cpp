// AnonXMusic C++ port — Phase 7
#include "anonx/ffmpeg_thumbnail_renderer.hpp"

#include <cstdlib>
#include <cstdio>
#include "anonx/utils.hpp"

namespace anonx {
namespace {

std::string escapeDrawtext(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\'' || c == '\\' || c == ':' || c == '%') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace

FfmpegThumbnailRenderer::FfmpegThumbnailRenderer() {}

std::string FfmpegThumbnailRenderer::render(const std::string& coverUrl,
                                            const std::string& title,
                                            const std::string& duration,
                                            const std::string& channel,
                                            const std::string& requester) {
    if (coverUrl.empty()) return "";

    // Bound the cache size to 100 items by cycling through IDs.
    const int id = (++counter_) % 100;
    const std::string inPath = "cache/thumb_in_" + std::to_string(id) + ".jpg";
    const std::string outPath = "cache/thumb_out_" + std::to_string(id) + ".jpg";

    // 1. Fetch cover art using curl
    // ponytail: using curl directly via system() as it's the simplest working seam.
    std::string curlCmd = "curl -s -L -o " + anonx::utils::shellQuote(inPath) + " " + anonx::utils::shellQuote(coverUrl);
    if (std::system(curlCmd.c_str()) != 0) {
        return "";
    }

    // 2. Render with ffmpeg
    // ponytail: a very basic ffmpeg drawtext filter is used. Production would likely use a custom font or imagemagick.
    std::string text = escapeDrawtext(title + " | " + duration + " | " + channel + " | " + requester);
    std::string filter = "drawtext=text='" + text + "':x=10:y=10:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.5";
    
    std::string ffmpegCmd = "ffmpeg -y -i " + anonx::utils::shellQuote(inPath) + " -vf " + anonx::utils::shellQuote(filter) + " -frames:v 1 " + anonx::utils::shellQuote(outPath) + " >/dev/null 2>&1";
    
    if (std::system(ffmpegCmd.c_str()) != 0) {
        std::remove(inPath.c_str());
        return "";
    }

    std::remove(inPath.c_str());
    return outPath;
}

std::string FfmpegThumbnailRenderer::fetch(const std::string& url) {
    if (url.empty()) return "";

    const int id = (++counter_) % 100;
    const std::string outPath = "cache/thumb_fetch_" + std::to_string(id) + ".jpg";

    std::string curlCmd = "curl -s -L -o " + anonx::utils::shellQuote(outPath) + " " + anonx::utils::shellQuote(url);
    if (std::system(curlCmd.c_str()) != 0) {
        return "";
    }
    return outPath;
}

} // namespace anonx
