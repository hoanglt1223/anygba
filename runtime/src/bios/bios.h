#pragma once

#include "anygba/system.hpp"

#include <cstdint>

// HLE BIOS functions (GBATEK "BIOS Functions"). Each takes its arguments from r0-r3 and
// leaves results and register side effects as the real BIOS does, then charges an
// approximate cycle cost.
namespace anygba::bios {

// Opcodes the BIOS last fetched at well-known points; BIOS-region reads return them.
inline constexpr std::uint32_t kOpcodeAfterBoot = 0xE129F000;
inline constexpr std::uint32_t kOpcodeAfterSwi = 0xE3A02004;
inline constexpr std::uint32_t kOpcodeInIrq = 0xE25EF004;
inline constexpr std::uint32_t kOpcodeAfterIrq = 0xE55EC002;

void boot_state(System& sys);
void soft_reset(System& sys);
void register_ram_reset(System& sys, std::uint32_t flags);

void div(System& sys, std::int32_t num, std::int32_t denom);
void sqrt(System& sys);
void arctan(System& sys);
void arctan2(System& sys);
void midi_key_to_freq(System& sys);

void cpu_set(System& sys);
void cpu_fast_set(System& sys);

void bg_affine_set(System& sys);
void obj_affine_set(System& sys);
// 1.14 fixed-point sine of angle * 2pi / 256, as stored in the BIOS table.
std::int32_t sine(std::uint32_t angle);

void bit_unpack(System& sys);
void lz77(System& sys, bool vram);
void huffman(System& sys);
void run_length(System& sys, bool vram);
void diff_unfilter(System& sys, int unit_bytes, bool vram);

void spend(System& sys, std::int64_t cycles);

}  // namespace anygba::bios
