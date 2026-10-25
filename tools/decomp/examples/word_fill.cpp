// void WordFill(u32 *dest, u32 bytes): clears `bytes` rounded up to whole words at dest (a crt0 helper).
// The Thumb code, as the recompiler translates it (offsets from the entry):
//   +0x00 movs r2, #3 ; adds r1, r1, r2 ; bics r1, r2 ; beq +0x10   block 1: 4 instructions
//   +0x08 movs r2, #0                                              block 2: 1 instruction
//   +0x0A stm r0!, {r2} ; subs r1, #4 ; bne +0x0A                   block 3: 3 instructions + 1 cycle
//   +0x10 bx lr                                                    block 4: 1 instruction
#include <anygba/override.hpp>

#include <cstdint>

namespace {

// a + b + carry with N, Z, C and V as adds/subs leave them.
std::uint32_t add_with_flags(anygba::guest::Guest& g, std::uint32_t a, std::uint32_t b, std::uint32_t carry) {
    const std::uint64_t wide = std::uint64_t{a} + b + carry;
    const auto r = static_cast<std::uint32_t>(wide);
    g.set_flags({(r >> 31) != 0, r == 0, (wide >> 32) != 0, ((~(a ^ b) & (a ^ r)) >> 31) != 0});
    return r;
}

}  // namespace

ANYGBA_OVERRIDE(example_WordFill, g) {
    const std::uint32_t entry = g.entry();
    std::uint32_t dest = g.reg(0);
    std::uint32_t count = add_with_flags(g, g.reg(1), 3, 0) & ~3u;
    g.set_nz(count);  // bics keeps C and V from the adds
    g.reg(1) = count;
    g.reg(2) = 3;
    g.block(entry + 0x06, 4);
    if (count != 0) {
        g.reg(2) = 0;
        g.set_nz(0);
        g.block(entry + 0x0A, 1);
        do {
            g.write<std::uint32_t>(dest, 0);
            dest += 4;
            g.reg(0) = dest;
            count = add_with_flags(g, count, ~4u, 1);
            g.reg(1) = count;
            g.block(entry + 0x0E, 3, 1);
        } while (count != 0);
    }
    g.block(entry + 0x10, 1);
    g.ret();
}
