#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace anygba::control {

// An RGB555 picture (red in bits 0-4) as an 8-bit RGB PNG, resized by nearest neighbour
// to width * scale x height * scale (scale 0.25-8). The zlib stream uses stored blocks:
// no compression, but no dependency either.
std::vector<std::uint8_t> encode_png(std::span<const std::uint16_t> rgb555, int width, int height, double scale = 1.0);

}  // namespace anygba::control
