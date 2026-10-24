#include <anyport/platform/window.h>

#include "sdl_error.h"

#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>

namespace anyport::platform {

namespace {

SDL_PixelFormat to_sdl(PixelFormat format) {
    switch (format) {
    case PixelFormat::Rgb555:
        return SDL_PIXELFORMAT_XBGR1555;
    }
    throw Error("unsupported pixel format");
}

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

Image read_output(SDL_Renderer* renderer) {
    SurfacePtr raw(SDL_RenderReadPixels(renderer, nullptr));
    if (!raw) {
        detail::throw_sdl_error("SDL_RenderReadPixels");
    }
    SurfacePtr rgba(SDL_ConvertSurface(raw.get(), SDL_PIXELFORMAT_RGBA32));
    if (!rgba) {
        detail::throw_sdl_error("SDL_ConvertSurface");
    }
    Image image;
    image.width = rgba->w;
    image.height = rgba->h;
    const auto row_bytes = static_cast<std::size_t>(rgba->w) * 4;
    image.rgba.resize(row_bytes * static_cast<std::size_t>(rgba->h));
    const auto* src = static_cast<const std::uint8_t*>(rgba->pixels);
    for (int y = 0; y < rgba->h; ++y) {
        std::memcpy(image.rgba.data() + row_bytes * static_cast<std::size_t>(y),
                    src + static_cast<std::ptrdiff_t>(y) * rgba->pitch, row_bytes);
    }
    return image;
}

}  // namespace

Window::Window(const WindowConfig& config) : width_(config.width), height_(config.height) {
    if (config.width <= 0 || config.height <= 0) {
        throw Error("window framebuffer size must be positive");
    }
    if (config.scale <= 0) {
        throw Error("window scale must be positive");
    }
    const SDL_PixelFormat format = to_sdl(config.format);

    window_ = SDL_CreateWindow(config.title.c_str(), config.width * config.scale, config.height * config.scale,
                               SDL_WINDOW_RESIZABLE | (config.hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!window_) {
        detail::throw_sdl_error("SDL_CreateWindow");
    }
    try {
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            detail::throw_sdl_error("SDL_CreateRenderer");
        }
        // VSync is best effort: some drivers (dummy, some VMs) cannot provide it.
        SDL_SetRenderVSync(renderer_, config.vsync ? 1 : 0);
        if (!SDL_SetRenderLogicalPresentation(renderer_, config.width, config.height,
                                              SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)) {
            detail::throw_sdl_error("SDL_SetRenderLogicalPresentation");
        }
        texture_ = SDL_CreateTexture(renderer_, format, SDL_TEXTUREACCESS_STREAMING,
                                     config.width, config.height);
        if (!texture_) {
            detail::throw_sdl_error("SDL_CreateTexture");
        }
        if (!SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST)) {
            detail::throw_sdl_error("SDL_SetTextureScaleMode");
        }
    } catch (...) {
        if (texture_) SDL_DestroyTexture(texture_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        throw;
    }
}

Window::~Window() {
    if (target_) SDL_DestroyTexture(target_);
    if (overlay_) SDL_DestroyTexture(overlay_);
    if (rgba_) SDL_DestroyTexture(rgba_);
    SDL_DestroyTexture(texture_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
}

void Window::present(std::span<const std::uint16_t> pixels, Image* capture) {
    const auto expected = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    if (pixels.size() != expected) {
        throw Error("framebuffer has " + std::to_string(pixels.size()) + " pixels, expected " +
                    std::to_string(expected));
    }
    if (!SDL_UpdateTexture(texture_, nullptr, pixels.data(),
                           width_ * static_cast<int>(sizeof(std::uint16_t)))) {
        detail::throw_sdl_error("SDL_UpdateTexture");
    }
    if (!logical_) {
        if (!SDL_SetRenderLogicalPresentation(renderer_, width_, height_, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)) {
            detail::throw_sdl_error("SDL_SetRenderLogicalPresentation");
        }
        logical_ = true;
    }
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE);
    if (!SDL_RenderClear(renderer_) || !SDL_RenderTexture(renderer_, texture_, nullptr, nullptr)) {
        detail::throw_sdl_error("render");
    }
    if (capture) {
        *capture = read_output(renderer_);
    }
    if (!SDL_RenderPresent(renderer_)) {
        detail::throw_sdl_error("SDL_RenderPresent");
    }
}

void Window::present_rgba(std::span<const std::uint32_t> pixels, int width, int height, Rect dst, Scaling scaling,
                          const Overlay* overlay, Image* capture) {
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        throw Error("picture has " + std::to_string(pixels.size()) + " pixels, expected " + std::to_string(width) +
                    "x" + std::to_string(height));
    }
    if (logical_) {
        if (!SDL_SetRenderLogicalPresentation(renderer_, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)) {
            detail::throw_sdl_error("SDL_SetRenderLogicalPresentation");
        }
        logical_ = false;
    }
    if (!rgba_ || rgba_width_ != width || rgba_height_ != height) {
        if (rgba_) SDL_DestroyTexture(rgba_);
        rgba_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!rgba_) {
            rgba_width_ = rgba_height_ = 0;
            detail::throw_sdl_error("SDL_CreateTexture");
        }
        rgba_width_ = width;
        rgba_height_ = height;
        if (!SDL_SetTextureBlendMode(rgba_, SDL_BLENDMODE_NONE)) {
            detail::throw_sdl_error("SDL_SetTextureBlendMode");
        }
    }
    if (!SDL_UpdateTexture(rgba_, nullptr, pixels.data(), width * 4)) {
        detail::throw_sdl_error("SDL_UpdateTexture");
    }
    if (!SDL_SetTextureScaleMode(rgba_, scaling == Scaling::Linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST)) {
        detail::throw_sdl_error("SDL_SetTextureScaleMode");
    }
    const SDL_FRect rect{static_cast<float>(dst.x), static_cast<float>(dst.y), static_cast<float>(dst.w),
                         static_cast<float>(dst.h)};
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE);
    if (target_ && !SDL_SetRenderTarget(renderer_, target_)) {
        detail::throw_sdl_error("SDL_SetRenderTarget");
    }
    if (!SDL_RenderClear(renderer_) || !SDL_RenderTexture(renderer_, rgba_, nullptr, &rect)) {
        detail::throw_sdl_error("render");
    }
    if (overlay) {
        if (overlay->width != dst.w || overlay->height != dst.h ||
            overlay->pixels.size() != static_cast<std::size_t>(dst.w) * static_cast<std::size_t>(dst.h)) {
            throw Error("overlay does not match the destination rectangle");
        }
        if (!overlay_ || overlay_width_ != dst.w || overlay_height_ != dst.h || overlay_version_ != overlay->version) {
            if (overlay_) SDL_DestroyTexture(overlay_);
            overlay_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, dst.w, dst.h);
            if (!overlay_) {
                overlay_width_ = overlay_height_ = 0;
                detail::throw_sdl_error("SDL_CreateTexture (overlay)");
            }
            overlay_width_ = dst.w;
            overlay_height_ = dst.h;
            overlay_version_ = overlay->version;
            if (!SDL_UpdateTexture(overlay_, nullptr, overlay->pixels.data(), dst.w * 4) ||
                !SDL_SetTextureBlendMode(overlay_, SDL_BLENDMODE_MOD) ||
                !SDL_SetTextureScaleMode(overlay_, SDL_SCALEMODE_NEAREST)) {
                detail::throw_sdl_error("overlay texture");
            }
        }
        if (!SDL_RenderTexture(renderer_, overlay_, nullptr, &rect)) {
            detail::throw_sdl_error("render overlay");
        }
    }
    if (capture) {
        *capture = read_output(renderer_);
    }
    if (target_) {
        if (!SDL_SetRenderTarget(renderer_, nullptr)) {
            detail::throw_sdl_error("SDL_SetRenderTarget");
        }
        int ow = 0, oh = 0, tw = 0, th = 0;
        SDL_GetCurrentRenderOutputSize(renderer_, &ow, &oh);
        std::tie(tw, th) = render_size();
        // Largest rectangle of the target's aspect ratio that fits the window, centred.
        const double s = std::min(static_cast<double>(ow) / tw, static_cast<double>(oh) / th);
        const float w = static_cast<float>(tw * s), h = static_cast<float>(th * s);
        const SDL_FRect fit{(static_cast<float>(ow) - w) / 2, (static_cast<float>(oh) - h) / 2, w, h};
        if (!SDL_RenderClear(renderer_) || !SDL_RenderTexture(renderer_, target_, nullptr, &fit)) {
            detail::throw_sdl_error("render");
        }
    }
    if (!SDL_RenderPresent(renderer_)) {
        detail::throw_sdl_error("SDL_RenderPresent");
    }
}

