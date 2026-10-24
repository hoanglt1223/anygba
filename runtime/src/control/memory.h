#pragma once

#include "anygba/system.hpp"

#include <cstdint>
#include <span>
#include <string>

// Guest memory for observers: reads straight from the backing arrays, so they trigger no
// watches and no I/O side effects (I/O registers read as last written).
namespace anygba::control {

// Throws anygba::Error naming the address when a byte is outside BIOS, EWRAM, IWRAM, I/O,
// palette, VRAM, OAM or the cartridge ROM.
void peek(System& sys, std::uint32_t addr, std::span<std::uint8_t> out);
std::uint32_t peek_le(System& sys, std::uint32_t addr, int size);  // size 1, 2 or 4

// Name of the region holding `addr` ("ewram", "rom", ...), or "unmapped".
std::string region_name(std::uint32_t addr);

}  // namespace anygba::control
