#include "video/hd/png_io.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_surface.h>

#include <cstring>
#include <system_error>

namespace anygba::video::hd {

namespace {

std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return {u.begin(), u.end()};
}

}  // namespace

std::optional<Texture> load_png(const std::filesystem::path& path, std::string* error) {
    SDL_Surface* loaded = SDL_LoadPNG(utf8(path).c_str());
    if (!loaded) {
        if (error) *error = utf8(path) + ": " + SDL_GetError();
        return std::nullopt;
    }
    // ABGR8888 is R, G, B, A in memory on little-endian machines: the layout of Image pixels.
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(loaded);
    if (!rgba) {
        if (error) *error = utf8(path) + ": " + SDL_GetError();
        return std::nullopt;
    }
    Texture t;
    t.width = rgba->w;
    t.height = rgba->h;
    t.pixels.resize(static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height));
    for (int y = 0; y < t.height; ++y) {
        std::memcpy(t.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(t.width),
                    static_cast<const std::uint8_t*>(rgba->pixels) + static_cast<std::ptrdiff_t>(y) * rgba->pitch,
                    static_cast<std::size_t>(t.width) * 4);
    }
    SDL_DestroySurface(rgba);
    return t;
}

std::optional<std::string> save_png(const std::filesystem::path& path, const std::uint32_t* pixels, int width,
                                    int height) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    SDL_Surface* s = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ABGR8888, const_cast<std::uint32_t*>(pixels),
                                           width * 4);
    if (!s) return utf8(path) + ": " + SDL_GetError();
    auto tmp = path;
    tmp += ".tmp";
    const bool ok = SDL_SavePNG(s, utf8(tmp).c_str());
    SDL_DestroySurface(s);
    if (!ok) return utf8(path) + ": " + SDL_GetError();
    std::filesystem::rename(tmp, path, ec);
    if (ec) return utf8(path) + ": " + ec.message();
    return std::nullopt;
}

}  // namespace anygba::video::hd
