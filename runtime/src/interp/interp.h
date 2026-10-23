#pragma once

#include "anygba/system.hpp"
#include "decode/insn.h"

#include <cstdint>

// Fallback interpreter and reference implementation of ARMv4T semantics. It shares the
// decoder with the recompiler but none of the generated code's helpers, so the
// differential tests compare two independent implementations.
namespace anygba::interp {

// Executes one decoded instruction. On entry r[15] is ignored (in.addr is used); on return
// r[15] holds the next address. Charges cycles and calls the scheduler when they run out.
void execute(System& sys, const decode::Insn& in);

// Fetch and decode only (Thumb BL pairs combined).
decode::Insn fetch(System& sys, std::uint32_t addr, bool thumb);

}  // namespace anygba::interp
