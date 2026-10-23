#include "codegen/function_emitter.h"

#include <bit>

// C for the shared instruction representation: data processing, multiplies, transfers,
// PSR access and branches. Thumb instructions arrive here as their ARM equivalents.
namespace anygba::codegen::detail {

using decode::Insn;
using decode::kLR;
using decode::kPC;
using decode::Op;
using decode::Shift;

// Writes to R15 that ARMv4T leaves UNPREDICTABLE (multiplies, SWP, MRS, block writeback)
// are dropped, in step with the reference interpreter.
static std::string gpr_write(unsigned r, const std::string& value) {
    if (r == kPC) {
        return "(void)(" + value + ");";
    }
    return "c->r[" + std::to_string(r) + "] = " + value + ";";
}

std::string FunctionEmitter::reg(const Insn& in, unsigned r) const {
    if (r != kPC) {
        return "c->r[" + std::to_string(r) + "]";
    }
    std::uint32_t pc = in.pc_value();
    if (!in.thumb && in.shift_by_reg) {
        pc += 4;  // the PC is read a cycle later when Rs specifies the shift
    }
    return hex(in.pc_align ? pc & ~3u : pc);
}

void FunctionEmitter::emit_operand2(const Insn& in, bool carry_out) {
    if (in.imm) {
        line("const uint32_t b = %s;", hex(in.imm_value).c_str());
        if (carry_out) {
            line("const uint32_t sc = %s;", in.imm_rot ? (in.imm_value >> 31 ? "1u" : "0u") : "c->c");
        }
        return;
    }
    const std::string m = reg(in, in.rm);
    if (in.shift_by_reg) {
        static constexpr const char* names[] = {"lsl", "lsr", "asr", "ror", "ror"};
        const char* name = names[static_cast<int>(in.shift)];
        line("const uint32_t amount = %s & 0xFFu;", reg(in, in.rs).c_str());
        if (carry_out) {
            line("uint32_t sc = c->c;");
            line("const uint32_t b = ag_%s_c(%s, amount, &sc);", name, m.c_str());
        } else {
            line("const uint32_t b = ag_%s(%s, amount);", name, m.c_str());
        }
        return;
    }
    const unsigned k = in.shift_amount;
    std::string value;
    std::string carry;
    switch (in.shift) {
    case Shift::LSL:
        if (k == 0) {
            value = m;
            carry = "c->c";
        } else {
            value = "(" + m + " << " + std::to_string(k) + ")";
            carry = "((" + m + " >> " + std::to_string(32 - k) + ") & 1u)";
        }
        break;
    case Shift::LSR:
        if (k == 32) {
            value = "0u";
            carry = "(" + m + " >> 31)";
        } else {
            value = "(" + m + " >> " + std::to_string(k) + ")";
            carry = "((" + m + " >> " + std::to_string(k - 1) + ") & 1u)";
        }
        break;
    case Shift::ASR:
        if (k == 32) {
            value = "(uint32_t)((int32_t)" + m + " >> 31)";
            carry = "(" + m + " >> 31)";
        } else {
            value = "(uint32_t)((int32_t)" + m + " >> " + std::to_string(k) + ")";
            carry = "((" + m + " >> " + std::to_string(k - 1) + ") & 1u)";
        }
        break;
    case Shift::ROR:
        value = "ag_rotr(" + m + ", " + std::to_string(k) + ")";
        carry = "((" + m + " >> " + std::to_string(k - 1) + ") & 1u)";
        break;
    case Shift::RRX:
        value = "((c->c << 31) | (" + m + " >> 1))";
        carry = "(" + m + " & 1u)";
        break;
    }
    line("const uint32_t b = %s;", value.c_str());
    if (carry_out) {
        line("const uint32_t sc = %s;", carry.c_str());
    }
}

void FunctionEmitter::emit_data_processing(const Insn& in) {
    const bool logical = in.op == Op::AND || in.op == Op::EOR || in.op == Op::TST || in.op == Op::TEQ ||
                         in.op == Op::ORR || in.op == Op::MOV || in.op == Op::BIC || in.op == Op::MVN;
    const bool compare = in.op == Op::TST || in.op == Op::TEQ || in.op == Op::CMP || in.op == Op::CMN;
    const bool s = in.s || compare;
    const bool uses_a = in.op != Op::MOV && in.op != Op::MVN;
    line("{");
    ++indent_;
    if (uses_a) {
        line("const uint32_t a = %s;", reg(in, in.rn).c_str());
    }
    emit_operand2(in, logical && s);
    const char* expr = nullptr;
    std::string flags_expr;
    switch (in.op) {
    case Op::AND: case Op::TST: expr = "a & b"; break;
    case Op::EOR: case Op::TEQ: expr = "a ^ b"; break;
    case Op::ORR: expr = "a | b"; break;
    case Op::MOV: expr = "b"; break;
    case Op::BIC: expr = "a & ~b"; break;
    case Op::MVN: expr = "~b"; break;
    case Op::SUB: case Op::CMP: expr = s ? "ag_adc_flags(c, a, ~b, 1u)" : "a - b"; break;
    case Op::RSB: expr = s ? "ag_adc_flags(c, b, ~a, 1u)" : "b - a"; break;
    case Op::ADD: case Op::CMN: expr = s ? "ag_adc_flags(c, a, b, 0u)" : "a + b"; break;
    case Op::ADC: expr = s ? "ag_adc_flags(c, a, b, c->c)" : "a + b + c->c"; break;
    case Op::SBC: expr = s ? "ag_adc_flags(c, a, ~b, c->c)" : "a + ~b + c->c"; break;
    case Op::RSC: expr = s ? "ag_adc_flags(c, b, ~a, c->c)" : "b + ~a + c->c"; break;
    default: break;
    }
    line("const uint32_t r = %s;", expr);
    if (logical && s) {
        line("ag_nz(c, r);");
        line("c->c = sc;");
    }
    if (compare) {
        line("(void)r;");
        if (in.rd == kPC) {
            line("anygba_restore_cpsr(c);");  // legacy TSTP/CMPP: CPSR = SPSR, no branch
        }
    } else if (in.rd != kPC) {
        line("c->r[%u] = r;", in.rd);
    } else {
        if (in.s) {
            line("anygba_restore_cpsr(c);");
        }
        line("c->r[15] = r & (c->t ? ~1u : ~3u);");
        emit_indirect_exit(in);
    }
    --indent_;
    line("}");
}

void FunctionEmitter::emit_multiply(const Insn& in) {
    line("{");
    ++indent_;
    if (in.op == Op::MUL || in.op == Op::MLA) {
        if (in.op == Op::MLA) {
            line("const uint32_t r = %s * %s + %s;", reg(in, in.rm).c_str(), reg(in, in.rs).c_str(),
                 reg(in, in.rn).c_str());
        } else {
            line("const uint32_t r = %s * %s;", reg(in, in.rm).c_str(), reg(in, in.rs).c_str());
        }
        line("%s", gpr_write(in.rd, "r").c_str());
        if (in.s) {
            line("ag_nz(c, r);");
        }
    } else {
        const bool sign = in.op == Op::SMULL || in.op == Op::SMLAL;
        const std::string m = reg(in, in.rm);
        const std::string s = reg(in, in.rs);
        if (sign) {
            line("uint64_t r = (uint64_t)((int64_t)(int32_t)%s * (int32_t)%s);", m.c_str(), s.c_str());
        } else {
            line("uint64_t r = (uint64_t)%s * %s;", m.c_str(), s.c_str());
        }
        if (in.op == Op::UMLAL || in.op == Op::SMLAL) {
            line("r += ((uint64_t)%s << 32) | %s;", reg(in, in.rn).c_str(), reg(in, in.rd).c_str());
        }
        line("%s", gpr_write(in.rd, "(uint32_t)r").c_str());
        line("%s", gpr_write(in.rn, "(uint32_t)(r >> 32)").c_str());
        if (in.s) {
            line("c->n = (uint32_t)(r >> 63);");
            line("c->z = r == 0;");
        }
    }
    --indent_;
    line("}");
}

void FunctionEmitter::emit_swap(const Insn& in) {
    line("{");
    ++indent_;
    line("const uint32_t a = %s;", reg(in, in.rn).c_str());
    line("const uint32_t v = %s;", reg(in, in.rm).c_str());
    if (in.op == Op::SWP) {
        line("const uint32_t old = ag_ldr(c, a);");
        line("ag_wr32(c, a, v);");
    } else {
        line("const uint32_t old = ag_rd8(c, a);");
        line("ag_wr8(c, a, v);");
    }
    line("%s", gpr_write(in.rd, "old").c_str());
    --indent_;
    line("}");
}

void FunctionEmitter::emit_offset(const Insn& in) {
    if (in.imm) {
        line("const uint32_t off = %s;", hex(in.imm_value).c_str());
        return;
    }
    Insn shifted = in;
    shifted.imm = false;
    emit_operand2(shifted, false);
    line("const uint32_t off = b;");
}

void FunctionEmitter::emit_single_transfer(const Insn& in) {
    const bool load = in.op == Op::LDR || in.op == Op::LDRB || in.op == Op::LDRH || in.op == Op::LDRSB ||
                      in.op == Op::LDRSH;
    // A PC-relative word load from ROM reads a constant: fold it.
    if (in.op == Op::LDR && in.rn == kPC && in.imm && options_.image && in.rd != kPC) {
        const std::uint32_t base = in.pc_align ? in.pc_value() & ~3u : in.pc_value();
        const std::uint32_t addr = in.up ? base + in.imm_value : base - in.imm_value;
        if (analysis::Image::is_rom(addr) && (addr & 3) == 0) {
            if (const auto value = options_.image->read32(addr)) {
                line("c->r[%u] = %s;", in.rd, hex(*value).c_str());
                return;
            }
        }
    }
    line("{");
    ++indent_;
    line("const uint32_t base = %s;", reg(in, in.rn).c_str());
    emit_offset(in);
    line("const uint32_t moved = base %s off;", in.up ? "+" : "-");
    line("const uint32_t a = %s;", in.pre ? "moved" : "base");
    line("(void)moved;");
    const bool writeback = in.writeback && in.rn != kPC;
    if (load) {
        const char* fn = "ag_ldr";
        switch (in.op) {
        case Op::LDRB: fn = "ag_rd8"; break;
        case Op::LDRH: fn = "ag_ldrh"; break;
        case Op::LDRSB: fn = "ag_ldrsb"; break;
        case Op::LDRSH: fn = "ag_ldrsh"; break;
        default: break;
        }
        line("const uint32_t v = %s(c, a);", fn);
        if (writeback) {
            line("c->r[%u] = moved;", in.rn);
        }
        if (in.rd == kPC) {
            line("c->r[15] = v & (c->t ? ~1u : ~3u);");
            emit_indirect_exit(in);
        } else {
            line("c->r[%u] = v;", in.rd);
        }
    } else {
        line("const uint32_t v = %s;", in.rd == kPC ? hex(in.addr + 12).c_str() : reg(in, in.rd).c_str());
        const char* fn = in.op == Op::STR ? "ag_wr32" : in.op == Op::STRB ? "ag_wr8" : "ag_wr16";
        line("%s(c, a, v);", fn);
        if (writeback) {
            line("c->r[%u] = moved;", in.rn);
        }
    }
    --indent_;
    line("}");
}

void FunctionEmitter::emit_block_transfer(const Insn& in) {
    std::uint32_t list = in.reglist;
    std::uint32_t bytes = static_cast<std::uint32_t>(std::popcount(list)) * 4;
    if (list == 0) {
        list = 1u << kPC;  // ARM7TDMI: empty list transfers PC, base moves by 0x40
        bytes = 0x40;
    }
    const bool has_pc = (list >> kPC) & 1u;
    line("{");
    ++indent_;
    line("const uint32_t base = %s;", reg(in, in.rn).c_str());
    if (in.up) {
        line("uint32_t a = base%s;", in.pre ? " + 4u" : "");
    } else {
        line("uint32_t a = base - %uu;", in.pre ? bytes : bytes - 4);
    }
    line("const uint32_t new_base = base %s %uu;", in.up ? "+" : "-", bytes);
    line("(void)new_base;");

    if (in.op == Op::STM) {
        if (in.s) {
            line("anygba_stm_user(c, a, %uu, %s);", list, hex(in.addr + 12).c_str());
        } else {
            const unsigned lowest = static_cast<unsigned>(std::countr_zero(list));
            for (unsigned r = 0; r < 16; ++r) {
                if (!((list >> r) & 1u)) {
                    continue;
                }
                std::string value = "c->r[" + std::to_string(r) + "]";
                if (r == kPC) {
                    value = hex(in.addr + (in.thumb ? 6u : 12u));
                } else if (r == in.rn && in.writeback && r != lowest) {
                    value = "new_base";
                }
                line("ag_wr32(c, a, %s);", value.c_str());
                line("a += 4u;");
            }
        }
        if (in.writeback) {
            line("%s", gpr_write(in.rn, "new_base").c_str());
        }
        line("(void)a;");
    } else if (in.s && !has_pc) {
        line("anygba_ldm_user(c, a, %uu);", list);
        if (in.writeback) {
            line("%s", gpr_write(in.rn, "new_base").c_str());
        }
    } else {
        for (unsigned r = 0; r < 16; ++r) {
            if ((list >> r) & 1u) {
                line("const uint32_t v%u = ag_rd32(c, a);", r);
                line("a += 4u;");
            }
        }
        line("(void)a;");
        if (in.writeback) {
            line("%s", gpr_write(in.rn, "new_base").c_str());
        }
        for (unsigned r = 0; r < 15; ++r) {
            if ((list >> r) & 1u) {
                line("c->r[%u] = v%u;", r, r);
            }
        }
        if (has_pc) {
            if (in.s) {
                line("anygba_restore_cpsr(c);");
            }
            line("c->r[15] = v15 & (c->t ? ~1u : ~3u);");
            emit_indirect_exit(in);
        }
    }
    --indent_;
    line("}");
}

void FunctionEmitter::emit_psr(const Insn& in) {
    if (in.op == Op::MRS) {
        line("%s", gpr_write(in.rd, in.spsr ? "c->spsr" : "ag_cpsr(c)").c_str());
        return;
    }
    const std::string value = in.imm ? hex(in.imm_value) : reg(in, in.rm);
    line("anygba_msr(c, %s, %uu, %uu);", value.c_str(), in.psr_mask, in.spsr ? 1u : 0u);
}

void FunctionEmitter::emit_branch(const Insn& in) {
    switch (in.op) {
    case Op::B:
        emit_jump_to(in, in.imm_value);
        return;
    case Op::BL: {
        const std::uint32_t ret = in.next();
        line("c->r[14] = %s;", hex(ret | (in.thumb ? 1u : 0u)).c_str());
        bool direct = false;
        if (const auto* functions = options_.functions) {
            const auto callee = functions->find(in.imm_value);
            direct = callee != functions->end() && callee->second.mode == fn_.mode;
        }
        if (direct) {
            line("AG_CALL(c, %s, %s);", function_name(in.imm_value).c_str(), hex(ret).c_str());
        } else {
            line("c->r[15] = %s;", hex(in.imm_value).c_str());
            line("AG_ICALL(c, %s);", hex(ret).c_str());
        }
        return;
    }
    case Op::BX:
        line("{");
        ++indent_;
        line("const uint32_t target = %s;", reg(in, in.rm).c_str());
        line("c->t = target & 1u;");
        line("c->r[15] = target & (c->t ? ~1u : ~3u);");
        emit_indirect_exit(in);
        --indent_;
        line("}");
        return;
    case Op::SWI:
        line("c->r[15] = %s;", hex(in.next()).c_str());
        line("anygba_swi(c, %uu);", in.thumb ? (in.imm_value & 0xFF) : ((in.imm_value >> 16) & 0xFF));
        return;
    default:
        return;
    }
}

}  // namespace anygba::codegen::detail
