#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace anygba::test {

// Writes an RGB555 image (red in bits 0-4) as an 8-bit RGB PNG with stored deflate blocks.
void write_png(const std::filesystem::path& path, std::span<const std::uint16_t> rgb555, int width, int height);

// Reads an 8-bit RGB PNG (as written above or by tests/oracle/mgba-oracle.py) back to
// RGB555 by keeping the top 5 bits of each channel. Throws on anything else.
std::vector<std::uint16_t> read_png_rgb555(const std::filesystem::path& path, int width, int height);

// Decompresses a zlib stream (RFC 1950/1951). Throws on malformed input.
std::vector<std::uint8_t> inflate_zlib(const std::vector<std::uint8_t>& data);

}  // namespace anygba::test
