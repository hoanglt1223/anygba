#include "video/color.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace anygba::video {

namespace {

unsigned to_byte(double v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

}  // namespace

ColorConverter::ColorConverter(const Options& options)
    : color_(options.color), gamma_(options.gamma), deband_(options.deband), table_(32768) {
    const bool identity = color_ == Color::Raw && gamma_ == 1.0;
    for (unsigned v = 0; v < 32768; ++v) {
        const unsigned r = v & 31u, g = (v >> 5) & 31u, b = (v >> 10) & 31u;
        if (identity) {
            // Bit replication, the exact 5-to-8-bit mapping the plain window path uses.
            table_[v] = pack((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
        } else {
            table_[v] = convert_precise(static_cast<float>(r) / 31.0f, static_cast<float>(g) / 31.0f,
                                        static_cast<float>(b) / 31.0f);
        }
    }
}

std::uint32_t ColorConverter::convert_precise(float r, float g, float b) const {
    double out[3] = {r, g, b};
    if (color_ == Color::GbaLcd) {
        // The GBA panel's response is close to gamma 4 and its primaries bleed into each other;
        // mix in linear light, then encode for a gamma-2.2 display at 255/280 brightness.
        const double lr = std::pow(r, 4.0), lg = std::pow(g, 4.0), lb = std::pow(b, 4.0);
        const double mixed[3] = {(255.0 * lr + 50.0 * lg + 0.0 * lb) / 255.0,
                                 (10.0 * lr + 230.0 * lg + 30.0 * lb) / 255.0,
                                 (50.0 * lr + 10.0 * lg + 220.0 * lb) / 255.0};
        for (int c = 0; c < 3; ++c) out[c] = std::pow(std::min(mixed[c], 1.0), 1.0 / 2.2) * (255.0 / 280.0);
    }
    if (gamma_ != 1.0) {
        for (double& c : out) c = std::pow(c, 1.0 / gamma_);
    }
    return pack(to_byte(out[0]), to_byte(out[1]), to_byte(out[2]));
}

void ColorConverter::convert(const Image& in, Image& out) {
    out.resize(in.width, in.height);
    if (color_ == Color::Raw && gamma_ == 1.0) {
        for (std::size_t i = 0; i < in.pixels.size(); ++i) out.pixels[i] = in.pixels[i] | kOpaque;
        return;
    }
    constexpr int kGrid = 33;
    if (cube_.empty()) {
        cube_.resize(kGrid * kGrid * kGrid);
        for (int b = 0; b < kGrid; ++b) {
            for (int g = 0; g < kGrid; ++g) {
                for (int r = 0; r < kGrid; ++r) {
                    const std::uint32_t c = convert_precise(static_cast<float>(r) / (kGrid - 1), static_cast<float>(g) / (kGrid - 1),
                                                            static_cast<float>(b) / (kGrid - 1));
                    cube_[static_cast<std::size_t>((b * kGrid + g) * kGrid + r)] = {
                        static_cast<float>(red(c)), static_cast<float>(green(c)), static_cast<float>(blue(c))};
                }
            }
        }
    }
    parallel_for(in.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const std::uint32_t* src = in.row(y);
            std::uint32_t* dst = out.row(y);
            for (int x = 0; x < in.width; ++x) {
                const std::uint32_t c = src[x];
                const float p[3] = {red(c) * (kGrid - 1) / 255.0f, green(c) * (kGrid - 1) / 255.0f, blue(c) * (kGrid - 1) / 255.0f};
                int i[3];
                float f[3];
                for (int k = 0; k < 3; ++k) {
                    i[k] = std::min(static_cast<int>(p[k]), kGrid - 2);
                    f[k] = p[k] - static_cast<float>(i[k]);
                }
                float acc[3] = {0, 0, 0};
                for (int corner = 0; corner < 8; ++corner) {
                    const int dr = corner & 1, dg = (corner >> 1) & 1, db = (corner >> 2) & 1;
                    const float w = (dr ? f[0] : 1 - f[0]) * (dg ? f[1] : 1 - f[1]) * (db ? f[2] : 1 - f[2]);
                    const auto& v = cube_[static_cast<std::size_t>(((i[2] + db) * kGrid + i[1] + dg) * kGrid + i[0] + dr)];
                    for (int k = 0; k < 3; ++k) acc[k] += w * v[k];
                }
                dst[x] = pack(static_cast<unsigned>(acc[0] + 0.5f), static_cast<unsigned>(acc[1] + 0.5f),
                              static_cast<unsigned>(acc[2] + 0.5f));
            }
        }
    });
}

void ColorConverter::convert(std::span<const std::uint16_t> frame, int width, int height, Image& out) const {
    out.resize(width, height);
    if (!deband_) {
        parallel_for(height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const std::uint16_t* src = frame.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
                std::uint32_t* dst = out.row(y);
                for (int x = 0; x < width; ++x) dst[x] = convert(src[x]);
            }
        });
        return;
    }
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return frame[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    };
    parallel_for(height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::uint32_t* dst = out.row(y);
            for (int x = 0; x < width; ++x) {
                const std::uint16_t center = at(x, y);
                int sum[3] = {0, 0, 0};
                bool smoothed = false;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const std::uint16_t n = at(x + dx, y + dy);
                        for (int c = 0; c < 3; ++c) {
                            const int cv = (center >> (5 * c)) & 31, nv = (n >> (5 * c)) & 31;
                            // Neighbours one level away belong to the same gradient; anything
                            // further is an edge and keeps the centre value.
                            const int use = std::abs(nv - cv) <= 1 ? nv : cv;
                            sum[c] += use;
                            smoothed |= use != cv;
                        }
                    }
                }
                dst[x] = smoothed ? convert_precise(static_cast<float>(sum[0]) / (9.0f * 31.0f),
                                                    static_cast<float>(sum[1]) / (9.0f * 31.0f),
                                                    static_cast<float>(sum[2]) / (9.0f * 31.0f))
                                  : convert(center);
            }
        }
    });
}

}  // namespace anygba::video