std::pair<int, int> Window::render_size() const {
    if (target_) {
        float w = 0, h = 0;
        SDL_GetTextureSize(target_, &w, &h);
        return {static_cast<int>(w), static_cast<int>(h)};
    }
    int w = 0, h = 0;
    if (!SDL_GetRenderOutputSize(renderer_, &w, &h)) {
        detail::throw_sdl_error("SDL_GetRenderOutputSize");
    }
    return {w, h};
}

void Window::set_render_size(int width, int height) {
    if (width < 0 || height < 0 || (width == 0) != (height == 0)) {
        throw Error("render size must be positive, or 0x0 for the window size");
    }
    if (target_) {
        const auto [w, h] = render_size();
        if (w == width && h == height) return;
        SDL_DestroyTexture(target_);
        target_ = nullptr;
    }
    if (width == 0) return;
    target_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height);
    if (!target_) {
        detail::throw_sdl_error("SDL_CreateTexture (render target)");
    }
    if (!SDL_SetTextureScaleMode(target_, SDL_SCALEMODE_LINEAR)) {
        detail::throw_sdl_error("SDL_SetTextureScaleMode");
    }
}

void Window::set_fullscreen(bool on) {
    if (!SDL_SetWindowFullscreen(window_, on)) {
        detail::throw_sdl_error("SDL_SetWindowFullscreen");
    }
}

void Window::set_window_size(int width, int height) {
    if (width <= 0 || height <= 0) {
        throw Error("window size must be positive");
    }
    if (!SDL_SetWindowSize(window_, width, height)) {
        detail::throw_sdl_error("SDL_SetWindowSize");
    }
}

bool Window::accelerated() const {
    const char* name = SDL_GetRendererName(renderer_);
    return name && std::strcmp(name, SDL_SOFTWARE_RENDERER) != 0;
}

std::pair<int, int> Window::desktop_size() {
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (!mode) return {0, 0};
    return {mode->w, mode->h};
}

}  // namespace anyport::platform
