#pragma once

#include "analysis/image.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace anygba::analysis {

// A recognised computed jump whose possible targets are known statically.
struct JumpTable {
    enum class Kind : std::uint8_t {
        LoadPc,         // ldr pc, [base, rI, lsl #2], or a load from base + rI*4 then mov pc / bx
        AddPcBranches,  // add pc, pc, rI, lsl #2 followed by one branch per case
    };
    Kind kind = Kind::LoadPc;
    std::uint32_t jump_addr = 0;  // the instruction that writes PC
    std::uint32_t table_addr = 0;
    std::uint8_t index_reg = 0;   // unscaled case index, for diagnostics
    std::vector<std::uint32_t> targets;  // case order; Thumb targets have bit 0 cleared
};

inline constexpr std::uint32_t kMaxJumpTableEntries = 1024;

// Reads and validates `count` entries of a table found by the CFG walker. LoadPc tables
// hold addresses; AddPcBranches tables are the instruction slots themselves. Returns
// nothing if any entry falls outside the image or the jump's memory area.
std::optional<JumpTable> read_jump_table(const Image& image, JumpTable::Kind kind, std::uint32_t jump_addr,
                                         std::uint32_t table_addr, std::uint32_t count, std::uint8_t index_reg,
                                         Mode mode);

}  // namespace anygba::analysis
