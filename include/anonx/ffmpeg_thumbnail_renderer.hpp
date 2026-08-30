// AnonXMusic C++ port — Phase 7
#ifndef ANONX_FFMPEG_THUMBNAIL_RENDERER_HPP
#define ANONX_FFMPEG_THUMBNAIL_RENDERER_HPP

#include "anonx/thumbnail_renderer.hpp"
#include <atomic>

namespace anonx {

class FfmpegThumbnailRenderer : public ThumbnailRenderer {
public:
    FfmpegThumbnailRenderer();
    ~FfmpegThumbnailRenderer() override = default;

    std::string render(const std::string& coverUrl,
                       const std::string& title,
                       const std::string& duration,
                       const std::string& channel,
                       const std::string& requester) override;

    std::string fetch(const std::string& url) override;

private:
    std::atomic<int> counter_{0};
};

}  // namespace anonx

#endif  // ANONX_FFMPEG_THUMBNAIL_RENDERER_HPP
