#include "interp/interp.h"

#include "decode/arm.h"

#include <bit>
#include <cstdio>

namespace anygba::interp {

using decode::Cond;
using decode::Insn;
using decode::Op;
using decode::Shift;

namespace {

struct Shifted {
    std::uint32_t value;
    std::uint32_t carry;
};

bool passes(const anygba_cpu& c, Cond cond) {
    switch (cond) {
    case Cond::EQ: return c.z;
    case Cond::NE: return !c.z;
    case Cond::CS: return c.c;
    case Cond::CC: return !c.c;
    case Cond::MI: return c.n;
    case Cond::PL: return !c.n;
    case Cond::VS: return c.v;
    case Cond::VC: return !c.v;
    case Cond::HI: return c.c && !c.z;
    case Cond::LS: return !c.c || c.z;
    case Cond::GE: return c.n == c.v;
    case Cond::LT: return c.n != c.v;
    case Cond::GT: return !c.z && c.n == c.v;
    case Cond::LE: return c.z || c.n != c.v;
    case Cond::AL: return true;
    case Cond::NV: return false;
    }
    return false;
}

// Immediate-amount shifts; `amount` is already normalised (LSR/ASR #32, RRX).
Shifted shift_by_immediate(std::uint32_t v, Shift s, std::uint32_t amount, std::uint32_t carry) {
    switch (s) {
    case Shift::LSL:
        if (amount == 0) return {v, carry};
        return {v << amount, (v >> (32 - amount)) & 1};
    case Shift::LSR:
        if (amount == 32) return {0, v >> 31};
        return {v >> amount, (v >> (amount - 1)) & 1};
    case Shift::ASR:
        if (amount == 32) return {static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> 31), v >> 31};
        return {static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> amount), (v >> (amount - 1)) & 1};
    case Shift::ROR:
        return {std::rotr(v, static_cast<int>(amount)), (v >> (amount - 1)) & 1};
    case Shift::RRX:
        return {(carry << 31) | (v >> 1), v & 1};
    }
    return {v, carry};
}

// Register-amount shifts: only the bottom byte counts, and 0 leaves value and carry alone.
Shifted shift_by_register(std::uint32_t v, Shift s, std::uint32_t amount, std::uint32_t carry) {
    amount &= 0xFF;
    if (amount == 0) {
        return {v, carry};
    }
    switch (s) {
    case Shift::LSL:
        if (amount < 32) return {v << amount, (v >> (32 - amount)) & 1};
        return {0, amount == 32 ? (v & 1) : 0};
    case Shift::LSR:
        if (amount < 32) return {v >> amount, (v >> (amount - 1)) & 1};
        return {0, amount == 32 ? (v >> 31) : 0};
    case Shift::ASR:
        if (amount < 32) {
            return {static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> amount), (v >> (amount - 1)) & 1};
        }
        return {static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> 31), v >> 31};
    case Shift::ROR:
    case Shift::RRX: {
        const std::uint32_t r = amount & 31;
        if (r == 0) return {v, v >> 31};
        return {std::rotr(v, static_cast<int>(r)), (v >> (r - 1)) & 1};
    }
    }
    return {v, carry};
}

class Executor {
public:
    Executor(System& sys, const Insn& in) : sys_(sys), c_(sys.cpu()), in_(in) {}

    void run() {
        next_ = in_.next();
        c_.r[15] = next_;
        if (passes(c_, in_.cond)) {
            dispatch();
        }
        charge();
    }

private:
    // Value of register r as an operand. PC reads 8 (ARM) or 4 (Thumb) ahead, 12 in ARM
    // when a register-specified shift delays the read.
    std::uint32_t reg(unsigned r) const {
        if (r != 15) {
            return c_.r[r];
        }
        std::uint32_t pc = in_.addr + (in_.thumb ? 4u : 8u);
        if (!in_.thumb && in_.shift_by_reg) {
            pc += 4;
        }
        return in_.pc_align ? pc & ~3u : pc;
    }

    void branch(std::uint32_t target) { c_.r[15] = target & (c_.t ? ~1u : ~3u); }

