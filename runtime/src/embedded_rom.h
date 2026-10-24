#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

// `anygba build` appends the user's ROM to the game executable: ROM bytes, then the ROM size
// as a little-endian u64, then kEmbeddedRomMagic. In a macOS .app the ROM is instead
// Contents/Resources/game.gba, because appended data breaks code signing.
namespace anygba {

inline constexpr std::string_view kEmbeddedRomMagic = "AGBAROM1";

std::optional<std::filesystem::path> executable_path();

// The embedded ROM, or nullopt if this executable has none. Throws std::runtime_error if
// the trailer is present but the data is cut short.
std::optional<std::vector<std::uint8_t>> read_embedded_rom();
std::optional<std::vector<std::uint8_t>> read_appended_rom(const std::filesystem::path& file);

}  // namespace anygba
