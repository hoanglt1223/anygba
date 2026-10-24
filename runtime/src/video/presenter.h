#pragma once

#include "anygba/video_options.h"
#include "video/pipeline.h"

#include <anyport/platform/window.h>

#include <cstdint>
#include <span>

namespace anygba::video {

// Shows GBA frames in a window through the filter chain. The final scaling runs on the GPU
// when the window has an accelerated renderer, else the whole chain runs on the CPU.
class Presenter {
public:
    Presenter(anyport::platform::Window& window, const Options& options);

    // Applies new settings at once, including fullscreen and the render resolution.
    void set_options(const Options& options);
    const Options& options() const { return pipeline_.options(); }
    bool gpu() const { return gpu_; }

    // `frame` is width x height BGR555.
    void present(std::span<const std::uint16_t> frame, int width, int height,
                 anyport::platform::Image* capture = nullptr);

private:
    anyport::platform::Window& window_;
    Pipeline pipeline_;
    bool gpu_;
};

}  // namespace anygba::video
