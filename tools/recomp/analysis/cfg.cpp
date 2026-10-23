#include "analysis/cfg.h"

#include "decode/arm.h"
#include "decode/thumb.h"

#include <array>
#include <bit>
#include <cstdio>
#include <deque>

namespace anygba::analysis {

using decode::Cond;
using decode::Insn;
using decode::kLR;
using decode::kPC;
using decode::kSP;
using decode::Op;
using decode::Shift;

namespace {

// Abstract value of a register along one path. Enough to resolve literal-pool constants,
// ADR-style arithmetic, and the "base + index * 4" shape of switch tables.
struct Val {
    enum class Kind : std::uint8_t { Unknown, Const, Scaled, TableAddr, TableLoad };
    Kind kind = Kind::Unknown;
    std::uint32_t value = 0;  // Const: value. TableAddr/TableLoad: table base.
    std::uint32_t count = 0;  // Scaled/TableAddr/TableLoad: number of cases
    std::uint8_t index = 0;   // register the case index came from
    bool thumb_bit = false;   // TableLoad: loaded pointer was ORed with 1 before a BX

    static Val constant(std::uint32_t v) { return {Kind::Const, v, 0, 0}; }
    bool is_const() const { return kind == Kind::Const; }
};

struct State {
    std::array<Val, 16> regs{};
    std::array<std::uint32_t, 16> bound{};  // cases implied by a preceding `cmp rX, #n` (n + 1)

    void clobber(unsigned r) {
        regs[r] = Val{};
        bound[r] = 0;
    }
    void clobber_call() {
        for (unsigned r : {0u, 1u, 2u, 3u, 12u, 14u}) {
            clobber(r);
        }
    }
};

constexpr std::uint32_t kIrqVector = 0x03007FFC;
constexpr std::uint32_t kIrqVectorMirror = 0x03FFFFFC;

Val read_reg(const State& st, const Insn& in, unsigned r) {
    if (r == kPC) {
        const std::uint32_t pc = in.pc_value();
        return Val::constant(in.pc_align ? pc & ~3u : pc);
    }
    return st.regs[r];
}

std::optional<std::uint32_t> const_shift(std::uint32_t v, Shift shift, unsigned amount) {
    switch (shift) {
    case Shift::LSL: return amount >= 32 ? 0 : v << amount;
    case Shift::LSR: return amount >= 32 ? 0 : v >> amount;
    case Shift::ASR: return static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> (amount >= 32 ? 31 : amount));
    case Shift::ROR: return std::rotr(v, static_cast<int>(amount));
    default: return std::nullopt;
    }
}

// Value of a data-processing second operand, or the register-offset of a transfer.
Val operand2(const State& st, const Insn& in) {
    if (in.imm) {
        return Val::constant(in.imm_value);
    }
    if (in.shift_by_reg) {
        return {};
    }
    const Val rm = read_reg(st, in, in.rm);
    if (in.shift == Shift::LSL && in.shift_amount == 0) {
        return rm;
    }
    if (rm.is_const()) {
        if (const auto v = const_shift(rm.value, in.shift, in.shift_amount)) {
            return Val::constant(*v);
        }
        return {};
    }
    // An unbounded index (count 0) still matters: it marks a table of function pointers.
    if (in.shift == Shift::LSL && in.shift_amount == 2 && in.rm != kPC) {
        return {Val::Kind::Scaled, 0, st.bound[in.rm], in.rm};
    }
    return {};
}

Val add_vals(const Val& a, const Val& b, bool subtract) {
    if (a.is_const() && b.is_const()) {
        return Val::constant(subtract ? a.value - b.value : a.value + b.value);
    }
    if (subtract) {
        return {};
    }
    const Val* base = a.is_const() ? &a : b.is_const() ? &b : nullptr;
    const Val* index = a.kind == Val::Kind::Scaled ? &a : b.kind == Val::Kind::Scaled ? &b : nullptr;
    if (base && index) {
        return {Val::Kind::TableAddr, base->value, index->count, index->index};
    }
    return {};
}

bool is_data_processing(Op op) { return static_cast<int>(op) <= static_cast<int>(Op::MVN); }
bool is_compare(Op op) { return op == Op::TST || op == Op::TEQ || op == Op::CMP || op == Op::CMN; }

// Address a single transfer accesses, before writeback.
Val transfer_address(const State& st, const Insn& in) {
    const Val base = read_reg(st, in, in.rn);
    if (!in.pre) {
        return base;
    }
    Val offset = operand2(st, in);
    if (in.imm) {
        offset = Val::constant(in.imm_value);
    }
    if (base.kind == Val::Kind::TableAddr && offset.is_const() && offset.value == 0) {
        return base;
    }
    return add_vals(base, offset, !in.up);
}

class Walker {
public:
    Walker(const Image& image, const EntryPoint& entry, const std::set<std::uint32_t>& known,
           const std::set<std::uint32_t>& noreturn, const std::vector<AddressRange>& skip)
        : image_(image), entry_(entry), known_(known), noreturn_(noreturn), skip_(skip) {
        result_.function.entry = entry.addr;
        result_.function.mode = entry.mode;
        result_.function.source = entry.source;
        result_.function.labels.insert(entry.addr);
    }

