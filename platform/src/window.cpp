#include <anyport/platform/window.h>

#include "sdl_error.h"

#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

#include <cstring>
#include <memory>
#include <string>

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

    window_ = SDL_CreateWindow(config.title.c_str(), config.width * config.scale,
                               config.height * config.scale, SDL_WINDOW_RESIZABLE);
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

}  // namespace anyport::platform
