#pragma once

#include "anygba/video_options.h"
#include "video/color.h"
#include "video/image.h"

#include <cstdint>
#include <span>

// The filter chain: colour (with debanding) -> ghosting -> pixel-art scaler -> final scaling to
// the output rectangle -> screen effect (a brightness mask multiplied over the output). The
// scalers run on the CPU at most 6x the GBA resolution; the final scaling and the effect run on
// the GPU through the window's renderer (the mask is uploaded once per output size), or on the
// CPU in render() when there is no GPU (and in tests).
namespace anygba::video {

// The largest rectangle of the picture's aspect ratio (3:2 for the GBA) inside an output,
// centred; with integer_scale, the largest whole multiple of the picture size (at least 1).
Rect fit(int out_width, int out_height, int pic_width, int pic_height, bool integer_scale);

struct Prepared {
    const Image* image = nullptr;    // owned by the pipeline, valid until the next call
    bool linear = false;             // scale it to the output bilinearly (else nearest)
    const Image* overlay = nullptr;  // screen effect: multiply the scaled picture by this
    std::uint64_t overlay_version = 0;  // changes whenever the overlay does
};

class Pipeline {
public:
    explicit Pipeline(const Options& options = {});

    void set_options(const Options& options);
    const Options& options() const { return options_; }

    // Runs the chain up to the final scaling, for a picture to be shown in a dst_w x dst_h
    // rectangle. `frame` is width x height BGR555.
    Prepared prepare(std::span<const std::uint16_t> frame, int width, int height, int dst_w, int dst_h);

    // The whole chain on the CPU: the frame letterboxed (black) into target_w x target_h.
    const Image& render(std::span<const std::uint16_t> frame, int width, int height, int target_w, int target_h);

    // The same for an 8-bit RGBA frame showing logical_w x logical_h GBA pixels at any size (HD
    // renderer and AI output). The pixel-art scaler only runs on frames at 1x; screen effects
    // follow the GBA pixel grid.
    Prepared prepare(const Image& frame, int logical_w, int logical_h, int dst_w, int dst_h);
    const Image& render(const Image& frame, int logical_w, int logical_h, int target_w, int target_h);

private:
    Prepared finish(int logical_w, int logical_h, int dst_w, int dst_h);

    Options options_;
    ColorConverter color_;
    Image source_;
    Image previous_;  // last frame before ghosting
    bool has_previous_ = false;
    Image scaled_;
    struct OverlayKey {
        Effect effect = Effect::None;
        int src_w = 0, src_h = 0, out_w = 0, out_h = 0;
        bool operator==(const OverlayKey&) const = default;
    };
    OverlayKey overlay_key_;
    Image overlay_;
    std::uint64_t overlay_version_ = 0;
    Image output_;
    Rect output_area_;
};

}  // namespace anygba::video