    WalkResult run() {
        work_.push_back(entry_.addr);
        while (!work_.empty()) {
            const std::uint32_t start = work_.front();
            work_.pop_front();
            walk_path(start);
        }
        return std::move(result_);
    }

private:
    Function& fn() { return result_.function; }
    bool thumb() const { return entry_.mode == Mode::Thumb; }

    void note(const char* fmt, std::uint32_t a, std::uint32_t b = 0) {
        char buf[160];
        std::snprintf(buf, sizeof buf, fmt, a, b);
        result_.notes.emplace_back(buf);
    }

    bool skipped(std::uint32_t addr) const {
        for (const auto& r : skip_) {
            if (r.contains(addr)) {
                return true;
            }
        }
        return false;
    }

    void add_entry(std::uint32_t target, Mode mode, EntrySource source) {
        result_.found.push_back({target, mode, source});
    }

    // Branch to `target` that stays in this function unless it is another function's entry.
    void branch_to(std::uint32_t target) {
        if (target != entry_.addr && known_.contains(target)) {
            fn().tail_calls.insert(target);
            return;
        }
        fn().labels.insert(target);
        work_.push_back(target);
    }

    // An indirect jump that leaves the function: a constant target is a tail call.
    void exit_jump(std::optional<std::uint32_t> constant_target) {
        if (constant_target) {
            fn().tail_calls.insert(*constant_target);
        } else {
            fn().has_exit = true;
        }
    }

    void constant_jump(std::uint32_t value, bool exchange) {
        Mode mode = entry_.mode;
        std::uint32_t target = value;
        if (exchange) {
            mode = (value & 1u) ? Mode::Thumb : Mode::Arm;
        }
        target &= mode == Mode::Thumb ? ~1u : ~3u;
        if (image_.contains(target, 2)) {
            add_entry(target, mode, EntrySource::Branch);
        }
    }

    // Walks forward from `start` until the path ends or meets code already walked.
    void walk_path(std::uint32_t start) {
        State st;
        std::uint32_t pc = start;
        for (;;) {
            if (fn().insns.contains(pc)) {
                return;
            }
            if (skipped(pc)) {
                note("path enters skipped range at 0x%08x", pc);
                return;
            }
            const Insn in = decode_at(image_, pc, entry_.mode);
            if (!in.valid()) {
                fn().hit_undefined = true;
                note(image_.contains(pc, 2) ? "undefined instruction at 0x%08x" : "path leaves the image at 0x%08x", pc);
                return;
            }
            fn().insns.emplace(pc, in);
            const bool keep_going = flow(st, in);
            apply(st, in);
            if (!keep_going) {
                return;
            }
            pc = in.next();
        }
    }

