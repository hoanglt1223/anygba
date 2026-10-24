#include "video/presenter.h"

namespace anygba::video {

namespace pf = anyport::platform;

Presenter::Presenter(pf::Window& window, const Options& options)
    : window_(window), gpu_(window.accelerated()) {
    set_options(options);
}

void Presenter::set_options(const Options& options) {
    const Options before = pipeline_.options();
    pipeline_.set_options(options);
    if (options.fullscreen != before.fullscreen) window_.set_fullscreen(options.fullscreen);
    std::pair<int, int> size{0, 0};
    if (const auto fixed = output_size(options.output)) {
        size = *fixed;
    } else if (options.output == Output::Desktop) {
        size = pf::Window::desktop_size();
    }
    window_.set_render_size(size.first, size.second);
    // A fixed resolution in a window also sizes the window, when it fits on the desktop.
    if (!options.fullscreen && size.first > 0 && (options.output != before.output || before.fullscreen)) {
        const auto [dw, dh] = pf::Window::desktop_size();
        if (size.first < dw && size.second < dh) window_.set_window_size(size.first, size.second);
    }
}

void Presenter::present(std::span<const std::uint16_t> frame, int width, int height, pf::Image* capture) {
    const auto [rw, rh] = window_.render_size();
    if (rw <= 0 || rh <= 0) return;  // minimised
    if (gpu_) {
        const Rect area = fit(rw, rh, width, height, options().integer_scale);
        const Prepared p = pipeline_.prepare(frame, width, height, area.w, area.h);
        pf::Overlay overlay;
        if (p.overlay) overlay = {p.overlay->pixels, p.overlay->width, p.overlay->height, p.overlay_version};
        window_.present_rgba(p.image->pixels, p.image->width, p.image->height, {area.x, area.y, area.w, area.h},
                             p.linear ? pf::Scaling::Linear : pf::Scaling::Nearest, p.overlay ? &overlay : nullptr,
                             capture);
        return;
    }
    const Image& image = pipeline_.render(frame, width, height, rw, rh);
    window_.present_rgba(image.pixels, image.width, image.height, {0, 0, rw, rh}, pf::Scaling::Nearest, nullptr,
                         capture);
}

}  // namespace anygba::video
