#pragma once

#include "video/hd/keys.h"
#include "video/image.h"

#include <filesystem>
#include <optional>
#include <string>

// PNG files through SDL (any PNG SDL reads; written as 8-bit RGBA).
namespace anygba::video::hd {

// Reads a PNG as RGBA (0xAABBGGRR). Returns nullopt and sets `error` on failure.
std::optional<Texture> load_png(const std::filesystem::path& path, std::string* error);

// Writes RGBA pixels (alpha kept). Returns an error message.
std::optional<std::string> save_png(const std::filesystem::path& path, const std::uint32_t* pixels, int width,
                                    int height);
inline std::optional<std::string> save_png(const std::filesystem::path& path, const Image& image) {
    return save_png(path, image.pixels.data(), image.width, image.height);
}

}  // namespace anygba::video::hd