    // True when execution can continue at the next instruction.
    bool flow(const State& st, const Insn& in) {
        const bool always = in.cond == Cond::AL;
        // A computed jump with LR already pointing just past it is an indirect call.
        const Val lr = st.regs[kLR];
        const bool call_idiom = lr.is_const() && (lr.value & ~1u) == in.next();

        switch (in.op) {
        case Op::B:
            branch_to(in.imm_value);
            return !always;
        case Op::BL: {
            add_entry(in.imm_value, entry_.mode, EntrySource::Call);
            // Calls through a `bx rN` veneer (GCC's _call_via_rN, crt0's blx stubs): the
            // real target is the caller's constant in rN.
            const Insn callee = decode_at(image_, in.imm_value, entry_.mode);
            if (callee.op == Op::BX && callee.cond == Cond::AL && callee.rm != kLR && callee.rm != kPC &&
                st.regs[callee.rm].is_const()) {
                constant_jump(st.regs[callee.rm].value, true);
            }
            return !noreturn_.contains(in.imm_value);
        }
        case Op::BL_HI:
        case Op::SWI:
            return true;
        case Op::BL_LO:
            return true;  // indirect call through LR
        case Op::BX: {
            const Val target = read_reg(st, in, in.rm);
            if (target.is_const()) {
                constant_jump(target.value, true);
                if (!call_idiom) {
                    exit_jump(target.value & ((target.value & 1u) ? ~1u : ~3u));
                }
            } else if (target.kind == Val::Kind::TableLoad) {
                table_jump(in, target, true);
            } else if (!call_idiom) {
                exit_jump(std::nullopt);
            }
            return !always || call_idiom;
        }
        case Op::LDM:
            if ((in.reglist >> kPC) & 1u) {
                exit_jump(std::nullopt);
                return !always;
            }
            return true;
        case Op::LDR:
            if (in.rd != kPC) {
                return true;
            }
            return load_pc(st, in, call_idiom) || !always || call_idiom;
        default:
            break;
        }
        if (is_data_processing(in.op) && !is_compare(in.op) && in.rd == kPC) {
            data_processing_pc(st, in, call_idiom);
            return !always || call_idiom;
        }
        return true;
    }

    // Returns true if the load was a recognised table whose cases continue in this function.
    bool load_pc(const State& st, const Insn& in, bool call_idiom) {
        if (in.pre && !in.imm && in.shift == Shift::LSL && in.shift_amount == 2 && !in.shift_by_reg && in.up &&
            in.rm != kPC) {
            const Val base = read_reg(st, in, in.rn);
            if (base.is_const()) {
                table_jump(in, {Val::Kind::TableLoad, base.value, st.bound[in.rm], in.rm}, false);
                return st.bound[in.rm] != 0 && in.cond != Cond::AL;
            }
        }
        const Val addr = transfer_address(st, in);
        if (addr.kind == Val::Kind::TableAddr) {
            table_jump(in, {Val::Kind::TableLoad, addr.value, addr.count, addr.index}, false);
            return false;
        }
        std::optional<std::uint32_t> target;
        if (addr.is_const() && Image::is_rom(addr.value)) {
            if (const auto value = image_.read32(addr.value)) {
                constant_jump(*value, false);  // ARMv4T LDR to PC does not change state
                target = *value & (thumb() ? ~1u : ~3u);
            }
        }
        if (!call_idiom) {
            exit_jump(target);
        }
        return false;
    }

    void data_processing_pc(const State& st, const Insn& in, bool call_idiom) {
        // add pc, pc, rI, lsl #2: a branch table starts two instructions after the jump.
        if (in.op == Op::ADD && in.rn == kPC && !in.imm && !in.shift_by_reg && in.shift == Shift::LSL &&
            in.shift_amount == 2 && st.bound[in.rm] != 0 && !thumb()) {
            if (auto table = read_jump_table(image_, JumpTable::Kind::AddPcBranches, in.addr, in.pc_value(),
                                             st.bound[in.rm], in.rm, entry_.mode)) {
                adopt_table(std::move(*table));
            }
            return;
        }
        std::optional<std::uint32_t> target;
        std::uint32_t unused_bound = 0;
        const Val v = dp_result(st, in, unused_bound);
        if (v.is_const()) {
            // `subs pc, ...` also restores CPSR; the state it returns to is assumed unchanged.
            constant_jump(v.value, false);
            target = v.value & (thumb() ? ~1u : ~3u);
        } else if (v.kind == Val::Kind::TableLoad) {
            table_jump(in, v, false);
            return;
        }
        if (!call_idiom) {
            exit_jump(target);
        }
    }

    void table_jump(const Insn& in, const Val& load, bool exchange) {
        if (load.count == 0) {
            pointer_table(load.value, exchange, load.thumb_bit);
            fn().has_exit = true;  // a tail call into one of the table's functions
            return;
        }
        if (auto table = read_jump_table(image_, JumpTable::Kind::LoadPc, in.addr, load.value, load.count, load.index,
                                         entry_.mode)) {
            adopt_table(std::move(*table));
        } else {
            note("unresolved jump table at 0x%08x (base 0x%08x)", in.addr, load.value);
        }
    }

