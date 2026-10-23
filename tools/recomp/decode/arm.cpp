#include "arm.h"

namespace anygba::decode {

namespace {

constexpr std::uint32_t bits(std::uint32_t w, int hi, int lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}
constexpr bool bit(std::uint32_t w, int n) { return ((w >> n) & 1u) != 0; }
constexpr std::uint8_t reg(std::uint32_t w, int lo) { return static_cast<std::uint8_t>(bits(w, lo + 3, lo)); }

void decode_shifted_register(Insn& in, std::uint32_t w) {
    in.rm = reg(w, 0);
    in.shift = static_cast<Shift>(bits(w, 6, 5));
    if (bit(w, 4)) {
        in.shift_by_reg = true;
        in.rs = reg(w, 8);
        return;
    }
    auto amount = static_cast<std::uint8_t>(bits(w, 11, 7));
    if (amount == 0) {
        if (in.shift == Shift::LSR || in.shift == Shift::ASR) {
            amount = 32;
        } else if (in.shift == Shift::ROR) {
            in.shift = Shift::RRX;
        }
    }
    in.shift_amount = amount;
}

void decode_data_processing(Insn& in, std::uint32_t w) {
    in.op = static_cast<Op>(bits(w, 24, 21));
    in.s = bit(w, 20);
    in.rn = reg(w, 16);
    in.rd = reg(w, 12);
    if (bit(w, 25)) {
        in.imm = true;
        in.imm_rot = static_cast<std::uint8_t>(bits(w, 11, 8) * 2);
        const std::uint32_t v = bits(w, 7, 0);
        in.imm_value = in.imm_rot ? (v >> in.imm_rot) | (v << (32 - in.imm_rot)) : v;
    } else {
        decode_shifted_register(in, w);
    }
}

void decode_psr_or_misc(Insn& in, std::uint32_t w) {
    if ((w & 0x0FFFFFF0u) == 0x012FFF10u) {
        in.op = Op::BX;
        in.rm = reg(w, 0);
    } else if ((w & 0x0FB000F0u) == 0x01000000u) {
        // MRS; ARM7TDMI ignores the SBO/SBZ fields.
        in.op = Op::MRS;
        in.rd = reg(w, 12);
        in.spsr = bit(w, 22);
    } else if ((w & 0x0FB0FFF0u) == 0x0120F000u) {
        in.op = Op::MSR;
        in.spsr = bit(w, 22);
        in.psr_mask = static_cast<std::uint8_t>(bits(w, 19, 16));
        in.rm = reg(w, 0);
    }
}

void decode_halfword_transfer(Insn& in, std::uint32_t w) {
    const bool load = bit(w, 20);
    const std::uint32_t sh = bits(w, 6, 5);
    if (!load && sh != 1) {
        return;  // LDRD/STRD (ARMv5TE)
    }
    if (!bit(w, 24) && bit(w, 21)) {
        return;  // post-indexed with W set is unpredictable
    }
    in.op = !load ? Op::STRH : sh == 1 ? Op::LDRH : sh == 2 ? Op::LDRSB : Op::LDRSH;
    in.pre = bit(w, 24);
    in.up = bit(w, 23);
    in.writeback = in.pre ? bit(w, 21) : true;
    in.rn = reg(w, 16);
    in.rd = reg(w, 12);
    if (bit(w, 22)) {
        in.imm = true;
        in.imm_value = (bits(w, 11, 8) << 4) | bits(w, 3, 0);
    } else {
        in.rm = reg(w, 0);
    }
}

void decode_single_transfer(Insn& in, std::uint32_t w) {
    const bool load = bit(w, 20);
    const bool byte = bit(w, 22);
    in.op = load ? (byte ? Op::LDRB : Op::LDR) : (byte ? Op::STRB : Op::STR);
    in.pre = bit(w, 24);
    in.up = bit(w, 23);
    in.writeback = in.pre ? bit(w, 21) : true;
    in.user = !in.pre && bit(w, 21);
    in.rn = reg(w, 16);
    in.rd = reg(w, 12);
    if (!bit(w, 25)) {
        in.imm = true;
        in.imm_value = bits(w, 11, 0);
    } else {
        decode_shifted_register(in, w);
    }
}

}  // namespace

Insn decode_arm(std::uint32_t w, std::uint32_t addr) {
    Insn in;
    in.addr = addr;
    in.raw = w;
    in.size = 4;
    in.cond = static_cast<Cond>(bits(w, 31, 28));
    if (in.cond == Cond::NV) {
        return in;
    }

    switch (bits(w, 27, 25)) {
    case 0b000:
        if ((w & 0x0FB000F0u) == 0x01000090u) {
            in.op = bit(w, 22) ? Op::SWPB : Op::SWP;
            in.rn = reg(w, 16);
            in.rd = reg(w, 12);
            in.rm = reg(w, 0);
        } else if ((w & 0x0FC000F0u) == 0x00000090u) {
            in.op = bit(w, 21) ? Op::MLA : Op::MUL;
            in.s = bit(w, 20);
            in.rd = reg(w, 16);
            in.rn = reg(w, 12);
            in.rs = reg(w, 8);
            in.rm = reg(w, 0);
        } else if ((w & 0x0F8000F0u) == 0x00800090u) {
            const bool sign = bit(w, 22);
            const bool acc = bit(w, 21);
            in.op = sign ? (acc ? Op::SMLAL : Op::SMULL) : (acc ? Op::UMLAL : Op::UMULL);
            in.s = bit(w, 20);
            in.rn = reg(w, 16);
            in.rd = reg(w, 12);
            in.rs = reg(w, 8);
            in.rm = reg(w, 0);
        } else if ((w & 0x90u) == 0x90u) {
            if ((w & 0x60u) != 0) {
                decode_halfword_transfer(in, w);
            }
        } else if (bits(w, 24, 23) == 0b10 && !bit(w, 20)) {
            decode_psr_or_misc(in, w);
        } else {
            decode_data_processing(in, w);
        }
        break;
    case 0b001:
        if (bits(w, 24, 23) == 0b10 && !bit(w, 20)) {
            // With no fields selected this is a no-op on ARM7TDMI (a hint on later cores).
            if ((w & 0x0FB0F000u) == 0x0320F000u) {
                in.op = Op::MSR;
                in.spsr = bit(w, 22);
                in.psr_mask = static_cast<std::uint8_t>(bits(w, 19, 16));
                in.imm = true;
                in.imm_rot = static_cast<std::uint8_t>(bits(w, 11, 8) * 2);
                const std::uint32_t v = bits(w, 7, 0);
                in.imm_value = in.imm_rot ? (v >> in.imm_rot) | (v << (32 - in.imm_rot)) : v;
            }
        } else {
            decode_data_processing(in, w);
        }
        break;
    case 0b010:
        decode_single_transfer(in, w);
        break;
    case 0b011:
        if (!bit(w, 4)) {
            decode_single_transfer(in, w);
        }
        break;
    case 0b100:
        in.op = bit(w, 20) ? Op::LDM : Op::STM;
        in.pre = bit(w, 24);
        in.up = bit(w, 23);
        in.s = bit(w, 22);
        in.writeback = bit(w, 21);
        in.rn = reg(w, 16);
        in.reglist = static_cast<std::uint16_t>(bits(w, 15, 0));
        break;
    case 0b101: {
        in.op = bit(w, 24) ? Op::BL : Op::B;
        const auto offset = static_cast<std::int32_t>(w << 8) >> 6;
        in.imm = true;
        in.imm_value = addr + 8 + static_cast<std::uint32_t>(offset);
        break;
    }
    case 0b110:
        break;  // LDC/STC: the GBA has no coprocessor
    case 0b111:
        if (bit(w, 24)) {
            in.op = Op::SWI;
            in.imm = true;
            in.imm_value = bits(w, 23, 0);
        }
        break;
    default:
        break;
    }
    return in;
}

}  // namespace anygba::decode
