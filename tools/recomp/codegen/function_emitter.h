#pragma once

#include "codegen/c_emitter.h"

#include <cstdarg>
#include <optional>
#include <string>

namespace anygba::codegen {
// ROM address of the literal a PC-relative word load reads, when it is folded to a constant.
std::optional<std::uint32_t> folded_literal(const decode::Insn& in, const analysis::Image* image);
}  // namespace anygba::codegen

namespace anygba::codegen::detail {

// Builds the C text of one function. Structure (labels, cycle accounting, exits) lives in
// c_emitter.cpp; instruction semantics in arm_ops.cpp and thumb_ops.cpp.
class FunctionEmitter {
public:
    FunctionEmitter(const analysis::Function& fn, const EmitOptions& options);
    std::string run();

private:
    // ---- structure (c_emitter.cpp) ----
    void line(const char* fmt, ...);
    void flush_cycles(std::uint32_t resume_pc);
    void emit_instruction(const decode::Insn& in);
    // r[15] and c->t hold the destination: continue in this function if possible, else return.
    void emit_indirect_exit(const decode::Insn& in);
    void emit_jump_to(const decode::Insn& in, std::uint32_t target);
    std::string cond_expr(decode::Cond cond) const;
    bool is_label(std::uint32_t addr) const { return labels_.contains(addr); }
    static std::string hex(std::uint32_t v);

    // ---- operands (arm_ops.cpp) ----
    std::string reg(const decode::Insn& in, unsigned r) const;  // operand read, PC as constant
    // Declares `b` (and `sc` when carry_out) for the shifter operand.
    void emit_operand2(const decode::Insn& in, bool carry_out);
    void emit_offset(const decode::Insn& in);  // declares `off` for single transfers

    // ---- instruction bodies (arm_ops.cpp) ----
    void emit_data_processing(const decode::Insn& in);
    void emit_multiply(const decode::Insn& in);
    void emit_swap(const decode::Insn& in);
    void emit_single_transfer(const decode::Insn& in);
    void emit_block_transfer(const decode::Insn& in);
    void emit_psr(const decode::Insn& in);
    void emit_branch(const decode::Insn& in);

    // ---- Thumb-only forms (thumb_ops.cpp) ----
    void emit_bl_half(const decode::Insn& in);

    static std::int32_t extra_cycles(const decode::Insn& in);

    const analysis::Function& fn_;
    const EmitOptions& options_;
    std::set<std::uint32_t> labels_;
    std::string out_;
    int indent_ = 1;
    std::uint32_t pending_insns_ = 0;
    std::int32_t pending_extra_ = 0;
    std::uint32_t pending_region_ = 0;
};

}  // namespace anygba::codegen::detail
