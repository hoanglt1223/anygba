// Scale2x/3x/4x, Eagle and the 2xSaI family, written from the published descriptions of the
// algorithms (THIRD_PARTY.md); plus resampling and the screen effects.
#include "video/scalers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <stdexcept>

namespace anygba::video {

namespace detail {

Padded::Padded(const Image& src) : width_(src.width), height_(src.height), stride_(src.width + 2 * kPad) {
    pixels_.resize(static_cast<std::size_t>(stride_) * static_cast<std::size_t>(height_ + 2 * kPad));
    for (int y = -kPad; y < height_ + kPad; ++y) {
        const std::uint32_t* row = src.row(std::clamp(y, 0, height_ - 1));
        std::uint32_t* out = pixels_.data() + static_cast<std::size_t>(y + kPad) * static_cast<std::size_t>(stride_);
        for (int x = -kPad; x < width_ + kPad; ++x) out[x + kPad] = row[std::clamp(x, 0, width_ - 1)];
    }
}

namespace {

struct Yuv {
    int y, u, v;
};

Yuv to_yuv(std::uint32_t p) {
    const int r = static_cast<int>(red(p)), g = static_cast<int>(green(p)), b = static_cast<int>(blue(p));
    return {(299 * r + 587 * g + 114 * b) / 1000, (-169 * r - 331 * g + 500 * b) / 1000,
            (500 * r - 419 * g - 81 * b) / 1000};
}

}  // namespace

int yuv_distance(std::uint32_t a, std::uint32_t b) {
    if (a == b) return 0;
    const Yuv x = to_yuv(a), y = to_yuv(b);
    return 48 * std::abs(x.y - y.y) + 7 * std::abs(x.u - y.u) + 6 * std::abs(x.v - y.v);
}

bool yuv_differ(std::uint32_t a, std::uint32_t b) {
    if (a == b) return false;
    const Yuv x = to_yuv(a), y = to_yuv(b);
    return std::abs(x.y - y.y) > 48 || std::abs(x.u - y.u) > 7 || std::abs(x.v - y.v) > 6;
}

}  // namespace detail

namespace {

using detail::Padded;

void put(Image& dst, int x, int y, int n, int i, int j, std::uint32_t c) { dst.row(y * n + j)[x * n + i] = c; }

// Runs fn(x, y) for every source pixel, rows split across threads.
template <typename Fn>
void each_pixel(const Padded& s, Fn&& fn) {
    parallel_for(s.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < s.width(); ++x) fn(x, y);
        }
    });
}

void scale2x(const Padded& s, Image& dst) {
    dst.resize(s.width() * 2, s.height() * 2);
    each_pixel(s, [&](int x, int y) {
        const std::uint32_t B = s.at(x, y - 1), D = s.at(x - 1, y), E = s.at(x, y), F = s.at(x + 1, y),
                            H = s.at(x, y + 1);
        std::uint32_t e0 = E, e1 = E, e2 = E, e3 = E;
        if (B != H && D != F) {
            e0 = D == B ? D : E;
            e1 = B == F ? F : E;
            e2 = D == H ? D : E;
            e3 = H == F ? F : E;
        }
        put(dst, x, y, 2, 0, 0, e0);
        put(dst, x, y, 2, 1, 0, e1);
        put(dst, x, y, 2, 0, 1, e2);
        put(dst, x, y, 2, 1, 1, e3);
    });
}

void scale3x(const Padded& s, Image& dst) {
    dst.resize(s.width() * 3, s.height() * 3);
    each_pixel(s, [&](int x, int y) {
        const std::uint32_t A = s.at(x - 1, y - 1), B = s.at(x, y - 1), C = s.at(x + 1, y - 1), D = s.at(x - 1, y),
                            E = s.at(x, y), F = s.at(x + 1, y), G = s.at(x - 1, y + 1), H = s.at(x, y + 1),
                            I = s.at(x + 1, y + 1);
        std::uint32_t e[9] = {E, E, E, E, E, E, E, E, E};
        if (B != H && D != F) {
            e[0] = D == B ? D : E;
            e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
            e[2] = B == F ? F : E;
            e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
            e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
            e[6] = D == H ? D : E;
            e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
            e[8] = H == F ? F : E;
        }
        for (int k = 0; k < 9; ++k) put(dst, x, y, 3, k % 3, k / 3, e[k]);
    });
}

