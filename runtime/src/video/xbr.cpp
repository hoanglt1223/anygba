// xBR and xBRZ written from Hyllian's published description of xBR (edge direction from
// weighted colour distances in a 5x5 window, then the corner of each pixel is cut along the
// edge) and the xBRZ write-up (dominant-direction and steep/shallow ratio thresholds, blending
// by the area the edge covers). No xBRZ code (GPL-3) is used; output differs from it in detail.
#include "video/scalers.h"

#include <array>
#include <cmath>
#include <vector>

namespace anygba::video::detail {

namespace {

enum Cut : int { kNone = -1, kDiagonal = 0, kShallow = 1, kSteep = 2, kBoth = 3 };

// Area (0..256) of each sub-pixel of an n x n block that lies past the cut line of the
// bottom-right corner. Coordinates are in source pixels, (1, 1) being the corner.
struct Coverage {
    std::array<std::vector<unsigned>, 4> cut;  // by Cut, then j * n + i
};

bool inside(int cut, double x, double y) {
    switch (cut) {
    case kDiagonal: return x + y > 1.5;
    case kShallow: return y > 1.0 - 0.5 * x;
    case kSteep: return x > 1.0 - 0.5 * y;
    default: return y > 1.0 - 0.5 * x || x > 1.0 - 0.5 * y;
    }
}

const Coverage& coverage(int n) {
    static const std::array<Coverage, 7> tables = [] {
        constexpr int kSamples = 16;
        std::array<Coverage, 7> t;
        for (int size = 2; size <= 6; ++size) {
            for (int cut = 0; cut < 4; ++cut) {
                auto& c = t[static_cast<std::size_t>(size)].cut[static_cast<std::size_t>(cut)];
                c.resize(static_cast<std::size_t>(size * size));
                for (int j = 0; j < size; ++j) {
                    for (int i = 0; i < size; ++i) {
                        int hits = 0;
                        for (int sy = 0; sy < kSamples; ++sy) {
                            for (int sx = 0; sx < kSamples; ++sx) {
                                hits += inside(cut, (i + (sx + 0.5) / kSamples) / size, (j + (sy + 0.5) / kSamples) / size);
                            }
                        }
                        c[static_cast<std::size_t>(j * size + i)] =
                            static_cast<unsigned>(std::lround(256.0 * hits / (kSamples * kSamples)));
                    }
                }
            }
        }
        return t;
    }();
    return tables[static_cast<std::size_t>(n)];
}

struct Decision {
    int cut = kNone;
    std::uint32_t color = 0;
    unsigned strength = 256;
};

// The bottom-right corner of n[2][2], n being the 5x5 window (row, column) mirrored so that
// the corner under test is the bottom-right one.
Decision decide(const std::uint32_t (&n)[5][5], bool xbrz) {
    const std::uint32_t A = n[1][1], B = n[1][2], C = n[1][3], D = n[2][1], E = n[2][2], F = n[2][3];
    const std::uint32_t G = n[3][1], H = n[3][2], I = n[3][3];
    const std::uint32_t F4 = n[2][4], I4 = n[3][4], H5 = n[4][2], I5 = n[4][3];
    if (E == F || E == H) return {};
    const auto d = yuv_distance;
    // Distances across the two candidate edge directions: the corner is cut when colours
    // change less along the F-H direction than along E-I.
    const int wd1 = d(E, C) + d(E, G) + d(I, F4) + d(I, H5) + 4 * d(H, F);
    const int wd2 = d(H, D) + d(H, I5) + d(F, I4) + d(F, B) + 4 * d(E, I);
    if (wd1 >= wd2) return {};

    Decision out;
    out.color = d(E, F) <= d(E, H) ? F : H;
    const int ke = d(F, G), ki = d(H, C);
    if (!xbrz) {
        // Keeps the corners of solid blocks and single-pixel details.
        const bool keep = (F != B && H != D) || (E == I && F != I4 && H != I5) || E == G || E == C;
        if (!keep) return {};
        const bool shallow = 2 * ke <= ki && E != G && D != G;
        const bool steep = ke >= 2 * ki && E != C && B != C;
        out.cut = shallow && steep ? kBoth : shallow ? kShallow : steep ? kSteep : kDiagonal;
        return out;
    }
    const bool dominant = wd1 * 36 < wd2 * 10;
    out.cut = kDiagonal;
    if (!dominant) {
        // A weak edge only rounds the corner if the corner pixel is not part of a straight
        // line through E (which a cut would break).
        if ((G == H && H == I && I == F && F == C) || (A == B && B == C && C == F && F == I)) return {};
        out.strength = 192;
        return out;
    }
    const bool shallow = ke * 22 <= ki * 10 && E != G && D != G;
    const bool steep = ki * 22 <= ke * 10 && E != C && B != C;
    out.cut = shallow && steep ? kBoth : shallow ? kShallow : steep ? kSteep : kDiagonal;
    return out;
}

}  // namespace

void xbr(const Padded& s, int n, bool xbrz, Image& dst) {
    dst.resize(s.width() * n, s.height() * n);
    const Coverage& cov = coverage(n);
    parallel_for(s.height(), [&](int y0, int y1) {
        std::vector<std::uint32_t> block(static_cast<std::size_t>(n * n));
        std::uint32_t win[5][5];
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < s.width(); ++x) {
                const std::uint32_t E = s.at(x, y);
                std::fill(block.begin(), block.end(), E);
                for (int corner = 0; corner < 4; ++corner) {
                    const int mx = (corner & 1) ? 1 : -1, my = (corner >> 1) ? 1 : -1;
                    for (int r = 0; r < 5; ++r) {
                        for (int c = 0; c < 5; ++c) win[r][c] = s.at(x + (c - 2) * mx, y + (r - 2) * my);
                    }
                    const Decision dec = decide(win, xbrz);
                    if (dec.cut == kNone) continue;
                    const auto& area = cov.cut[static_cast<std::size_t>(dec.cut)];
                    for (int j = 0; j < n; ++j) {
                        const int bj = my > 0 ? j : n - 1 - j;
                        for (int i = 0; i < n; ++i) {
                            const unsigned a = area[static_cast<std::size_t>(j * n + i)];
                            if (!a) continue;
                            const int bi = mx > 0 ? i : n - 1 - i;
                            auto& px = block[static_cast<std::size_t>(bj * n + bi)];
                            px = blend(px, dec.color, (a * dec.strength) >> 8);
                        }
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
