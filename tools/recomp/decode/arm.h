#pragma once

#include "insn.h"

namespace anygba::decode {

// Decodes one ARM (32-bit) instruction at `addr`. Encodings outside ARMv4T, coprocessor
// instructions, and the NV condition decode as Op::UNDEFINED.
Insn decode_arm(std::uint32_t word, std::uint32_t addr);

}  // namespace anygba::decode