    // Results that ARMv4T leaves UNPREDICTABLE when they target R15 (multiplies, SWP, MRS,
    // block-transfer writeback) are dropped, in step with the code generator.
    void set_gpr(unsigned r, std::uint32_t v) {
        if (r != 15) {
            c_.r[r] = v;
        }
    }

    void set_reg(unsigned r, std::uint32_t v) {
        if (r == 15) {
            branch(v);
        } else {
            c_.r[r] = v;
        }
    }

    Shifted operand2() const {
        if (in_.imm) {
            const std::uint32_t carry = in_.imm_rot ? in_.imm_value >> 31 : c_.c;
            return {in_.imm_value, carry};
        }
        const std::uint32_t v = reg(in_.rm);
        if (in_.shift_by_reg) {
            return shift_by_register(v, in_.shift, reg(in_.rs), c_.c);
        }
        return shift_by_immediate(v, in_.shift, in_.shift_amount, c_.c);
    }

    void set_nz(std::uint32_t r) {
        c_.n = r >> 31;
        c_.z = r == 0 ? 1 : 0;
    }

    std::uint32_t add(std::uint32_t a, std::uint32_t b, std::uint32_t carry_in, bool flags) {
        const std::uint64_t wide = static_cast<std::uint64_t>(a) + b + carry_in;
        const auto r = static_cast<std::uint32_t>(wide);
        if (flags) {
            set_nz(r);
            c_.c = static_cast<std::uint32_t>(wide >> 32);
            c_.v = ((a ^ r) & (b ^ r)) >> 31;
        }
        return r;
    }

    void data_processing() {
        const std::uint32_t a = reg(in_.rn);
        const Shifted op2 = operand2();
        const std::uint32_t b = op2.value;
        const bool s = in_.s;
        std::uint32_t r = 0;
        bool logical = false;
        bool write = true;
        switch (in_.op) {
        case Op::AND: r = a & b; logical = true; break;
        case Op::EOR: r = a ^ b; logical = true; break;
        case Op::SUB: r = add(a, ~b, 1, s); break;
        case Op::RSB: r = add(b, ~a, 1, s); break;
        case Op::ADD: r = add(a, b, 0, s); break;
        case Op::ADC: r = add(a, b, c_.c, s); break;
        case Op::SBC: r = add(a, ~b, c_.c, s); break;
        case Op::RSC: r = add(b, ~a, c_.c, s); break;
        case Op::TST: r = a & b; logical = true; write = false; break;
        case Op::TEQ: r = a ^ b; logical = true; write = false; break;
        case Op::CMP: r = add(a, ~b, 1, true); write = false; break;
        case Op::CMN: r = add(a, b, 0, true); write = false; break;
        case Op::ORR: r = a | b; logical = true; break;
        case Op::MOV: r = b; logical = true; break;
        case Op::BIC: r = a & ~b; logical = true; break;
        case Op::MVN: r = ~b; logical = true; break;
        default: break;
        }
        if (logical && s) {
            set_nz(r);
            c_.c = op2.carry;
        }
        if (!write) {
            if (in_.rd == 15) {
                sys_.restore_cpsr();  // legacy TSTP/CMPP: CPSR = SPSR, no branch
            }
            return;
        }
        if (in_.rd == 15) {
            if (s) {
                sys_.restore_cpsr();
            }
            branch(r);
            return;
        }
        c_.r[in_.rd] = r;
    }

