#pragma once

#include "insn.h"

#include <string>

namespace anygba::decode {

// One-line disassembly in Capstone's ARM syntax. Lone Thumb BL halves, which Capstone
// cannot show, print as `bl.hi` / `bl.lo`.
std::string disassemble(const Insn& in);

}  // namespace anygba::decode