// Eagle fills a corner with the neighbour colour when the three pixels around that corner
// agree; Super Eagle also smooths corners where only the two edge neighbours agree.
void eagle(const Padded& s, bool super, Image& dst) {
    dst.resize(s.width() * 2, s.height() * 2);
    each_pixel(s, [&](int x, int y) {
        const std::uint32_t E = s.at(x, y);
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const int dx = i ? 1 : -1, dy = j ? 1 : -1;
                const std::uint32_t h = s.at(x + dx, y), v = s.at(x, y + dy), d = s.at(x + dx, y + dy);
                std::uint32_t out = E;
                if (h == v && v == d) {
                    out = h;
                } else if (super && h == v && h != E) {
                    out = mix2(h, E);
                }
                put(dst, x, y, 2, i, j, out);
            }
        }
    });
}

// Which colour the two neighbours c and d side with: +1 for a, -1 for b, 0 for neither.
int side(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    const int for_a = (c == a) + (d == a);
    const int for_b = (c == b) + (d == b);
    return (for_a > for_b) - (for_b > for_a);
}

// 2xSaI: each pixel A with its right, lower and lower-right neighbours B, C, D forms a block;
// a diagonal of equal colours in the block is extended, ties between the two diagonals are
// settled by the surrounding pixels, and the rest is interpolated. The Super 2xSaI variant
// weights the interpolations toward the centre pixel, which keeps more of the original shapes.
void sai(const Padded& s, bool super, Image& dst) {
    dst.resize(s.width() * 2, s.height() * 2);
    each_pixel(s, [&](int x, int y) {
        const auto p = [&](int dx, int dy) { return s.at(x + dx, y + dy); };
        const std::uint32_t A = p(0, 0), B = p(1, 0), C = p(0, 1), D = p(1, 1);
        const std::uint32_t E = p(0, -1), F = p(1, -1), G = p(-1, 0), H = p(-1, 1), I = p(-1, -1);
        const std::uint32_t J = p(2, -1), K = p(2, 0), L = p(2, 1), M = p(-1, 2), N = p(0, 2), O = p(1, 2);
        const auto interp = [&](std::uint32_t a, std::uint32_t b) { return super ? blend(a, b, 64) : mix2(a, b); };
        const auto quad = [&] { return super ? blend(A, mix4(A, B, C, D), 128) : mix4(A, B, C, D); };

        std::uint32_t right, below, diag;
        if (A == D && B != C) {
            right = (A == E && B == L) || (A == C && A == F && B != E && B == J) ? A : interp(A, B);
            below = (A == G && C == O) || (A == B && A == H && G != C && C == M) ? A : interp(A, C);
            diag = A;
        } else if (B == C && A != D) {
            right = (B == F && A == H) || (B == E && B == D && A != F && A == I) ? B : interp(A, B);
            below = (C == H && A == F) || (C == G && C == D && A != H && A == I) ? C : interp(A, C);
            diag = B;
        } else if (A == D && B == C) {
            if (A == B) {
                right = below = diag = A;
            } else {
                const int r = side(A, B, G, E) + side(A, B, K, F) + side(A, B, H, N) + side(A, B, L, O);
                diag = r > 0 ? A : r < 0 ? B : quad();
                right = interp(A, B);
                below = interp(A, C);
            }
        } else {
            diag = quad();
            if (A == C && A == F && B != E && B == J) {
                right = A;
            } else if (B == E && B == D && A != F && A == I) {
                right = B;
            } else {
                right = interp(A, B);
            }
            if (A == B && A == H && G != C && C == M) {
                below = A;
            } else if (C == G && C == D && A != H && A == I) {
                below = C;
            } else {
                below = interp(A, C);
            }
        }
        put(dst, x, y, 2, 0, 0, A);
        put(dst, x, y, 2, 1, 0, right);
        put(dst, x, y, 2, 0, 1, below);
        put(dst, x, y, 2, 1, 1, diag);
    });
}

}  // namespace

