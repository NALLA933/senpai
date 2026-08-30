// AnonXMusic C++ port — Phase 7
#ifndef ANONX_THUMBNAIL_RENDERER_HPP
#define ANONX_THUMBNAIL_RENDERER_HPP

#include <string>

namespace anonx {

// Abstract interface for thumbnail generation.
class ThumbnailRenderer {
public:
    virtual ~ThumbnailRenderer() = default;

    // Renders a thumbnail using the given details. Returns the path to the
    // generated image, or an empty string on failure.
    virtual std::string render(const std::string& coverUrl,
                               const std::string& title,
                               const std::string& duration,
                               const std::string& channel,
                               const std::string& requester) = 0;

    // Fetch an image (e.g. for /start or /ping) without rendering text.
    // Returns the path to the downloaded image, or empty string on failure.
    virtual std::string fetch(const std::string& url) = 0;
};

}  // namespace anonx

#endif  // ANONX_THUMBNAIL_RENDERER_HPP
