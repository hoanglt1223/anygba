#include "codegen/c_emitter.h"

#include "codegen/function_emitter.h"
#include "decode/disasm.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <set>

namespace anygba::codegen {

using decode::Cond;
using decode::Insn;
using decode::kPC;
using decode::Op;

std::string function_name(std::uint32_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "f_%08x", addr);
    return buf;
}

std::string emit_function(const analysis::Function& fn, const EmitOptions& options) {
    return detail::FunctionEmitter(fn, options).run();
}

std::optional<std::uint32_t> folded_literal(const Insn& in, const analysis::Image* image) {
    if (in.op != Op::LDR || in.rn != kPC || !in.imm || !image || in.rd == kPC) {
        return std::nullopt;
    }
    const std::uint32_t base = in.pc_align ? in.pc_value() & ~3u : in.pc_value();
    const std::uint32_t addr = in.up ? base + in.imm_value : base - in.imm_value;
    if (!analysis::Image::is_rom(addr) || (addr & 3) != 0 || !image->read32(addr)) {
        return std::nullopt;
    }
    return addr;
}

analysis::AddressRange code_footprint(const analysis::Function& fn, const analysis::Image* image) {
    if (fn.insns.empty()) {
        return {};
    }
    std::uint32_t lo = UINT32_MAX;
    std::uint32_t hi = 0;
    const auto cover = [&](std::uint32_t begin, std::uint32_t end) {
        lo = std::min(lo, begin);
        hi = std::max(hi, end);
    };
    for (const auto& [addr, in] : fn.insns) {
        cover(addr, in.next());
        if (const auto literal = folded_literal(in, image)) {
            cover(*literal, *literal + 4);
        }
    }
    for (const auto& table : fn.jump_tables) {
        if (table.kind == analysis::JumpTable::Kind::LoadPc) {
            cover(table.table_addr, table.table_addr + 4 * static_cast<std::uint32_t>(table.targets.size()));
        }
    }
    return {lo, hi};
}

namespace detail {

namespace {

bool writes_pc_indirectly(const Insn& in) {
    switch (in.op) {
    case Op::BX:
        return true;
    case Op::LDR:
        return in.rd == kPC;
    case Op::LDM:
        return ((in.reglist >> kPC) & 1u) != 0 || in.reglist == 0;
    default:
        return static_cast<int>(in.op) <= static_cast<int>(Op::MVN) && in.rd == kPC &&
               !(in.op == Op::TST || in.op == Op::TEQ || in.op == Op::CMP || in.op == Op::CMN);
    }
}

bool is_control(const Insn& in) {
    switch (in.op) {
    case Op::B:
    case Op::BL:
    case Op::BL_LO:
    case Op::SWI:
        return true;
    default:
        return writes_pc_indirectly(in);
    }
}

}  // namespace

FunctionEmitter::FunctionEmitter(const analysis::Function& fn, const EmitOptions& options)
    : fn_(fn), options_(options), labels_(fn.labels) {
    for (const auto& table : fn_.jump_tables) {
        labels_.insert(table.targets.begin(), table.targets.end());
    }
    // An indirect jump may be a call that comes back to the next instruction.
    for (const auto& [addr, in] : fn_.insns) {
        if ((writes_pc_indirectly(in) || in.op == Op::BL_LO) && fn_.insns.contains(in.next())) {
            labels_.insert(in.next());
        }
    }
}

std::string FunctionEmitter::hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08xu", v);
    return buf;
}

void FunctionEmitter::line(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
    out_ += buf;
    out_ += '\n';
}

std::string FunctionEmitter::cond_expr(Cond cond) const {
    switch (cond) {
    case Cond::EQ: return "c->z";
    case Cond::NE: return "!c->z";
    case Cond::CS: return "c->c";
    case Cond::CC: return "!c->c";
    case Cond::MI: return "c->n";
    case Cond::PL: return "!c->n";
    case Cond::VS: return "c->v";
    case Cond::VC: return "!c->v";
    case Cond::HI: return "c->c && !c->z";
    case Cond::LS: return "!c->c || c->z";
    case Cond::GE: return "c->n == c->v";
    case Cond::LT: return "c->n != c->v";
    case Cond::GT: return "!c->z && c->n == c->v";
    case Cond::LE: return "c->z || c->n != c->v";
    default: return "1";
    }
}

void FunctionEmitter::flush_cycles(std::uint32_t resume_pc) {
    if (pending_insns_ == 0 && pending_extra_ == 0) {
        return;
    }
    line("AG_SPEND(c, %u, %u, %d);", pending_region_, pending_insns_, pending_extra_);
    line("AG_TICK(c, %s);", hex(resume_pc).c_str());
    pending_insns_ = 0;
    pending_extra_ = 0;
}

std::int32_t FunctionEmitter::extra_cycles(const Insn& in) {
    switch (in.op) {
    case Op::LDR: case Op::LDRB: case Op::LDRH: case Op::LDRSB: case Op::LDRSH: return 1;
    case Op::MUL: case Op::MLA: return 2;
    case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL: return 3;
    case Op::LDM: case Op::STM: {
        std::int32_t n = 0;
        for (unsigned r = 0; r < 16; ++r) {
            n += (in.reglist >> r) & 1;
        }
        return n;
    }
    default: return 0;
    }
}