void upscale(Scaler s, const Image& src, Image& dst) {
    if (src.width <= 0 || src.height <= 0) throw std::invalid_argument("upscale: empty source");
    if (factor(s) == 1) {
        dst = src;
        return;
    }
    const Padded padded(src);
    switch (s) {
    case Scaler::Scale2x: scale2x(padded, dst); return;
    case Scaler::Scale3x: scale3x(padded, dst); return;
    case Scaler::Scale4x: {
        Image twice;
        scale2x(padded, twice);
        scale2x(Padded(twice), dst);
        return;
    }
    case Scaler::Eagle: eagle(padded, false, dst); return;
    case Scaler::SuperEagle: eagle(padded, true, dst); return;
    case Scaler::Sai2x: sai(padded, false, dst); return;
    case Scaler::Super2xSai: sai(padded, true, dst); return;
    case Scaler::Hq2x:
    case Scaler::Hq3x:
    case Scaler::Hq4x: detail::hqx(padded, factor(s), dst); return;
    case Scaler::Xbr2x:
    case Scaler::Xbr3x:
    case Scaler::Xbr4x: detail::xbr(padded, factor(s), false, dst); return;
    default: detail::xbr(padded, factor(s), true, dst); return;
    }
}

void resize(const Image& src, Image& dst, Rect area, bool linear) {
    if (src.width <= 0 || src.height <= 0 || area.w <= 0 || area.h <= 0 || area.x < 0 || area.y < 0 ||
        area.x + area.w > dst.width || area.y + area.h > dst.height) {
        throw std::invalid_argument("resize: bad area");
    }
    struct Tap {
        int i0, i1;
        unsigned w;  // weight of i1, 0..256
    };
    const auto taps = [&](int out, int in) {
        std::vector<Tap> t(static_cast<std::size_t>(out));
        for (int o = 0; o < out; ++o) {
            if (!linear) {
                const int i = static_cast<int>((2LL * o + 1) * in / (2LL * out));
                t[static_cast<std::size_t>(o)] = {i, i, 0};
                continue;
            }
            const double pos = std::max(0.0, (o + 0.5) * in / out - 0.5);
            const int i0 = std::min(static_cast<int>(pos), in - 1);
            t[static_cast<std::size_t>(o)] = {i0, std::min(i0 + 1, in - 1),
                                              static_cast<unsigned>(std::lround((pos - i0) * 256.0))};
        }
        return t;
    };
    const std::vector<Tap> xs = taps(area.w, src.width), ys = taps(area.h, src.height);
    parallel_for(area.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const Tap ty = ys[static_cast<std::size_t>(y)];
            const std::uint32_t* r0 = src.row(ty.i0);
            const std::uint32_t* r1 = src.row(ty.i1);
            std::uint32_t* out = dst.row(area.y + y) + area.x;
            if (!linear) {
                for (int x = 0; x < area.w; ++x) out[x] = r0[xs[static_cast<std::size_t>(x)].i0];
                continue;
            }
            for (int x = 0; x < area.w; ++x) {
                const Tap tx = xs[static_cast<std::size_t>(x)];
                const std::uint32_t top = blend(r0[tx.i0], r0[tx.i1], tx.w);
                const std::uint32_t bottom = blend(r1[tx.i0], r1[tx.i1], tx.w);
                out[x] = blend(top, bottom, ty.w);
            }
        }
    });
}

unsigned effect_gain(Effect e) { return e == Effect::Crt ? 346 : 256; }

