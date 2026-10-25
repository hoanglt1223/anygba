// u8 *StringCopy(u8 *dest, const u8 *src): copies a 0xFF-terminated string, terminator included, and
// returns the address of the terminator in dest.
//
// The engine's Thumb code, as the recompiler translates it (offsets from the entry):
//   +0x00 push {lr} ; adds r3, r0, #0 ; b +0x0C          block 1: 3 instructions
//   +0x06 strb r2, [r3] ; adds r3, #1 ; adds r1, #1      block 3: 3 instructions
//   +0x0C ldrb r2, [r1] ; adds r0, r2, #0 ; cmp r0, #0xFF ; bne +0x06
//                                                        block 2: 4 instructions
//   +0x14 movs r0, #0xFF ; strb r0, [r3] ; adds r0, r3, #0 ; pop {r1} ; bx r1
//                                                        block 4: 5 instructions
// This override leaves the same registers, flags, stack slot and cycle count behind, block by
// block, so a game runs frame for frame as it does without it.
#include <anygba/override.hpp>

#include <cstdint>

ANYGBA_OVERRIDE(pokemon_gen3_StringCopy, g) {
    const std::uint32_t entry = g.entry();
    const std::uint32_t sp = g.reg(13);
    std::uint32_t dest = g.arg(0);
    std::uint32_t src = g.arg(1);

    g.write<std::uint32_t>(sp - 4, g.reg(14));  // push {lr}: the slot keeps the return address
    g.block(entry + 0x04, 3, 1);
    for (;;) {
        const std::uint32_t ch = g.read<std::uint8_t>(src);
        g.block(entry + 0x12, 4, 1);
        if (ch == 0xFF) break;
        g.write<std::uint8_t>(dest, static_cast<std::uint8_t>(ch));
        ++dest;
        ++src;
        g.block(entry + 0x0C, 3);
    }
    g.write<std::uint8_t>(dest, 0xFF);

    const std::uint32_t ret = g.read<std::uint32_t>(sp - 4);  // pop {r1}
    g.reg(0) = dest;
    g.reg(1) = ret;
    g.reg(2) = 0xFF;
    g.reg(3) = dest;
    g.set_flags({(dest >> 31) != 0, dest == 0, false, false});  // adds r0, r3, #0
    g.block(entry + 0x1C, 5, 1);
    g.ret_to(ret);  // bx r1
}
