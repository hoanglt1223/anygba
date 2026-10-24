#pragma once

#include <cstdint>
#include <span>

namespace anygba {

// CRC-32 (IEEE 802.3, the zlib/PNG polynomial). Names ROM images in cheat databases and
// guards state files against corruption.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

}  // namespace anygba
