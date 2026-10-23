#include "interp/interp.h"

#include "decode/arm.h"
#include "decode/thumb.h"

// Instruction fetch for both states. Thumb needs the following halfword to join a BL pair;
// everything after decoding is shared with ARM in arm_interp.cpp.
namespace anygba::interp {

decode::Insn fetch(System& sys, std::uint32_t addr, bool thumb) {
    if (thumb) {
        const auto first = static_cast<std::uint16_t>(sys.read16(addr));
        const auto second = static_cast<std::uint16_t>(sys.read16(addr + 2));
        return decode::decode_thumb_at(first, second, addr);
    }
    return decode::decode_arm(sys.read32(addr), addr);
}

std::uint32_t step(System& sys) {
    anygba_cpu& c = sys.cpu();
    const decode::Insn in = fetch(sys, c.r[15], c.t != 0);
    execute(sys, in);
    return in.next();
}

}  // namespace anygba::interp
