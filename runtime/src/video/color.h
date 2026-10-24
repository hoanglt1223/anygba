#pragma once

#include "anygba/video_options.h"
#include "video/image.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace anygba::video {

// BGR555 (red in bits 0-4) to 32-bit colour: raw, or the GBA LCD's darker, desaturated look,
// then the user gamma. With debanding, smooth 5-bit gradients are averaged at source resolution
// so their steps disappear; edges (steps of more than one level) are kept.
class ColorConverter {
public:
    explicit ColorConverter(const Options& options = {});

    std::uint32_t convert(std::uint16_t bgr555) const { return table_[bgr555 & 0x7FFFu]; }
    void convert(std::span<const std::uint16_t> frame, int width, int height, Image& out) const;

private:
    std::uint32_t convert_precise(float r, float g, float b) const;  // channels in 0..1 of the 5-bit range

    Color color_;
    double gamma_;
    bool deband_;
    std::vector<std::uint32_t> table_;
};

}  // namespace anygba::video
