#include "disasm.h"

#include <array>
#include <bit>
#include <cstdio>

// Output follows Capstone's ARM syntax (UAL, hex immediates above 9) so tests can compare
// against it directly.
namespace anygba::decode {

namespace {

constexpr std::array<const char*, 16> kRegNames = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                                   "r8", "sb", "sl", "fp", "ip", "sp", "lr", "pc"};
constexpr std::array<const char*, 16> kCondNames = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                                    "hi", "ls", "ge", "lt", "gt", "le", "", "nv"};
constexpr std::array<const char*, 16> kDpNames = {"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
                                                  "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
constexpr std::array<const char*, 5> kShiftNames = {"lsl", "lsr", "asr", "ror", "rrx"};

std::string reg(unsigned r) { return kRegNames[r & 15]; }

std::string hex_or_dec(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, v > 9 ? "0x%x" : "%u", v);
    return buf;
}

std::string imm(std::uint32_t v) { return "#" + hex_or_dec(v); }

std::string signed_offset(bool up, std::uint32_t v) { return std::string("#") + (up ? "" : "-") + hex_or_dec(v); }

std::uint32_t rotr(std::uint32_t v, unsigned n) { return std::rotr(v, static_cast<int>(n)); }

// LLVM's canonical rotation choice for a modified immediate, as used by its printer to
// decide between `#value` and the explicit `#bits, #rot` form.
int canonical_mod_imm(std::uint32_t value) {
    unsigned rot = 0;
    if ((value & ~255u) != 0) {
        const unsigned rot_amt = static_cast<unsigned>(std::countr_zero(value)) & ~1u;
        rot = (32 - rot_amt) & 31;
        if ((rotr(value, rot_amt) & ~255u) != 0 && (value & 63u) != 0) {
            const unsigned rot_amt2 = static_cast<unsigned>(std::countr_zero(value & ~63u)) & ~1u;
            if ((rotr(value, rot_amt2) & ~255u) == 0) {
                rot = (32 - rot_amt2) & 31;
            }
        }
    }
    const std::uint32_t bits8 = std::rotl(value, static_cast<int>(rot));
    if (bits8 > 255) {
        return -1;
    }
    return static_cast<int>(bits8 | ((rot >> 1) << 8));
}

std::string mod_imm(const Insn& in) {
    const std::uint32_t bits8 = std::rotl(in.imm_value, in.imm_rot);
    const int encoded = static_cast<int>(bits8 | (static_cast<unsigned>(in.imm_rot >> 1) << 8));
    if (canonical_mod_imm(in.imm_value) != encoded) {
        return "#" + std::to_string(bits8) + ", #" + std::to_string(in.imm_rot);
    }
    return imm(in.imm_value);
}

std::string shifted_reg(const Insn& in) {
    std::string out = reg(in.rm);
    if (in.shift == Shift::RRX) {
        return out + ", rrx";
    }
    if (in.shift_by_reg) {
        return out + ", " + kShiftNames[static_cast<int>(in.shift)] + " " + reg(in.rs);
    }
    if (in.shift == Shift::LSL && in.shift_amount == 0) {
        return out;
    }
    return out + ", " + kShiftNames[static_cast<int>(in.shift)] + " #" + std::to_string(in.shift_amount);
}

std::string reglist(std::uint16_t list) {
    std::string out = "{";
    for (unsigned r = 0; r < 16; ++r) {
        if ((list >> r) & 1u) {
            if (out.size() > 1) {
                out += ", ";
            }
            out += reg(r);
        }
    }
    return out + "}";
}

std::string cond(const Insn& in) { return kCondNames[static_cast<int>(in.cond)]; }

std::string join(const std::string& mnemonic, const std::string& operands) {
    return operands.empty() ? mnemonic : mnemonic + " " + operands;
}

std::string arm_data_processing(const Insn& in) {
    const auto opcode = static_cast<int>(in.op);
    const bool compare = in.op == Op::TST || in.op == Op::TEQ || in.op == Op::CMP || in.op == Op::CMN;
    const std::string suffix = std::string(compare || !in.s ? "" : "s") + cond(in);
    if (in.op == Op::MOV && !in.imm && !(in.shift == Shift::LSL && !in.shift_by_reg && in.shift_amount == 0)) {
        const std::string name = kShiftNames[static_cast<int>(in.shift)];
        std::string ops = reg(in.rd) + ", " + reg(in.rm);
        if (in.shift_by_reg) {
            ops += ", " + reg(in.rs);
        } else if (in.shift != Shift::RRX) {
            ops += ", " + imm(in.shift_amount);
        }
        return join(name + suffix, ops);
    }
    const std::string op2 = in.imm ? mod_imm(in) : shifted_reg(in);
    std::string ops;
    if (compare) {
        ops = reg(in.rn) + ", " + op2;
    } else if (in.op == Op::MOV || in.op == Op::MVN) {
        ops = reg(in.rd) + ", " + op2;
    } else {
        ops = reg(in.rd) + ", " + reg(in.rn) + ", " + op2;
    }
    return join(std::string(kDpNames[opcode]) + suffix, ops);
}