std::string FunctionEmitter::run() {
    const std::string name = options_.name.empty() ? function_name(fn_.entry) : options_.name;
    out_ += "void " + name + "(anygba_cpu* c) {\n";
    if (analysis::Image::is_rom(fn_.entry)) {
        line("AG_ENTER(c, %s);", hex(fn_.entry).c_str());
    } else {
        line("AG_ENTER_RAM(c, %s, %d);", hex(fn_.entry).c_str(), fn_.mode == analysis::Mode::Thumb ? 1 : 0);
    }
    if (!fn_.insns.empty() && fn_.insns.begin()->first != fn_.entry) {
        // Code is emitted in address order; a branch target below the entry comes first.
        line("goto L_%08x;", fn_.entry);
    }
    const Insn* previous = nullptr;
    for (auto it = fn_.insns.begin(); it != fn_.insns.end(); ++it) {
        const Insn& in = it->second;
        const std::uint32_t region = (in.addr >> 24) & 15;
        if (is_label(in.addr)) {
            flush_cycles(in.addr);
            out_ += "L_" + hex(in.addr).substr(2, 8) + ":;\n";
        } else if (pending_insns_ != 0 && region != pending_region_) {
            flush_cycles(in.addr);
        }
        if (!options_.coverage.empty() && options_.blocks) {
            // Blocks: the entry, branch targets and return points, and the fall-through of a
            // conditional branch (whose cycles emit_instruction has already charged).
            const bool fall_through = previous && previous->next() == in.addr && previous->cond != Cond::AL &&
                                      is_control(*previous);
            if (in.addr == fn_.entry || is_label(in.addr) || fall_through) {
                line("%s[%zu]++;", options_.coverage.c_str(), options_.blocks->size());
                options_.blocks->push_back(in.addr);
            }
        }
        previous = &in;
        if (pending_insns_ == 0) {
            pending_region_ = region;
        }
        line("/* %08x: %s */", in.addr, decode::disassemble(in).c_str());
        ++pending_insns_;
        pending_extra_ += extra_cycles(in);
        emit_instruction(in);

        const auto next = std::next(it);
        if (next == fn_.insns.end() || next->first != in.next()) {
            flush_cycles(in.next());
            line("c->r[15] = %s;", hex(in.next()).c_str());
            line("return;");
        }
    }
    out_ += "}\n";
    // Drop labels nothing jumps to; compilers warn about them.
    for (const std::uint32_t label : labels_) {
        const std::string label_name = "L_" + hex(label).substr(2, 8);
        if (out_.find("goto " + label_name + ";") == std::string::npos) {
            const auto pos = out_.find(label_name + ":;\n");
            if (pos != std::string::npos) {
                out_.erase(pos, label_name.size() + 3);
            }
        }
    }
    return std::move(out_);
}

void FunctionEmitter::emit_instruction(const Insn& in) {
    if (!in.valid()) {
        flush_cycles(in.addr);
        line("anygba_undefined(c, %s, %s);", hex(in.addr).c_str(), hex(in.raw).c_str());
        line("return;");
        return;
    }
    if (is_control(in)) {
        flush_cycles(in.addr);
    }
    const bool conditional = in.cond != Cond::AL;
    if (conditional) {
        line("if (%s) {", cond_expr(in.cond).c_str());
        ++indent_;
    }
    switch (in.op) {
    case Op::MUL: case Op::MLA: case Op::UMULL: case Op::UMLAL: case Op::SMULL: case Op::SMLAL:
        emit_multiply(in);
        break;
    case Op::SWP: case Op::SWPB:
        emit_swap(in);
        break;
    case Op::LDR: case Op::STR: case Op::LDRB: case Op::STRB:
    case Op::LDRH: case Op::STRH: case Op::LDRSB: case Op::LDRSH:
        emit_single_transfer(in);
        break;
    case Op::LDM: case Op::STM:
        emit_block_transfer(in);
        break;
    case Op::B: case Op::BL: case Op::BX: case Op::SWI:
        emit_branch(in);
        break;
    case Op::BL_HI: case Op::BL_LO:
        emit_bl_half(in);
        break;
    case Op::MRS: case Op::MSR:
        emit_psr(in);
        break;
    default:
        emit_data_processing(in);
        break;
    }
    if (conditional) {
        --indent_;
        line("}");
    }
}

void FunctionEmitter::emit_indirect_exit(const Insn& in) {
    for (const auto& table : fn_.jump_tables) {
        if (table.jump_addr != in.addr) {
            continue;
        }
        line("if (c->t == %d) {", fn_.mode == analysis::Mode::Thumb ? 1 : 0);
        ++indent_;
        line("switch (c->r[15]) {");
        // Tables often repeat a target (the default case of a sparse switch); C needs each once.
        for (const std::uint32_t target : std::set<std::uint32_t>(table.targets.begin(), table.targets.end())) {
            line("case %s: goto L_%08x;", hex(target).c_str(), target);
        }
        line("default: break;");
        line("}");
        --indent_;
        line("}");
    }
    const std::uint32_t next = in.next();
    if (fn_.insns.contains(next) && is_label(next)) {
        line("if ((c->r[14] & ~1u) == %s) {", hex(next).c_str());
        ++indent_;
        line("AG_ICALL(c, %s);", hex(next).c_str());
        line("goto L_%08x;", next);
        --indent_;
        line("}");
    }
    line("return;");
}

void FunctionEmitter::emit_jump_to(const Insn& in, std::uint32_t target) {
    if (target == in.addr && in.cond == Cond::AL && is_label(target)) {
        line("c->r[15] = %s;", hex(target).c_str());
        line("anygba_idle_loop(c);");
        line("goto L_%08x;", target);
    } else if (is_label(target)) {
        line("goto L_%08x;", target);
    } else {
        line("c->r[15] = %s;", hex(target).c_str());
        line("return;");
    }
}

}  // namespace detail

}  // namespace anygba::codegen
