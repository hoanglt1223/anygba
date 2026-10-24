#pragma once

#include "anygba/video_options.h"
#include "video/image.h"

namespace anygba::video {

// Pixel-art scalers: dst becomes factor(s) times the size of src. Scalers with factor 1
// (nearest, bilinear, sharp-bilinear) copy src.
void upscale(Scaler s, const Image& src, Image& dst);

// Resamples all of src into `area` of dst (which must lie inside dst), nearest or bilinear
// with pixel centres aligned.
void resize(const Image& src, Image& dst, Rect area, bool linear);

// A screen effect is a brightness mask for a src_w x src_h picture shown at out_w x out_h
// (white for none); the shown picture is multiplied by it, on the GPU or by multiply().
void make_overlay(Effect e, int src_w, int src_h, int out_w, int out_h, Image& overlay);
void multiply(Image& image, Rect area, const Image& overlay);
// Colour gain (256 = 1) applied before scaling, so the darker effects keep a similar overall
// brightness; apply_gain() clamps at white.
unsigned effect_gain(Effect e);
void apply_gain(Image& image, unsigned gain);

namespace detail {

// Source with a two-pixel border of repeated edge pixels, so scalers read neighbours freely.
class Padded {
public:
    static constexpr int kPad = 2;
    explicit Padded(const Image& src);
    std::uint32_t at(int x, int y) const {
        return pixels_[static_cast<std::size_t>(y + kPad) * static_cast<std::size_t>(stride_) +
                       static_cast<std::size_t>(x + kPad)];
    }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    int width_, height_, stride_;
    std::vector<std::uint32_t> pixels_;
};

// Perceptual distance in YUV, weighted 48:7:6 as in the hqx and xBR descriptions.
int yuv_distance(std::uint32_t a, std::uint32_t b);
// The hqx similarity test: Y within 48, U within 7, V within 6.
bool yuv_differ(std::uint32_t a, std::uint32_t b);

void hqx(const Padded& src, int n, Image& dst);
void xbr(const Padded& src, int n, bool xbrz, Image& dst);

}  // namespace detail

}  // namespace anygba::video