    void multiply() {
        const std::uint32_t m = reg(in_.rm);
        const std::uint32_t s = reg(in_.rs);
        switch (in_.op) {
        case Op::MUL:
        case Op::MLA: {
            std::uint32_t r = m * s;
            if (in_.op == Op::MLA) {
                r += reg(in_.rn);
            }
            set_gpr(in_.rd, r);
            if (in_.s) {
                set_nz(r);
            }
            return;
        }
        default: {
            std::uint64_t r;
            if (in_.op == Op::SMULL || in_.op == Op::SMLAL) {
                r = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(m)) *
                                               static_cast<std::int32_t>(s));
            } else {
                r = static_cast<std::uint64_t>(m) * s;
            }
            if (in_.op == Op::UMLAL || in_.op == Op::SMLAL) {
                r += (static_cast<std::uint64_t>(reg(in_.rn)) << 32) | reg(in_.rd);
            }
            set_gpr(in_.rd, static_cast<std::uint32_t>(r));
            set_gpr(in_.rn, static_cast<std::uint32_t>(r >> 32));
            if (in_.s) {
                c_.n = static_cast<std::uint32_t>(r >> 63);
                c_.z = r == 0 ? 1 : 0;
            }
            return;
        }
        }
    }

    std::uint32_t load_word(std::uint32_t a) { return std::rotr(sys_.read32(a), static_cast<int>((a & 3) * 8)); }

    void swap() {
        const std::uint32_t a = reg(in_.rn);
        const std::uint32_t value = reg(in_.rm);
        std::uint32_t old;
        if (in_.op == Op::SWP) {
            old = load_word(a);
            sys_.write32(a, value);
        } else {
            old = sys_.read8(a);
            sys_.write8(a, value & 0xFF);
        }
        set_gpr(in_.rd, old);
    }

    void single_transfer() {
        const std::uint32_t base = reg(in_.rn);
        std::uint32_t offset;
        if (in_.imm) {
            offset = in_.imm_value;
        } else {
            offset = shift_by_immediate(reg(in_.rm), in_.shift, in_.shift_amount, c_.c).value;
        }
        const std::uint32_t moved = in_.up ? base + offset : base - offset;
        const std::uint32_t addr = in_.pre ? moved : base;
        const bool load = in_.op == Op::LDR || in_.op == Op::LDRB || in_.op == Op::LDRH || in_.op == Op::LDRSB ||
                          in_.op == Op::LDRSH;
        if (load) {
            std::uint32_t v = 0;
            switch (in_.op) {
            case Op::LDR: v = load_word(addr); break;
            case Op::LDRB: v = sys_.read8(addr); break;
            case Op::LDRH: v = std::rotr(sys_.read16(addr), static_cast<int>((addr & 1) * 8)); break;
            case Op::LDRSB: v = static_cast<std::uint32_t>(static_cast<std::int8_t>(sys_.read8(addr))); break;
            case Op::LDRSH:
                v = (addr & 1) ? static_cast<std::uint32_t>(static_cast<std::int8_t>(sys_.read8(addr)))
                               : static_cast<std::uint32_t>(static_cast<std::int16_t>(sys_.read16(addr)));
                break;
            default: break;
            }
            if (in_.writeback && in_.rn != 15) {
                c_.r[in_.rn] = moved;
            }
            set_reg(in_.rd, v);  // a load into the base wins over writeback
            return;
        }
        const std::uint32_t v = in_.rd == 15 ? in_.addr + 12 : c_.r[in_.rd];
        switch (in_.op) {
        case Op::STR: sys_.write32(addr, v); break;
        case Op::STRB: sys_.write8(addr, v & 0xFF); break;
        case Op::STRH: sys_.write16(addr, v & 0xFFFF); break;
        default: break;
        }
        if (in_.writeback && in_.rn != 15) {
            c_.r[in_.rn] = moved;
        }
    }

    void block_transfer() {
        const std::uint32_t base = reg(in_.rn);
        std::uint32_t list = in_.reglist;
        std::uint32_t bytes = static_cast<std::uint32_t>(std::popcount(list)) * 4;
        if (list == 0) {
            list = 1u << 15;  // ARM7TDMI: an empty list transfers PC and moves the base by 0x40
            bytes = 0x40;
        }
        std::uint32_t addr;
        if (in_.up) {
            addr = in_.pre ? base + 4 : base;
        } else {
            addr = in_.pre ? base - bytes : base - bytes + 4;
        }
        const std::uint32_t new_base = in_.up ? base + bytes : base - bytes;
        const bool has_pc = (list >> 15) & 1;

        if (in_.op == Op::STM) {
            if (in_.s) {
                anygba_stm_user(&c_, addr, list, in_.addr + 12);
            } else {
                const unsigned lowest = static_cast<unsigned>(std::countr_zero(list));
                for (unsigned r = 0; r < 16; ++r) {
                    if (!((list >> r) & 1)) {
                        continue;
                    }
                    std::uint32_t v = c_.r[r];
                    if (r == 15) {
                        v = in_.addr + (in_.thumb ? 6u : 12u);
                    } else if (r == in_.rn && in_.writeback && r != lowest) {
                        v = new_base;  // the base is written back after the first transfer
                    }
                    sys_.write32(addr, v);
                    addr += 4;
                }
            }
            if (in_.writeback) {
                set_gpr(in_.rn, new_base);
            }
            return;
        }

        if (in_.s && !has_pc) {
            anygba_ldm_user(&c_, addr, list);
            if (in_.writeback) {
                set_gpr(in_.rn, new_base);
            }
            return;
        }
        std::uint32_t values[16] = {};
        for (unsigned r = 0; r < 16; ++r) {
            if ((list >> r) & 1) {
                values[r] = sys_.read32(addr);
                addr += 4;
            }
        }
        if (in_.writeback) {
            set_gpr(in_.rn, new_base);
        }
        for (unsigned r = 0; r < 15; ++r) {
            if ((list >> r) & 1) {
                c_.r[r] = values[r];
            }
        }
        if (has_pc) {
            if (in_.s) {
                sys_.restore_cpsr();
            }
            branch(values[15]);
        }
    }

    void dispatch() {
        switch (in_.op) {
        case Op::MUL: case Op::MLA: case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL:
            multiply();
            return;
        case Op::SWP: case Op::SWPB:
            swap();
            return;
        case Op::LDR: case Op::STR: case Op::LDRB: case Op::STRB:
        case Op::LDRH: case Op::STRH: case Op::LDRSB: case Op::LDRSH:
            single_transfer();
            return;
        case Op::LDM: case Op::STM:
            block_transfer();
            return;
        case Op::B:
            branch(in_.imm_value);
            if (in_.imm_value == in_.addr) {
                sys_.idle_loop();  // branch to self: skip to the next event, as generated code does
            }
            return;
        case Op::BL:
            c_.r[14] = next_ | (in_.thumb ? 1u : 0u);
            branch(in_.imm_value);
            return;
        case Op::BX: {
            const std::uint32_t target = reg(in_.rm);
            c_.t = target & 1;
            branch(target);
            return;
        }
        case Op::BL_HI:
            c_.r[14] = in_.addr + 4 + in_.imm_value;
            return;
        case Op::BL_LO: {
            const std::uint32_t target = c_.r[14] + in_.imm_value;
            c_.r[14] = next_ | 1u;
            branch(target);
            return;
        }
        case Op::SWI:
            sys_.swi(in_.thumb ? (in_.imm_value & 0xFF) : ((in_.imm_value >> 16) & 0xFF));
            return;
        case Op::MRS:
            set_gpr(in_.rd, in_.spsr ? c_.spsr : ag_cpsr(&c_));
            return;
        case Op::MSR:
            sys_.msr(in_.imm ? in_.imm_value : reg(in_.rm), in_.psr_mask, in_.spsr);
            return;
        case Op::UNDEFINED: {
            char msg[96];
            std::snprintf(msg, sizeof msg, "undefined instruction 0x%08X at 0x%08X", in_.raw, in_.addr);
            sys_.fail(msg);
        }
        default:
            data_processing();
            return;
        }
    }

    void charge() {
        std::int32_t extra = 0;
        switch (in_.op) {
        case Op::LDR: case Op::LDRB: case Op::LDRH: case Op::LDRSB: case Op::LDRSH: extra = 1; break;
        case Op::MUL: case Op::MLA: extra = 2; break;
        case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL: extra = 3; break;
        case Op::LDM: case Op::STM: extra = std::popcount(in_.reglist); break;
        default: break;
        }
        c_.cycles -= c_.fetch_cost[in_.thumb ? 1 : 0][(in_.addr >> 24) & 15] + extra;
        if (c_.cycles <= 0) {
            sys_.tick();
        }
    }

    System& sys_;
    anygba_cpu& c_;
    const Insn& in_;
    std::uint32_t next_ = 0;
};

}  // namespace

void execute(System& sys, const Insn& in) { Executor(sys, in).run(); }

}  // namespace anygba::interp
