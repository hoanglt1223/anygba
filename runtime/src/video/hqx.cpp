// hq2x/hq3x/hq4x in the spirit of Maxim Stepin's hqx, written from the published description
// (neighbour colours compared in YUV with fixed thresholds; output sub-pixels near a corner are
// interpolated toward the neighbours that form an edge there). It does not use hqx's lookup
// tables or code (LGPL), so its output differs from the reference implementation in detail.
#include "video/scalers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace anygba::video::detail {

namespace {

// Weight (0..256) of corner (cx, cy) on sub-pixel (i, j) of an n x n block: highest at the
// corner, falling to zero at the block centre.
struct CornerWeights {
    std::array<std::vector<unsigned>, 4> w;  // corner index (cy * 2 + cx), then j * n + i
};

const CornerWeights& corner_weights(int n) {
    static const std::array<CornerWeights, 5> tables = [] {
        std::array<CornerWeights, 5> t;
        for (int size = 2; size <= 4; ++size) {
            for (int corner = 0; corner < 4; ++corner) {
                const double cx = corner & 1, cy = corner >> 1;
                auto& w = t[static_cast<std::size_t>(size)].w[static_cast<std::size_t>(corner)];
                w.resize(static_cast<std::size_t>(size * size));
                for (int j = 0; j < size; ++j) {
                    for (int i = 0; i < size; ++i) {
                        const double du = std::abs((i + 0.5) / size - cx), dv = std::abs((j + 0.5) / size - cy);
                        double v = 0;
                        if (du <= 0.5 + 1e-9 && dv <= 0.5 + 1e-9) v = std::clamp((1.0 - du - dv) / 0.667, 0.0, 1.0);
                        w[static_cast<std::size_t>(j * size + i)] = static_cast<unsigned>(std::lround(v * 256.0));
                    }
                }
            }
        }
        return t;
    }();
    return tables[static_cast<std::size_t>(n)];
}

}  // namespace

void hqx(const Padded& s, int n, Image& dst) {
    dst.resize(s.width() * n, s.height() * n);
    const CornerWeights& weights = corner_weights(n);
    parallel_for(s.height(), [&](int y0, int y1) {
        std::vector<std::uint32_t> block(static_cast<std::size_t>(n * n));
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < s.width(); ++x) {
                const std::uint32_t E = s.at(x, y);
                std::fill(block.begin(), block.end(), E);
                for (int corner = 0; corner < 4; ++corner) {
                    const int dx = (corner & 1) ? 1 : -1, dy = (corner >> 1) ? 1 : -1;
                    const std::uint32_t h = s.at(x + dx, y), v = s.at(x, y + dy), d = s.at(x + dx, y + dy);
                    const bool dh = yuv_differ(E, h), dv = yuv_differ(E, v);
                    if (!dh && !dv) continue;
                    std::uint32_t color;
                    unsigned strength;
                    if (dh && dv && !yuv_differ(h, v)) {
                        // An edge between E and its two like neighbours crosses this corner. When
                        // the diagonal matches E it is a thin line of E's colour: blend less.
                        color = mix2(h, v);
                        strength = yuv_differ(E, d) ? 256 : 128;
                    } else {
                        color = dh && dv ? mix2(h, v) : dh ? h : v;
                        strength = 48;
                    }
                    const auto& w = weights.w[static_cast<std::size_t>(corner)];
                    for (std::size_t k = 0; k < block.size(); ++k) {
                        if (w[k]) block[k] = blend(block[k], color, (w[k] * strength) >> 8);
                    }
                }
                for (int j = 0; j < n; ++j) {
                    std::copy_n(block.begin() + j * n, n, dst.row(y * n + j) + x * n);
                }
            }
        }
    });
}

}  // namespace anygba::video::detail
