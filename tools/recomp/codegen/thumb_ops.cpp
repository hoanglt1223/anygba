#include "codegen/function_emitter.h"

// Thumb forms with no ARM equivalent: the two halves of BL when they appear on their own.
// Everything else in Thumb is emitted through the ARM-equivalent paths in arm_ops.cpp.
namespace anygba::codegen::detail {

using decode::Insn;
using decode::Op;

void FunctionEmitter::emit_bl_half(const Insn& in) {
    if (in.op == Op::BL_HI) {
        line("c->r[14] = %s;", hex(in.pc_value() + in.imm_value).c_str());
        return;
    }
    // The suffix alone jumps to LR + offset and leaves the return address in LR.
    line("{");
    ++indent_;
    line("const uint32_t target = c->r[14] + %s;", hex(in.imm_value).c_str());
    line("c->r[14] = %s;", hex(in.next() | 1u).c_str());
    line("c->r[15] = target & ~1u;");
    line("AG_ICALL(c, %s);", hex(in.next()).c_str());
    --indent_;
    line("}");
}

}  // namespace anygba::codegen::detail