void make_overlay(Effect e, int src_w, int src_h, int out_w, int out_h, Image& overlay) {
    if (src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0) throw std::invalid_argument("make_overlay: bad size");
    overlay.resize(out_w, out_h);
    if (e == Effect::None) {
        std::fill(overlay.pixels.begin(), overlay.pixels.end(), pack(255, 255, 255));
        return;
    }
    // Which source cell an output pixel shows, by its centre (the mapping nearest scaling
    // uses), and where inside that cell it lies.
    const auto cell = [](int o, int in, int out) { return static_cast<int>((2LL * o + 1) * in / (2LL * out)); };
    const auto inner = [](int o, int in, int out) {
        const double p = (o + 0.5) * in / out;
        return p - std::floor(p);
    };
    const auto last_in_cell = [&](int o, int in, int out) { return o + 1 == out || cell(o + 1, in, out) != cell(o, in, out); };
    const auto first_in_cell = [&](int o, int in, int out) { return o == 0 || cell(o - 1, in, out) != cell(o, in, out); };
    const double cell_w = static_cast<double>(out_w) / src_w, cell_h = static_cast<double>(out_h) / src_h;

    // Brightness (0..255) per row, and per column and channel.
    std::vector<unsigned> row(static_cast<std::size_t>(out_h), 255);
    std::vector<std::array<unsigned, 3>> col(static_cast<std::size_t>(out_w), {255, 255, 255});
    switch (e) {
    case Effect::LcdGrid: {
        // Dark lines between LCD cells, like the gaps between a GBA screen's pixels.
        const unsigned line_x = cell_w < 3 ? 204 : 165, line_y = cell_h < 3 ? 204 : 165;
        for (int x = 0; x < out_w; ++x) {
            unsigned v = 255;
            if (last_in_cell(x, src_w, out_w)) v = line_x;
            else if (cell_w >= 6 && first_in_cell(x, src_w, out_w)) v = 217;
            col[static_cast<std::size_t>(x)] = {v, v, v};
        }
        for (int y = 0; y < out_h; ++y) {
            if (last_in_cell(y, src_h, out_h)) row[static_cast<std::size_t>(y)] = line_y;
            else if (cell_h >= 6 && first_in_cell(y, src_h, out_h)) row[static_cast<std::size_t>(y)] = 217;
        }
        break;
    }
    case Effect::Scanlines:
    case Effect::Crt: {
        // A beam profile across each source line: full brightness in the middle, dark between
        // lines. Below three output lines per source line, every second line is darkened.
        const double floor = e == Effect::Crt ? 0.35 : 0.55;
        for (int y = 0; y < out_h; ++y) {
            const double v = cell_h < 3 ? (last_in_cell(y, src_h, out_h) ? floor + 0.15 : 1.0)
                                        : floor + (1.0 - floor) * std::sin(std::numbers::pi * inner(y, src_h, out_h));
            row[static_cast<std::size_t>(y)] = static_cast<unsigned>(std::lround(v * 255.0));
        }
        if (e == Effect::Crt) {
            // Aperture grille: red, green and blue stripes, one output pixel each.
            for (int x = 0; x < out_w; ++x) {
                auto& c = col[static_cast<std::size_t>(x)];
                for (int ch = 0; ch < 3; ++ch) c[static_cast<std::size_t>(ch)] = x % 3 == ch ? 255 : 179;
            }
        }
        break;
    }
    case Effect::None: break;
    }
    parallel_for(out_h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const unsigned fy = row[static_cast<std::size_t>(y)];
            std::uint32_t* px = overlay.row(y);
            for (int x = 0; x < out_w; ++x) {
                const auto& c = col[static_cast<std::size_t>(x)];
                px[x] = pack((c[0] * fy + 127) / 255, (c[1] * fy + 127) / 255, (c[2] * fy + 127) / 255);
            }
        }
    });
}

void multiply(Image& image, Rect area, const Image& overlay) {
    if (overlay.width != area.w || overlay.height != area.h || area.x < 0 || area.y < 0 ||
        area.x + area.w > image.width || area.y + area.h > image.height) {
        throw std::invalid_argument("multiply: overlay does not match the area");
    }
    parallel_for(area.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::uint32_t* px = image.row(area.y + y) + area.x;
            const std::uint32_t* m = overlay.row(y);
            for (int x = 0; x < area.w; ++x) {
                const std::uint32_t p = px[x], f = m[x];
                px[x] = pack((red(p) * red(f) + 127) / 255, (green(p) * green(f) + 127) / 255,
                             (blue(p) * blue(f) + 127) / 255);
            }
        }
    });
}

void apply_gain(Image& image, unsigned gain) {
    if (gain == 256) return;
    for (std::uint32_t& p : image.pixels) {
        p = pack(std::min(255u, (red(p) * gain + 128) >> 8), std::min(255u, (green(p) * gain + 128) >> 8),
                 std::min(255u, (blue(p) * gain + 128) >> 8));
    }
}

}  // namespace anygba::video
