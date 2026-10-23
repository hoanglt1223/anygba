#pragma once

#include "analysis/functions.h"

#include <set>
#include <string>
#include <vector>

namespace anygba::analysis {

struct WalkResult {
    Function function;
    std::vector<EntryPoint> found;       // entries this function reveals (calls, constant branches, IRQ vector)
    std::vector<EntryPoint> candidates;  // weak: code-looking literal pool values
    std::vector<std::string> notes;
};

// Follows every path from `entry` with a worklist, tracking constants and simple table
// arithmetic per path. A branch to an address in `known_entries` (other than `entry`)
// is a tail call and ends the path; so do returns, unresolved indirect jumps, and calls
// to functions in `noreturn`.
WalkResult walk_function(const Image& image, const EntryPoint& entry, const std::set<std::uint32_t>& known_entries,
                         const std::set<std::uint32_t>& noreturn, const std::vector<AddressRange>& skip);

// Decodes the instruction at `addr` in `mode` (Thumb BL pairs combined), or an undefined
// instruction if the bytes are not in the image.
decode::Insn decode_at(const Image& image, std::uint32_t addr, Mode mode);

}  // namespace anygba::analysis