    // An index with no visible bound: the table holds function pointers. Take entries for as
    // long as they look like code in this area; each becomes a function, not a label, since
    // the generated code dispatches these jumps at run time anyway.
    void pointer_table(std::uint32_t base, bool exchange, bool thumb_bit) {
        for (std::uint32_t i = 0; i < kMaxJumpTableEntries; ++i) {
            auto value = image_.read32(base + 4 * i);
            if (!value) {
                return;
            }
            if (thumb_bit) {
                *value |= 1u;
            }
            const Mode mode = exchange ? ((*value & 1u) ? Mode::Thumb : Mode::Arm) : entry_.mode;
            const std::uint32_t target = *value & (mode == Mode::Thumb ? ~1u : ~3u);
            const bool aligned = mode == Mode::Thumb ? (!exchange || (*value & 1u) != 0) : (*value & 3u) == 0;
            if (!aligned || (target >> 24) != (entry_.addr >> 24) || !decode_at(image_, target, mode).valid()) {
                return;
            }
            add_entry(target, mode, EntrySource::JumpTable);
        }
    }

    void adopt_table(JumpTable table) {
        for (const std::uint32_t target : table.targets) {
            fn().labels.insert(target);
            work_.push_back(target);
        }
        fn().jump_tables.push_back(std::move(table));
    }

    void set(State& st, const Insn& in, unsigned r, Val v, std::uint32_t bound = 0) {
        if (r == kPC) {
            return;
        }
        if (in.cond != Cond::AL) {
            st.clobber(r);
            return;
        }
        st.regs[r] = v;
        st.bound[r] = bound;
    }

    void apply(State& st, const Insn& in) {
        if (is_data_processing(in.op)) {
            apply_data_processing(st, in);
            return;
        }
        switch (in.op) {
        case Op::LDR: {
            const Val addr = transfer_address(st, in);
            Val result;
            if (addr.is_const() && (in.rn == kPC || Image::is_rom(addr.value))) {
                if (const auto value = image_.read32(addr.value)) {
                    result = Val::constant(*value);
                    if (in.rn == kPC) {
                        literal(*value);
                    }
                }
            } else if (addr.kind == Val::Kind::TableAddr) {
                result = {Val::Kind::TableLoad, addr.value, addr.count, addr.index};
            } else if (!in.imm && in.pre && in.up && !in.shift_by_reg && in.shift == Shift::LSL &&
                       in.shift_amount == 2 && in.rm != kPC) {
                const Val base = read_reg(st, in, in.rn);
                if (base.is_const()) {
                    result = {Val::Kind::TableLoad, base.value, st.bound[in.rm], in.rm};
                }
            }
            if (in.writeback) {
                st.clobber(in.rn);
            }
            set(st, in, in.rd, result);
            break;
        }
        case Op::STR: {
            const Val addr = transfer_address(st, in);
            const Val value = read_reg(st, in, in.rd);
            if (addr.is_const() && (addr.value == kIrqVector || addr.value == kIrqVectorMirror) && value.is_const()) {
                // The BIOS calls the handler with LDR PC, so it always runs in ARM state.
                const std::uint32_t handler = value.value & ~3u;
                if (image_.contains(handler, 4)) {
                    add_entry(handler, Mode::Arm, EntrySource::IrqHandler);
                } else {
                    note("IRQ handler 0x%08x stored at 0x%08x is outside the image", handler, in.addr);
                }
            }
            if (in.writeback) {
                st.clobber(in.rn);
            }
            break;
        }
        case Op::LDRB:
        case Op::LDRH:
        case Op::LDRSB:
        case Op::LDRSH:
            if (in.writeback) {
                st.clobber(in.rn);
            }
            st.clobber(in.rd);
            break;
        case Op::STRB:
        case Op::STRH:
        case Op::STM:
            if (in.writeback) {
                st.clobber(in.rn);
            }
            break;
        case Op::LDM:
            for (unsigned r = 0; r < 15; ++r) {
                if ((in.reglist >> r) & 1u) {
                    st.clobber(r);
                }
            }
            if (in.writeback) {
                st.clobber(in.rn);
            }
            break;
        case Op::MUL:
        case Op::MLA:
        case Op::SWP:
        case Op::SWPB:
        case Op::MRS:
            st.clobber(in.rd);
            break;
        case Op::UMULL:
        case Op::UMLAL:
        case Op::SMULL:
        case Op::SMLAL:
            st.clobber(in.rd);
            st.clobber(in.rn);
            break;
        case Op::BL:
        case Op::BL_LO:
        case Op::SWI:
            st.clobber_call();
            break;
        case Op::BL_HI:
            set(st, in, kLR, Val::constant(in.pc_value() + in.imm_value));
            break;
        default:
            break;
        }
    }

