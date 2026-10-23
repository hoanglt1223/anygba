#pragma once

#include "insn.h"

namespace anygba::decode {

// Decodes one Thumb halfword at `addr`. BL halves decode as Op::BL_HI / Op::BL_LO.
Insn decode_thumb(std::uint16_t halfword, std::uint32_t addr);

// Decodes the instruction at `addr`, combining a BL prefix with the following suffix
// into one 4-byte Op::BL. `next` is the halfword at addr + 2 (pass 0 if unavailable).
Insn decode_thumb_at(std::uint16_t halfword, std::uint16_t next, std::uint32_t addr);

}  // namespace anygba::decode
