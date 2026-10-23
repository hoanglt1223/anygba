#include "thumb.h"

namespace anygba::decode {

namespace {

constexpr std::uint32_t bits(std::uint32_t w, int hi, int lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}
constexpr bool bit(std::uint32_t w, int n) { return ((w >> n) & 1u) != 0; }
constexpr std::uint8_t reg(std::uint32_t w, int lo) { return static_cast<std::uint8_t>(bits(w, lo + 2, lo)); }

Insn base(std::uint16_t h, std::uint32_t addr, std::uint8_t format) {
    Insn in;
    in.addr = addr;
    in.raw = h;
    in.size = 2;
    in.thumb = true;
    in.thumb_format = format;
    return in;
}

void set_imm(Insn& in, std::uint32_t value) {
    in.imm = true;
    in.imm_value = value;
}

// Format 4 ALU operations mapped onto ARM data processing.
void decode_alu(Insn& in, std::uint16_t h) {
    const std::uint8_t rd = reg(h, 0);
    const std::uint8_t rs = reg(h, 3);
    in.s = true;
    in.rd = rd;
    in.rn = rd;
    in.rm = rs;
    const auto shift_by = [&](Shift sh) {
        in.op = Op::MOV;
        in.rm = rd;
        in.shift = sh;
        in.shift_by_reg = true;
        in.rs = rs;
    };
    switch (bits(h, 9, 6)) {
    case 0x0: in.op = Op::AND; break;
    case 0x1: in.op = Op::EOR; break;
    case 0x2: shift_by(Shift::LSL); break;
    case 0x3: shift_by(Shift::LSR); break;
    case 0x4: shift_by(Shift::ASR); break;
    case 0x5: in.op = Op::ADC; break;
    case 0x6: in.op = Op::SBC; break;
    case 0x7: shift_by(Shift::ROR); break;
    case 0x8: in.op = Op::TST; break;
    case 0x9:
        in.op = Op::RSB;
        in.rn = rs;
        set_imm(in, 0);
        break;
    case 0xA: in.op = Op::CMP; break;
    case 0xB: in.op = Op::CMN; break;
    case 0xC: in.op = Op::ORR; break;
    case 0xD:
        in.op = Op::MUL;
        in.rm = rs;
        in.rs = rd;
        break;
    case 0xE: in.op = Op::BIC; break;
    case 0xF: in.op = Op::MVN; break;
    default: break;
    }
}

void decode_hi_register(Insn& in, std::uint16_t h) {
    const auto rd = static_cast<std::uint8_t>(reg(h, 0) | (bit(h, 7) ? 8 : 0));
    const auto rs = static_cast<std::uint8_t>(bits(h, 6, 3));
    switch (bits(h, 9, 8)) {
    case 0:
        in.op = Op::ADD;
        in.rd = rd;
        in.rn = rd;
        in.rm = rs;
        break;
    case 1:
        in.op = Op::CMP;
        in.s = true;
        in.rn = rd;
        in.rm = rs;
        break;
    case 2:
        in.op = Op::MOV;
        in.rd = rd;
        in.rm = rs;
        break;
    case 3:
        if (bit(h, 7)) {
            return;  // BLX (ARMv5)
        }
        in.op = Op::BX;
        in.rm = rs;
        break;
    default:
        break;
    }
}

void decode_load_store_register(Insn& in, std::uint16_t h) {
    static constexpr Op format7[4] = {Op::STR, Op::STRB, Op::LDR, Op::LDRB};
    static constexpr Op format8[4] = {Op::STRH, Op::LDRSB, Op::LDRH, Op::LDRSH};
    const std::uint32_t idx = bits(h, 11, 10);
    in.thumb_format = bit(h, 9) ? 8 : 7;
    in.op = bit(h, 9) ? format8[idx] : format7[idx];
    in.rd = reg(h, 0);
    in.rn = reg(h, 3);
    in.rm = reg(h, 6);
}

}  // namespace

Insn decode_thumb(std::uint16_t h, std::uint32_t addr) {
    Insn in = base(h, addr, 0);
    switch (bits(h, 15, 13)) {
    case 0b000:
        if (bits(h, 12, 11) == 0b11) {
            in.thumb_format = 2;
            in.op = bit(h, 9) ? Op::SUB : Op::ADD;
            in.s = true;
            in.rd = reg(h, 0);
            in.rn = reg(h, 3);
            if (bit(h, 10)) {
                set_imm(in, bits(h, 8, 6));
            } else {
                in.rm = reg(h, 6);
            }
        } else {
            in.thumb_format = 1;
            in.op = Op::MOV;
            in.s = true;
            in.rd = reg(h, 0);
            in.rm = reg(h, 3);
            in.shift = static_cast<Shift>(bits(h, 12, 11));
            auto amount = static_cast<std::uint8_t>(bits(h, 10, 6));
            if (amount == 0 && in.shift != Shift::LSL) {
                amount = 32;
            }
            in.shift_amount = amount;
        }
        break;
    case 0b001: {
        static constexpr Op ops[4] = {Op::MOV, Op::CMP, Op::ADD, Op::SUB};
        in.thumb_format = 3;
        in.op = ops[bits(h, 12, 11)];
        in.s = true;
        in.rd = reg(h, 8);
        in.rn = in.rd;
        set_imm(in, bits(h, 7, 0));
        break;
    }
    case 0b010:
        if (bits(h, 12, 10) == 0b000) {
            in.thumb_format = 4;
            decode_alu(in, h);
        } else if (bits(h, 12, 10) == 0b001) {
            in.thumb_format = 5;
            decode_hi_register(in, h);
        } else if (bits(h, 12, 11) == 0b01) {
            in.thumb_format = 6;
            in.op = Op::LDR;
            in.rd = reg(h, 8);
            in.rn = kPC;
            in.pc_align = true;
            set_imm(in, bits(h, 7, 0) << 2);
        } else {
            decode_load_store_register(in, h);
        }
        break;
    case 0b011: {
        static constexpr Op ops[4] = {Op::STR, Op::LDR, Op::STRB, Op::LDRB};
        in.thumb_format = 9;
        in.op = ops[bits(h, 12, 11)];
        in.rd = reg(h, 0);
        in.rn = reg(h, 3);
        const bool byte = bit(h, 12);
        set_imm(in, bits(h, 10, 6) << (byte ? 0 : 2));
        break;
    }
    case 0b100:
        in.rd = bit(h, 12) ? reg(h, 8) : reg(h, 0);
        if (!bit(h, 12)) {
            in.thumb_format = 10;
            in.op = bit(h, 11) ? Op::LDRH : Op::STRH;
            in.rn = reg(h, 3);
            set_imm(in, bits(h, 10, 6) << 1);
        } else {
            in.thumb_format = 11;
            in.op = bit(h, 11) ? Op::LDR : Op::STR;
            in.rn = kSP;
            set_imm(in, bits(h, 7, 0) << 2);
        }
        break;
    case 0b101:
        if (!bit(h, 12)) {
            in.thumb_format = 12;
            in.op = Op::ADD;
            in.rd = reg(h, 8);
            in.rn = bit(h, 11) ? kSP : kPC;
            in.pc_align = !bit(h, 11);
            set_imm(in, bits(h, 7, 0) << 2);
        } else if (bits(h, 11, 8) == 0b0000) {
            in.thumb_format = 13;
            in.op = bit(h, 7) ? Op::SUB : Op::ADD;
            in.rd = kSP;
            in.rn = kSP;
            set_imm(in, bits(h, 6, 0) << 2);
        } else if (bits(h, 10, 9) == 0b10) {
            in.thumb_format = 14;
            const bool pop = bit(h, 11);
            auto list = static_cast<std::uint16_t>(bits(h, 7, 0));
            if (bit(h, 8)) {
                list = static_cast<std::uint16_t>(list | (pop ? 1u << kPC : 1u << kLR));
            }
            in.op = pop ? Op::LDM : Op::STM;
            in.rn = kSP;
            in.pre = !pop;
            in.up = pop;
            in.writeback = true;
            in.reglist = list;
        }
        break;
    case 0b110:
        if (!bit(h, 12)) {
            in.thumb_format = 15;
            in.op = bit(h, 11) ? Op::LDM : Op::STM;
            in.rn = reg(h, 8);
            in.pre = false;
            in.up = true;
            in.reglist = static_cast<std::uint16_t>(bits(h, 7, 0));
            // A Thumb LDMIA whose base is in the list keeps the loaded value.
            in.writeback = !(in.op == Op::LDM && (in.reglist >> in.rn) & 1u);
        } else if (bits(h, 11, 8) == 0xF) {
            in.thumb_format = 17;
            in.op = Op::SWI;
            set_imm(in, bits(h, 7, 0));
        } else if (bits(h, 11, 8) != 0xE) {
            in.thumb_format = 16;
            in.op = Op::B;
            in.cond = static_cast<Cond>(bits(h, 11, 8));
            const auto offset = static_cast<std::int32_t>(static_cast<std::int8_t>(bits(h, 7, 0))) * 2;
            set_imm(in, addr + 4 + static_cast<std::uint32_t>(offset));
        }
        break;
    case 0b111: {
        const std::uint32_t op = bits(h, 12, 11);
        if (op == 0b00) {
            in.thumb_format = 18;
            in.op = Op::B;
            const std::int32_t offset = (static_cast<std::int32_t>(bits(h, 10, 0) << 21) >> 20);
            set_imm(in, addr + 4 + static_cast<std::uint32_t>(offset));
        } else if (op == 0b10) {
            in.thumb_format = 19;
            in.op = Op::BL_HI;
            const std::int32_t offset = (static_cast<std::int32_t>(bits(h, 10, 0) << 21) >> 9);
            set_imm(in, static_cast<std::uint32_t>(offset));
        } else if (op == 0b11) {
            in.thumb_format = 19;
            in.op = Op::BL_LO;
            set_imm(in, bits(h, 10, 0) << 1);
        }
        break;  // op == 0b01 is the ARMv5 BLX suffix
    }
    default:
        break;
    }
    return in;
}

Insn decode_thumb_at(std::uint16_t h, std::uint16_t next, std::uint32_t addr) {
    Insn in = decode_thumb(h, addr);
    if (in.op != Op::BL_HI || (next & 0xF800u) != 0xF800u) {
        return in;
    }
    const std::uint32_t target = addr + 4 + in.imm_value + ((next & 0x7FFu) << 1);
    in.op = Op::BL;
    in.size = 4;
    in.raw = h | (static_cast<std::uint32_t>(next) << 16);
    in.imm_value = target;
    return in;
}

}  // namespace anygba::decode
