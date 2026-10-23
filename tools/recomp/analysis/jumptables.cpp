#include "analysis/jumptables.h"

namespace anygba::analysis {

std::optional<JumpTable> read_jump_table(const Image& image, JumpTable::Kind kind, std::uint32_t jump_addr,
                                         std::uint32_t table_addr, std::uint32_t count, std::uint8_t index_reg,
                                         Mode mode) {
    if (count == 0 || count > kMaxJumpTableEntries) {
        return std::nullopt;
    }
    JumpTable table;
    table.kind = kind;
    table.jump_addr = jump_addr;
    table.table_addr = table_addr;
    table.index_reg = index_reg;
    const std::uint32_t align_mask = mode == Mode::Thumb ? ~1u : ~3u;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t slot = table_addr + 4 * i;
        std::uint32_t target = slot;
        if (kind == JumpTable::Kind::LoadPc) {
            const auto value = image.read32(slot);
            if (!value) {
                return std::nullopt;
            }
            target = *value & align_mask;
        }
        // Every case must land in code the image can see, in the same area as the jump.
        if (!image.contains(target, mode == Mode::Thumb ? 2 : 4) || (target >> 24) != (jump_addr >> 24)) {
            return std::nullopt;
        }
        table.targets.push_back(target);
    }
    return table;
}

}  // namespace anygba::analysis
