#include "anygba/system.hpp"

// Processor modes and banked registers (ARM7TDMI). Generated code and the interpreter keep
// the live registers in r[]; banks hold the rest.
namespace anygba {

namespace {

int bank_of(std::uint32_t mode) {
    switch (mode & 0x1F) {
    case ANYGBA_MODE_FIQ: return 1;
    case ANYGBA_MODE_IRQ: return 2;
    case ANYGBA_MODE_SVC: return 3;
    case ANYGBA_MODE_ABT: return 4;
    case ANYGBA_MODE_UND: return 5;
    default: return 0;  // USR, SYS, and invalid modes
    }
}

}  // namespace

void System::set_cpsr(std::uint32_t value) {
    anygba_cpu& c = cpu_;
    const std::uint32_t old_mode = c.ctrl & 0x1F;
    const std::uint32_t new_mode = value & 0x1F;
    const int old_bank = bank_of(old_mode);
    const int new_bank = bank_of(new_mode);
    if (old_bank != new_bank) {
        c.bank_r13_14[old_bank][0] = c.r[13];
        c.bank_r13_14[old_bank][1] = c.r[14];
        c.bank_spsr[old_bank] = c.spsr;
        if (old_bank == 1 || new_bank == 1) {
            const int from = old_bank == 1 ? 1 : 0;
            const int to = new_bank == 1 ? 1 : 0;
            for (int i = 0; i < 5; ++i) {
                c.bank_r8_12[from][i] = c.r[8 + i];
                c.r[8 + i] = c.bank_r8_12[to][i];
            }
        }
        c.r[13] = c.bank_r13_14[new_bank][0];
        c.r[14] = c.bank_r13_14[new_bank][1];
        c.spsr = new_bank == 0 ? 0 : c.bank_spsr[new_bank];
    }
    c.n = (value >> 31) & 1;
    c.z = (value >> 30) & 1;
    c.c = (value >> 29) & 1;
    c.v = (value >> 28) & 1;
    c.t = (value >> 5) & 1;
    c.ctrl = value & 0xDF;
}

void System::msr(std::uint32_t value, std::uint32_t mask, bool to_spsr) {
    std::uint32_t bits = 0;
    if (mask & 1) bits |= 0x000000FF;
    if (mask & 2) bits |= 0x0000FF00;
    if (mask & 4) bits |= 0x00FF0000;
    if (mask & 8) bits |= 0xFF000000;
    const bool privileged = (cpu_.ctrl & 0x1F) != ANYGBA_MODE_USR;
    if (to_spsr) {
        if (bank_of(cpu_.ctrl) != 0) {
            cpu_.spsr = (cpu_.spsr & ~bits) | (value & bits);
        }
        return;
    }
    if (!privileged) {
        bits &= 0xFF000000;
    }
    bits &= ~0x20u;  // MSR cannot change the T bit
    set_cpsr((ag_cpsr(&cpu_) & ~bits) | (value & bits));
}

void System::restore_cpsr() {
    if (bank_of(cpu_.ctrl) != 0) {
        set_cpsr(cpu_.spsr);
    }
}

}  // namespace anygba

namespace {

// Address of the USR-bank copy of register r while in the current mode.
std::uint32_t* user_reg(anygba_cpu* c, unsigned r) {
    const std::uint32_t mode = c->ctrl & 0x1F;
    const bool usr_bank = mode == ANYGBA_MODE_USR || mode == ANYGBA_MODE_SYS;
    if (r >= 8 && r <= 12 && mode == ANYGBA_MODE_FIQ) {
        return &c->bank_r8_12[0][r - 8];
    }
    if ((r == 13 || r == 14) && !usr_bank) {
        return &c->bank_r13_14[0][r - 13];
    }
    return &c->r[r];
}

}  // namespace

extern "C" {

void anygba_msr(anygba_cpu* c, std::uint32_t value, std::uint32_t mask, std::uint32_t to_spsr) {
    anygba::system_of(c).msr(value, mask, to_spsr != 0);
}

void anygba_restore_cpsr(anygba_cpu* c) { anygba::system_of(c).restore_cpsr(); }

void anygba_ldm_user(anygba_cpu* c, std::uint32_t addr, std::uint32_t list) {
    for (unsigned r = 0; r < 15; ++r) {
        if ((list >> r) & 1u) {
            *user_reg(c, r) = ag_rd32(c, addr);
            addr += 4;
        }
    }
}

void anygba_stm_user(anygba_cpu* c, std::uint32_t addr, std::uint32_t list, std::uint32_t pc_value) {
    for (unsigned r = 0; r < 16; ++r) {
        if ((list >> r) & 1u) {
            ag_wr32(c, addr, r == 15 ? pc_value : *user_reg(c, r));
            addr += 4;
        }
    }
}

}