std::string transfer_name(Op op) {
    switch (op) {
    case Op::LDR: return "ldr";
    case Op::STR: return "str";
    case Op::LDRB: return "ldrb";
    case Op::STRB: return "strb";
    case Op::LDRH: return "ldrh";
    case Op::STRH: return "strh";
    case Op::LDRSB: return "ldrsb";
    case Op::LDRSH: return "ldrsh";
    default: return "?";
    }
}

std::string arm_address(const Insn& in) {
    const std::string base = "[" + reg(in.rn);
    std::string offset;
    if (in.imm) {
        offset = signed_offset(in.up, in.imm_value);
    } else {
        offset = (in.up ? "" : "-") + shifted_reg(in);
    }
    if (!in.pre) {
        return base + "], " + offset;
    }
    if (in.imm && in.up && in.imm_value == 0 && !in.writeback) {
        return base + "]";
    }
    return base + ", " + offset + "]" + (in.writeback ? "!" : "");
}

std::string arm_transfer(const Insn& in) {
    if (in.op == Op::LDR && !in.pre && in.up && in.imm && in.imm_value == 4 && in.rn == kSP && !in.user) {
        return join("pop" + cond(in), "{" + reg(in.rd) + "}");
    }
    const std::string name = transfer_name(in.op) + (in.user ? "t" : "") + cond(in);
    return join(name, reg(in.rd) + ", " + arm_address(in));
}

std::string arm_block(const Insn& in) {
    const bool many = std::popcount(in.reglist) >= 2;
    if (many && !in.s && in.rn == kSP && in.writeback) {
        if (in.op == Op::LDM && !in.pre && in.up) {
            return join("pop" + cond(in), reglist(in.reglist));
        }
        if (in.op == Op::STM && in.pre && !in.up) {
            return join("push" + cond(in), reglist(in.reglist));
        }
    }
    std::string name = in.op == Op::LDM ? "ldm" : "stm";
    if (in.pre) {
        name += in.up ? "ib" : "db";
    } else if (!in.up) {
        name += "da";
    }
    std::string ops = reg(in.rn) + (in.writeback ? "!" : "") + ", " + reglist(in.reglist);
    if (in.s) {
        ops += " ^";
    }
    return join(name + cond(in), ops);
}

std::string psr_name(const Insn& in) {
    if (in.spsr) {
        std::string fields;
        if (in.psr_mask & 8) fields += 'f';
        if (in.psr_mask & 4) fields += 's';
        if (in.psr_mask & 2) fields += 'x';
        if (in.psr_mask & 1) fields += 'c';
        return fields.empty() ? "spsr" : "spsr_" + fields;
    }
    switch (in.psr_mask) {
    case 4: return "apsr_g";
    case 8: return "apsr_nzcvq";
    case 12: return "apsr_nzcvqg";
    default: break;
    }
    std::string fields;
    if (in.psr_mask & 8) fields += 'f';
    if (in.psr_mask & 4) fields += 's';
    if (in.psr_mask & 2) fields += 'x';
    if (in.psr_mask & 1) fields += 'c';
    return "cpsr_" + fields;
}

std::string disassemble_arm(const Insn& in) {
    const std::string c = cond(in);
    switch (in.op) {
    case Op::MUL:
        return join(std::string("mul") + (in.s ? "s" : "") + c, reg(in.rd) + ", " + reg(in.rm) + ", " + reg(in.rs));
    case Op::MLA:
        return join(std::string("mla") + (in.s ? "s" : "") + c,
                    reg(in.rd) + ", " + reg(in.rm) + ", " + reg(in.rs) + ", " + reg(in.rn));
    case Op::UMULL:
    case Op::UMLAL:
    case Op::SMULL:
    case Op::SMLAL: {
        static constexpr const char* names[] = {"umull", "umlal", "smull", "smlal"};
        const auto idx = static_cast<int>(in.op) - static_cast<int>(Op::UMULL);
        return join(std::string(names[idx]) + (in.s ? "s" : "") + c,
                    reg(in.rd) + ", " + reg(in.rn) + ", " + reg(in.rm) + ", " + reg(in.rs));
    }
    case Op::SWP:
    case Op::SWPB:
        return join(std::string(in.op == Op::SWP ? "swp" : "swpb") + c,
                    reg(in.rd) + ", " + reg(in.rm) + ", [" + reg(in.rn) + "]");
    case Op::LDR:
    case Op::STR:
    case Op::LDRB:
    case Op::STRB:
    case Op::LDRH:
    case Op::STRH:
    case Op::LDRSB:
    case Op::LDRSH:
        return arm_transfer(in);
    case Op::LDM:
    case Op::STM:
        return arm_block(in);
    case Op::B:
        return join("b" + c, imm(in.imm_value));
    case Op::BL:
        return join("bl" + c, imm(in.imm_value));
    case Op::BX:
        return join("bx" + c, reg(in.rm));
    case Op::SWI:
        return join("svc" + c, imm(in.imm_value));
    case Op::MRS:
        return join("mrs" + c, reg(in.rd) + ", " + (in.spsr ? "spsr" : "apsr"));
    case Op::MSR:
        return join("msr" + c, psr_name(in) + ", " + (in.imm ? mod_imm(in) : reg(in.rm)));
    case Op::UNDEFINED:
    case Op::BL_HI:
    case Op::BL_LO:
        return "<undefined>";
    default:
        return arm_data_processing(in);
    }
}

