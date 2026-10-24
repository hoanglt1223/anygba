#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// 32-bit pictures of the video chain. A pixel is R, G, B, A bytes in memory order
// (little-endian value 0xAABBGGRR), alpha always 0xFF.
namespace anygba::video {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;

    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    }
    std::uint32_t* row(int y) { return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width); }
    const std::uint32_t* row(int y) const {
        return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
    }
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect&) const = default;
};

inline constexpr std::uint32_t kOpaque = 0xFF000000u;

constexpr std::uint32_t pack(unsigned r, unsigned g, unsigned b) { return r | (g << 8) | (b << 16) | kOpaque; }
constexpr unsigned red(std::uint32_t p) { return p & 0xFFu; }
constexpr unsigned green(std::uint32_t p) { return (p >> 8) & 0xFFu; }
constexpr unsigned blue(std::uint32_t p) { return (p >> 16) & 0xFFu; }

// (a * (256 - w) + b * w) / 256 per channel, w in 0..256. Red and blue are computed together
// in one 32-bit word: each 16-bit lane holds at most 255 * 256 + 128, so nothing carries over.
constexpr std::uint32_t blend(std::uint32_t a, std::uint32_t b, unsigned w) {
    const unsigned v = 256 - w;
    const std::uint32_t rb = (((a & 0xFF00FFu) * v + (b & 0xFF00FFu) * w + 0x800080u) >> 8) & 0xFF00FFu;
    const std::uint32_t g = (((a & 0x00FF00u) * v + (b & 0x00FF00u) * w + 0x008000u) >> 8) & 0x00FF00u;
    return rb | g | kOpaque;
}

constexpr std::uint32_t mix2(std::uint32_t a, std::uint32_t b) { return blend(a, b, 128); }

constexpr std::uint32_t mix4(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    return pack((red(a) + red(b) + red(c) + red(d) + 2) >> 2, (green(a) + green(b) + green(c) + green(d) + 2) >> 2,
                (blue(a) + blue(b) + blue(c) + blue(d) + 2) >> 2);
}

// Runs fn(begin, end) over [0, count) split across a process-wide worker pool; returns when
// all parts are done. Small counts run on the calling thread.
void parallel_for(int count, const std::function<void(int, int)>& fn);

}  // namespace anygba::video
