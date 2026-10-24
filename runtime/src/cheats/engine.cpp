#include "cheats/engine.h"

#include "anygba/system.hpp"

namespace anygba::cheats {

namespace {

using Kind = Op::Kind;
using Cond = Op::Cond;

std::uint32_t read(System& sys, std::uint32_t addr, std::uint8_t size) {
    return size == 1 ? sys.read8(addr) : size == 2 ? sys.read16(addr) : sys.read32(addr);
}

void store(System& sys, std::uint32_t addr, std::uint8_t size, std::uint32_t value) {
    if (size == 1) {
        sys.write8(addr, value & 0xFF);
    } else if (size == 2) {
        sys.write16(addr, value & 0xFFFF);
    } else {
        sys.write32(addr, value);
    }
}

std::int32_t sign_extend(std::uint32_t v, std::uint8_t size) {
    return size == 1 ? static_cast<std::int8_t>(v) : size == 2 ? static_cast<std::int16_t>(v) : static_cast<std::int32_t>(v);
}

bool holds(const Op& op, System& sys, const FrameKeys& keys) {
    switch (op.source) {
    case Op::Source::KeysPressed: return (keys.pressed & op.value) == op.value;
    case Op::Source::CheatButton: return keys.cheat_button;
    case Op::Source::Memory: break;
    }
    const std::uint32_t mask = op.size == 1 ? 0xFFu : op.size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    const std::uint32_t m = op.cond == Cond::Never ? 0 : read(sys, op.addr, op.size) & mask;
    const std::uint32_t v = op.value & mask;
    switch (op.cond) {
    case Cond::Eq: return m == v;
    case Cond::Ne: return m != v;
    case Cond::SignedLt: return sign_extend(m, op.size) < sign_extend(v, op.size);
    case Cond::SignedGt: return sign_extend(m, op.size) > sign_extend(v, op.size);
    case Cond::UnsignedLt: return m < v;
    case Cond::UnsignedGt: return m > v;
    case Cond::And: return (m & v) != 0;
    case Cond::Never: return false;
    }
    return false;
}

// Index just past the Else or EndIf matching the block opened before `i`; with
// `else_ends` false only EndIf ends it.
std::size_t skip_block(const std::vector<Op>& ops, std::size_t i, bool else_ends) {
    int depth = 0;
    for (; i < ops.size(); ++i) {
        const Op& op = ops[i];
        if (op.kind == Kind::If && op.block) {
            ++depth;
        } else if (op.kind == Kind::EndIf || (else_ends && op.kind == Kind::Else)) {
            if (depth == 0) return i + 1;
            if (op.kind == Kind::EndIf) --depth;
        }
    }
    return ops.size();
}

}  // namespace

void run(const Program& program, System& sys, const FrameKeys& keys) {
    const std::vector<Op>& ops = program.ops;
    std::size_t i = 0;
    while (i < ops.size()) {
        const Op& op = ops[i++];
        switch (op.kind) {
        case Kind::Write: {
            std::uint32_t addr = op.addr;
            std::uint32_t value = op.value;
            for (std::uint32_t n = 0; n < op.count; ++n) {
                store(sys, addr, op.size, value);
                addr += op.addr_step;
                value += op.value_step;
            }
            break;
        }
        case Kind::Add: store(sys, op.addr, op.size, read(sys, op.addr, op.size) + op.value); break;
        case Kind::Or: store(sys, op.addr, op.size, read(sys, op.addr, op.size) | op.value); break;
        case Kind::And: store(sys, op.addr, op.size, read(sys, op.addr, op.size) & op.value); break;
        case Kind::WriteBytes:
            for (std::size_t b = 0; b < op.bytes.size(); ++b) {
                sys.write8(op.addr + static_cast<std::uint32_t>(b), op.bytes[b]);
            }
            break;
        case Kind::Indirect: store(sys, sys.read32(op.addr) + op.addr_step, op.size, op.value); break;
        case Kind::If:
            if (holds(op, sys, keys)) break;
            if (op.stop_if_false) return;
            i = op.block ? skip_block(ops, i, true) : i + op.skip;
            break;
        case Kind::Else: i = skip_block(ops, i, false); break;  // the true branch ran
        case Kind::EndIf: break;
        case Kind::End: return;
        case Kind::RomPatch: break;
        }
    }
}

std::map<std::uint32_t, std::uint16_t> rom_patches(const std::vector<const Program*>& programs) {
    std::map<std::uint32_t, std::uint16_t> patches;
    for (const Program* p : programs) {
        for (const Op& op : p->ops) {
            if (op.kind == Kind::RomPatch) {
                patches[op.addr] = static_cast<std::uint16_t>(op.value);
            }
        }
    }
    return patches;
}

}  // namespace anygba::cheats
