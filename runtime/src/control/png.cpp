#include "control/png.h"

#include "anygba/crc32.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace anygba::control {

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}

void chunk(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(std::span(out.data() + start, out.size() - start)));
}

std::uint8_t expand5(std::uint16_t v) {
    const auto c = static_cast<std::uint8_t>(v & 31);
    return static_cast<std::uint8_t>(c << 3 | c >> 2);
}

}  // namespace

std::vector<std::uint8_t> encode_png(std::span<const std::uint16_t> rgb555, int width, int height, double scale) {
    if (width <= 0 || height <= 0 || rgb555.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        throw std::invalid_argument("encode_png: picture smaller than its size");
    }
    if (!(scale >= 0.25 && scale <= 8)) {
        throw std::invalid_argument("scale must be between 0.25 and 8");
    }
    const int out_w = std::max(1, static_cast<int>(std::lround(width * scale)));
    const int out_h = std::max(1, static_cast<int>(std::lround(height * scale)));

    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(out_h) * (static_cast<std::size_t>(out_w) * 3 + 1));
    for (int y = 0; y < out_h; ++y) {
        raw.push_back(0);  // filter: none
        const int sy = y * height / out_h;
        for (int x = 0; x < out_w; ++x) {
            const int sx = x * width / out_w;
            const std::uint16_t p = rgb555[static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(sx)];
            raw.push_back(expand5(p));
            raw.push_back(expand5(static_cast<std::uint16_t>(p >> 5)));
            raw.push_back(expand5(static_cast<std::uint16_t>(p >> 10)));
        }
    }

    std::vector<std::uint8_t> z = {0x78, 0x01};
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last) break;
    }
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, static_cast<std::uint32_t>(out_w));
    put32(ihdr, static_cast<std::uint32_t>(out_h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

}  // namespace anygba::control
