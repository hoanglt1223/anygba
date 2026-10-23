#include "anygba/system.hpp"

#include <cstdio>
#include <cstring>

// Interrupt delivery. The BIOS IRQ vector saves r0-r3, r12 and lr on the IRQ stack, calls
// the handler stored at 0x03007FFC in ARM state with lr = 0x138, then restores and returns
// with `subs pc, lr, #4`. The runtime does the same synchronously from anygba_tick.
namespace anygba {

namespace {
constexpr std::uint32_t kBiosIrqReturn = 0x138;
}

void System::raise_irq(std::uint16_t bits) {
    std::uint16_t iflag;
    std::memcpy(&iflag, mem_.io.data() + 0x202, 2);
    iflag = static_cast<std::uint16_t>(iflag | bits);
    std::memcpy(mem_.io.data() + 0x202, &iflag, 2);
}

void System::deliver_irq() {
    if (!(io_read16(0x208) & 1) || !(io_read16(0x200) & io_read16(0x202)) || (cpu_.ctrl & 0x80)) {
        return;
    }
    anygba_cpu& c = cpu_;
    const std::uint32_t resume_pc = c.r[15];
    const std::uint32_t old_cpsr = ag_cpsr(&c);
    set_cpsr((old_cpsr & ~0xFFu) | 0x80u | ANYGBA_MODE_IRQ);  // ARM state, IRQs masked
    c.spsr = old_cpsr;
    c.r[14] = resume_pc + 4;

    c.r[13] -= 24;
    const std::uint32_t saved[6] = {c.r[0], c.r[1], c.r[2], c.r[3], c.r[12], c.r[14]};
    for (int i = 0; i < 6; ++i) {
        ag_wr32(&c, c.r[13] + 4u * static_cast<std::uint32_t>(i), saved[i]);
    }
    c.r[0] = 0x04000000;
    c.r[14] = kBiosIrqReturn;
    c.r[15] = ag_rd32(&c, 0x03FFFFFC) & ~3u;

    if (c.ret_depth >= ANYGBA_RET_STACK) {
        unwind();
    }
    c.ret_stack[c.ret_depth++] = kBiosIrqReturn;
    const int unwinding = resume(kBiosIrqReturn);
    c.ret_depth--;
    if (unwinding) {
        unwind();  // an outer frame expects the handler's return address: no sane resume point
    }

    std::uint32_t restored[6];
    for (int i = 0; i < 6; ++i) {
        restored[i] = ag_rd32(&c, c.r[13] + 4u * static_cast<std::uint32_t>(i));
    }
    c.r[13] += 24;
    c.r[0] = restored[0];
    c.r[1] = restored[1];
    c.r[2] = restored[2];
    c.r[3] = restored[3];
    c.r[12] = restored[4];
    c.r[14] = restored[5];
    const std::uint32_t target = c.r[14] - 4;
    restore_cpsr();
    c.r[15] = target & (c.t ? ~1u : ~3u);
    last_bios_opcode_ = 0xE25EF004;
    if (c.r[15] != resume_pc) {
        unwind();  // the handler switched context; restart dispatch at the new PC
    }
}

void System::intr_wait(bool discard_old, std::uint16_t flags) {
    // BIOS IntrWait: the handler acknowledges by setting bits in 0x03007FF8.
    constexpr std::uint32_t kBiosIf = 0x03007FF8;
    io_write16(0x208, 1);
    if (discard_old) {
        write16(kBiosIf, read16(kBiosIf) & ~flags);
    }
    const std::uint32_t resume_pc = cpu_.r[15];
    for (;;) {
        if (read16(kBiosIf) & flags) {
            write16(kBiosIf, read16(kBiosIf) & ~flags);
            break;
        }
        halt_until_irq();
        run_events();
        cpu_.r[15] = resume_pc;
        deliver_irq();
    }
}

void System::swi(std::uint32_t number) {
    last_bios_opcode_ = 0xE3A02004;
    switch (number) {
    case 0x02:  // Halt
        halt_until_irq();
        run_events();
        deliver_irq();
        return;
    case 0x04:  // IntrWait
        intr_wait(cpu_.r[0] != 0, static_cast<std::uint16_t>(cpu_.r[1]));
        return;
    case 0x05:  // VBlankIntrWait
        intr_wait(true, 1);
        return;
    default: {
        char msg[96];
        std::snprintf(msg, sizeof msg, "unimplemented BIOS call SWI 0x%02X at 0x%08X", number,
                      cpu_.r[15] - (cpu_.t ? 2u : 4u));
        fail(msg);
    }
    }
}

}  // namespace anygba

extern "C" void anygba_swi(anygba_cpu* c, std::uint32_t number) { anygba::system_of(c).swi(number); }