std::string thumb_address(const Insn& in) {
    if (in.thumb_format == 7 || in.thumb_format == 8) {
        return "[" + reg(in.rn) + ", " + reg(in.rm) + "]";
    }
    if (in.imm_value == 0 && in.rn != kPC) {
        return "[" + reg(in.rn) + "]";
    }
    return "[" + reg(in.rn) + ", " + imm(in.imm_value) + "]";
}

std::string disassemble_thumb(const Insn& in) {
    const std::string s = in.s ? "s" : "";
    switch (in.thumb_format) {
    case 1:
        if (in.shift == Shift::LSL && in.shift_amount == 0) {
            return join("movs", reg(in.rd) + ", " + reg(in.rm));
        }
        return join(std::string(kShiftNames[static_cast<int>(in.shift)]) + "s",
                    reg(in.rd) + ", " + reg(in.rm) + ", " + imm(in.shift_amount));
    case 2:
        return join(std::string(kDpNames[static_cast<int>(in.op)]) + "s",
                    reg(in.rd) + ", " + reg(in.rn) + ", " + (in.imm ? imm(in.imm_value) : reg(in.rm)));
    case 3:
        if (in.op == Op::CMP) {
            return join("cmp", reg(in.rn) + ", " + imm(in.imm_value));
        }
        return join(std::string(kDpNames[static_cast<int>(in.op)]) + "s", reg(in.rd) + ", " + imm(in.imm_value));
    case 4:
        if (in.op == Op::MOV) {
            return join(std::string(kShiftNames[static_cast<int>(in.shift)]) + "s", reg(in.rd) + ", " + reg(in.rs));
        }
        if (in.op == Op::RSB) {
            return join("rsbs", reg(in.rd) + ", " + reg(in.rn) + ", #0");
        }
        if (in.op == Op::MUL) {
            return join("muls", reg(in.rd) + ", " + reg(in.rm) + ", " + reg(in.rd));
        }
        if (in.op == Op::TST || in.op == Op::CMP || in.op == Op::CMN) {
            return join(kDpNames[static_cast<int>(in.op)], reg(in.rn) + ", " + reg(in.rm));
        }
        return join(std::string(kDpNames[static_cast<int>(in.op)]) + "s", reg(in.rd) + ", " + reg(in.rm));
    case 5:
        switch (in.op) {
        case Op::ADD:
            if (in.rm == kSP) {
                return join("add", reg(in.rd) + ", sp, " + reg(in.rd));
            }
            return join("add", reg(in.rd) + ", " + reg(in.rm));
        case Op::CMP: return join("cmp", reg(in.rn) + ", " + reg(in.rm));
        case Op::MOV: return join("mov", reg(in.rd) + ", " + reg(in.rm));
        case Op::BX: return join("bx", reg(in.rm));
        default: return "<undefined>";
        }
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
        return join(transfer_name(in.op), reg(in.rd) + ", " + thumb_address(in));
    case 12:
        if (in.rn == kPC) {
            return join("adr", reg(in.rd) + ", " + imm(in.imm_value));
        }
        return join("add", reg(in.rd) + ", sp, " + imm(in.imm_value));
    case 13:
        return join(in.op == Op::ADD ? "add" : "sub", "sp, " + imm(in.imm_value));
    case 14:
        return join(in.op == Op::LDM ? "pop" : "push", reglist(in.reglist));
    case 15:
        return join(in.op == Op::LDM ? "ldm" : "stm",
                    reg(in.rn) + (in.writeback ? "!" : "") + ", " + reglist(in.reglist));
    case 16:
        return join("b" + cond(in), imm(in.imm_value));
    case 17:
        return join("svc", imm(in.imm_value));
    case 18:
        return join("b", imm(in.imm_value));
    case 19:
        if (in.op == Op::BL) {
            return join("bl", imm(in.imm_value));
        }
        if (in.op == Op::BL_HI) {
            return join("bl.hi", "#" + std::to_string(static_cast<std::int32_t>(in.imm_value)));
        }
        return join("bl.lo", imm(in.imm_value));
    default:
        return "<undefined>";
    }
}

}  // namespace

std::string disassemble(const Insn& in) {
    if (!in.valid()) {
        return "<undefined>";
    }
    return in.thumb ? disassemble_thumb(in) : disassemble_arm(in);
}

}  // namespace anygba::decode
