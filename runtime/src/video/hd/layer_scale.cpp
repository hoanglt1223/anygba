#include "video/hd/layer_scale.h"

#include "video/scalers.h"

namespace anygba::video::hd {

namespace {

bool clear(std::uint32_t c) { return (c >> 24) < 128; }

}  // namespace

void scale_layer(Scaler s, const Image& in, Image& out) {
    const int w = in.width, h = in.height;
    bool any_clear = false;
    for (const std::uint32_t c : in.pixels) any_clear |= clear(c);

    Image colors;
    colors.resize(w, h);
    Image mask;
    if (any_clear) mask.resize(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint32_t c = in.row(y)[x];
            if (clear(c)) {
                mask.row(y)[x] = kOpaque;
                c = kOpaque;  // black unless a neighbour is opaque
                static constexpr int kNear[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                for (const auto& d : kNear) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const std::uint32_t n = in.row(ny)[nx];
                    if (!clear(n)) {
                        c = n;
                        break;
                    }
                }
            } else if (any_clear) {
                mask.row(y)[x] = 0xFFFFFFFFu;
            }
            colors.row(y)[x] = c | kOpaque;
        }
    }
    upscale(s, colors, out);
    if (!any_clear) return;
    Image scaled_mask;
    upscale(s, mask, scaled_mask);
    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
        const bool covered = red(scaled_mask.pixels[i]) >= 128;
        out.pixels[i] = covered ? (out.pixels[i] | kOpaque) : (out.pixels[i] & 0x00FFFFFFu);
    }
}

void fit_layer(const Image& in, int w, int h, Image& out) {
    if (in.width == w && in.height == h) {
        out = in;
        return;
    }
    out.resize(w, h);
    for (int y = 0; y < h; ++y) {
        const int sy = static_cast<int>(static_cast<long long>(y) * in.height / h);
        const std::uint32_t* src = in.row(sy);
        std::uint32_t* dst = out.row(y);
        for (int x = 0; x < w; ++x) dst[x] = src[static_cast<long long>(x) * in.width / w];
    }
}

}  // namespace anygba::video::hd