    void apply_data_processing(State& st, const Insn& in) {
        if (is_compare(in.op)) {
            if (in.op == Op::CMP && in.imm && in.rn != kPC && in.imm_value < kMaxJumpTableEntries) {
                st.bound[in.rn] = in.imm_value + 1;
            }
            return;
        }
        std::uint32_t bound = 0;
        const Val result = dp_result(st, in, bound);
        set(st, in, in.rd, result, bound);
    }

    // Abstract result of a data-processing instruction; `bound` receives the case count a
    // plain register copy carries over.
    Val dp_result(const State& st, const Insn& in, std::uint32_t& bound) const {
        const Val a = read_reg(st, in, in.rn);
        const Val b = operand2(st, in);
        Val result;
        switch (in.op) {
        case Op::MOV:
            result = b;
            if (!in.imm && !in.shift_by_reg && in.shift == Shift::LSL && in.shift_amount == 0 && in.rm != kPC) {
                bound = st.bound[in.rm];
            }
            break;
        case Op::MVN:
            if (b.is_const()) {
                result = Val::constant(~b.value);
            }
            break;
        case Op::ADD:
            result = add_vals(a, b, false);
            break;
        case Op::SUB:
            result = add_vals(a, b, true);
            break;
        case Op::RSB:
            if (a.is_const() && b.is_const()) {
                result = Val::constant(b.value - a.value);
            }
            break;
        case Op::ORR:
            if (a.kind == Val::Kind::TableLoad && b.is_const() && b.value == 1) {
                result = a;
                result.thumb_bit = true;
                break;
            }
            [[fallthrough]];
        case Op::EOR:
        case Op::AND:
        case Op::BIC:
            if (a.is_const() && b.is_const()) {
                const std::uint32_t x = a.value;
                const std::uint32_t y = b.value;
                result = Val::constant(in.op == Op::ORR ? x | y : in.op == Op::EOR ? x ^ y : in.op == Op::AND ? x & y : x & ~y);
            }
            break;
        default:
            break;
        }
        return result;
    }

    void literal(std::uint32_t value) {
        const bool odd = (value & 1u) != 0;
        const std::uint32_t target = value & ~1u;
        if (!odd && (value & 3u) != 0) {
            return;
        }
        if (target == entry_.addr || !image_.contains(target, odd ? 2 : 4)) {
            return;
        }
        result_.candidates.push_back({target, odd ? Mode::Thumb : Mode::Arm, EntrySource::LiteralPool});
    }

    const Image& image_;
    EntryPoint entry_;
    const std::set<std::uint32_t>& known_;
    const std::set<std::uint32_t>& noreturn_;
    const std::vector<AddressRange>& skip_;
    std::deque<std::uint32_t> work_;
    WalkResult result_;
};

}  // namespace

Insn decode_at(const Image& image, std::uint32_t addr, Mode mode) {
    if (mode == Mode::Arm) {
        const auto word = image.read32(addr);
        if (!word || (addr & 3u) != 0) {
            Insn undefined;
            undefined.addr = addr;
            return undefined;
        }
        return decode::decode_arm(*word, addr);
    }
    const auto half = image.read16(addr);
    if (!half || (addr & 1u) != 0) {
        Insn undefined;
        undefined.addr = addr;
        undefined.thumb = true;
        undefined.size = 2;
        return undefined;
    }
    const auto next = image.read16(addr + 2);
    return decode::decode_thumb_at(*half, next.value_or(0), addr);
}

WalkResult walk_function(const Image& image, const EntryPoint& entry, const std::set<std::uint32_t>& known_entries,
                         const std::set<std::uint32_t>& noreturn, const std::vector<AddressRange>& skip) {
    return Walker(image, entry, known_entries, noreturn, skip).run();
}

}  // namespace anygba::analysis
