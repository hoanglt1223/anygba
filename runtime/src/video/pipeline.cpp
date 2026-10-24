#include "video/pipeline.h"

#include "video/scalers.h"

#include <algorithm>
#include <stdexcept>

namespace anygba::video {

Rect fit(int out_width, int out_height, int pic_width, int pic_height, bool integer_scale) {
    if (out_width <= 0 || out_height <= 0 || pic_width <= 0 || pic_height <= 0) {
        throw std::invalid_argument("fit: sizes must be positive");
    }
    int w, h;
    if (integer_scale) {
        const int s = std::max(1, std::min(out_width / pic_width, out_height / pic_height));
        w = pic_width * s;
        h = pic_height * s;
    } else if (static_cast<long long>(out_width) * pic_height <= static_cast<long long>(out_height) * pic_width) {
        w = out_width;
        h = static_cast<int>((static_cast<long long>(out_width) * pic_height + pic_width / 2) / pic_width);
    } else {
        h = out_height;
        w = static_cast<int>((static_cast<long long>(out_height) * pic_width + pic_height / 2) / pic_height);
    }
    // An integer scale larger than the output (tiny windows) is cropped from the top left.
    return {std::max(0, (out_width - w) / 2), std::max(0, (out_height - h) / 2), std::min(w, out_width),
            std::min(h, out_height)};
}

Pipeline::Pipeline(const Options& options) : options_(options), color_(options) {}

void Pipeline::set_options(const Options& options) {
    if (options.color != options_.color || options.gamma != options_.gamma || options.deband != options_.deband) {
        color_ = ColorConverter(options);
    }
    if (!options.ghosting) has_previous_ = false;
    options_ = options;
}

Prepared Pipeline::prepare(std::span<const std::uint16_t> frame, int width, int height, int dst_w, int dst_h) {
    if (width <= 0 || height <= 0 || frame.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        throw std::invalid_argument("video frame size does not match its dimensions");
    }
    if (dst_w <= 0 || dst_h <= 0) {
        throw std::invalid_argument("video output size must be positive");
    }
    color_.convert(frame, width, height, source_);
    if (options_.ghosting) {
        if (has_previous_ && previous_.width == width && previous_.height == height) {
            for (std::size_t i = 0; i < source_.pixels.size(); ++i) {
                const std::uint32_t now = source_.pixels[i];
                source_.pixels[i] = mix2(now, previous_.pixels[i]);
                previous_.pixels[i] = now;
            }
        } else {
            previous_ = source_;
            has_previous_ = true;
        }
    }

    apply_gain(source_, effect_gain(options_.effect));

    const Scaler scaler = options_.scaler;
    const int whole = std::max(1, std::min(dst_w / width, dst_h / height));
    const Image* image = &source_;
    if (scaler == Scaler::SharpBilinear && whole > 1) {
        // Sharp bilinear: whole-number nearest prescale, so bilinear only blends the last
        // fraction of a pixel at each edge.
        scaled_.resize(width * whole, height * whole);
        resize(source_, scaled_, {0, 0, scaled_.width, scaled_.height}, false);
        image = &scaled_;
    } else if (factor(scaler) > 1) {
        upscale(scaler, source_, scaled_);
        image = &scaled_;
    }

    const Image* overlay = nullptr;
    if (options_.effect != Effect::None) {
        const OverlayKey key{options_.effect, width, height, dst_w, dst_h};
        if (!(key == overlay_key_)) {
            make_overlay(options_.effect, width, height, dst_w, dst_h, overlay_);
            overlay_key_ = key;
            ++overlay_version_;
        }
        overlay = &overlay_;
    }

    const bool exact = dst_w % image->width == 0 && dst_h % image->height == 0 &&
                       dst_w / image->width == dst_h / image->height;
    return {image, !exact && scaler != Scaler::Nearest, overlay, overlay_version_};
}

const Image& Pipeline::render(std::span<const std::uint16_t> frame, int width, int height, int target_w,
                              int target_h) {
    const Rect area = fit(target_w, target_h, width, height, options_.integer_scale);
    const Prepared p = prepare(frame, width, height, area.w, area.h);
    if (output_.width != target_w || output_.height != target_h || output_area_ != area) {
        output_.resize(target_w, target_h);
        std::fill(output_.pixels.begin(), output_.pixels.end(), kOpaque);
        output_area_ = area;
    }
    resize(*p.image, output_, area, p.linear);
    if (p.overlay) multiply(output_, area, *p.overlay);
    return output_;
}

}  // namespace anygba::video
