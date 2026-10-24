#include "anygba/system.hpp"
#include "codegen/c_emitter.h"
#include "decode/arm.h"
#include "decode/disasm.h"
#include "decode/thumb.h"
#include "interp/interp.h"
#include "support/codegen_fixture.h"

#include <doctest.h>

#include <bit>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

using anygba::decode::Insn;
using anygba::decode::Op;

namespace {

// Memory operations are steered into an IWRAM mirror so every store they can make lands
// in the 32 KiB that the test compares.
constexpr std::uint32_t kSandbox = 0x03804000;
constexpr std::uint32_t kSandboxReach = 0x10000;

bool is_single_transfer(const Insn& in) {
    switch (in.op) {
    case Op::LDR: case Op::STR: case Op::LDRB: case Op::STRB:
    case Op::LDRH: case Op::STRH: case Op::LDRSB: case Op::LDRSH:
        return true;
    default:
        return false;
    }
}

bool touches_memory(const Insn& in) {
    return is_single_transfer(in) || in.op == Op::LDM || in.op == Op::STM || in.op == Op::SWP || in.op == Op::SWPB;
}

// Effective address of a single transfer for steering only; correctness is judged by
// comparing the two implementations, not by this.
std::uint32_t transfer_address(const anygba_cpu& c, const Insn& in) {
    if (in.rn == 15) {
        return 0x08000000;  // ROM: loads read the random ROM, stores are ignored
    }
    const std::uint32_t base = c.r[in.rn];
    std::uint32_t off = in.imm_value;
    if (!in.imm) {
        const std::uint32_t m = c.r[in.rm];
        const unsigned k = in.shift_amount;
        switch (in.shift) {
        case anygba::decode::Shift::LSL: off = k >= 32 ? 0 : m << k; break;
        case anygba::decode::Shift::LSR: off = k >= 32 ? 0 : m >> k; break;
        case anygba::decode::Shift::ASR: off = static_cast<std::uint32_t>(static_cast<std::int32_t>(m) >> (k >= 32 ? 31 : k)); break;
        case anygba::decode::Shift::ROR: off = std::rotr(m, static_cast<int>(k)); break;
        case anygba::decode::Shift::RRX: off = (c.c << 31) | (m >> 1); break;
        }
    }
    return in.pre ? (in.up ? base + off : base - off) : base;
}

bool in_sandbox(std::uint32_t addr) { return addr - (kSandbox - kSandboxReach) < 2 * kSandboxReach; }

std::string describe(const anygba_cpu& a, const anygba_cpu& b) {
    std::string out;
    char buf[64];
    for (int r = 0; r < 16; ++r) {
        if (a.r[r] != b.r[r]) {
            std::snprintf(buf, sizeof buf, " r%d %08x/%08x", r, a.r[r], b.r[r]);
            out += buf;
        }
    }
    if (ag_cpsr(&a) != ag_cpsr(&b)) {
        std::snprintf(buf, sizeof buf, " cpsr %08x/%08x", ag_cpsr(&a), ag_cpsr(&b));
        out += buf;
    }
    if (a.spsr != b.spsr) {
        out += " spsr";
    }
    if (std::memcmp(a.bank_r8_12, b.bank_r8_12, sizeof a.bank_r8_12) != 0 ||
        std::memcmp(a.bank_r13_14, b.bank_r13_14, sizeof a.bank_r13_14) != 0 ||
        std::memcmp(a.bank_spsr, b.bank_spsr, sizeof a.bank_spsr) != 0) {
        out += " banks";
    }
    return out;
}

}  // namespace

TEST_CASE("generated code matches the reference interpreter on random instruction states") {
    constexpr int kStatesPerInstruction = 80;
    std::mt19937 rng(0x5EED);
    std::vector<std::uint8_t> rom(1u << 20);
    for (auto& b : rom) {
        b = static_cast<std::uint8_t>(rng());
    }
    anygba::Options options;
    options.log_misses = false;
    anygba::System sys(rom, nullptr, options);
    anygba_cpu& c = sys.cpu();
    std::vector<std::uint8_t>& iwram = sys.memory().iwram;
    for (auto& b : iwram) {
        b = static_cast<std::uint8_t>(rng());
    }

    static constexpr std::uint32_t kModes[] = {ANYGBA_MODE_USR, ANYGBA_MODE_SYS, ANYGBA_MODE_IRQ,
                                               ANYGBA_MODE_SVC, ANYGBA_MODE_FIQ, ANYGBA_MODE_UND};
    std::size_t states = 0;
    std::size_t skipped = 0;
    std::size_t failures = 0;
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> interp_memory;
    std::set<unsigned> reported;  // one example per failing instruction

    for (unsigned i = 0; i < anygba_fixture_count; ++i) {
        const anygba_fixture_entry& e = anygba_fixture[i];
        const Insn in = e.thumb ? anygba::decode::decode_thumb(static_cast<std::uint16_t>(e.raw), e.addr)
                                : anygba::decode::decode_arm(e.raw, e.addr);
        REQUIRE(in.valid());
        const bool memory = touches_memory(in);
        for (int s = 0; s < kStatesPerInstruction; ++s) {
            for (auto& bank : c.bank_r8_12) for (auto& r : bank) r = rng();
            for (auto& bank : c.bank_r13_14) for (auto& r : bank) r = rng();
            for (auto& r : c.bank_spsr) r = rng();
            const std::uint32_t mode = kModes[rng() % std::size(kModes)];
            sys.set_cpsr((rng() & 0xF00000C0u) | mode | (e.thumb ? 0x20u : 0u));
            c.spsr = rng();
            for (int r = 0; r < 15; ++r) {
                c.r[r] = (rng() & 3) == 0 ? rng() & 0x3F : rng();
            }
            if (memory) {
                c.r[in.rn] = kSandbox + (rng() & 0xFFF);
                if (is_single_transfer(in) && !in.imm && in.rm != in.rn && in.rm != 15) {
                    c.r[in.rm] = rng() & 0xFF;
                }
                if (is_single_transfer(in) && !in_sandbox(transfer_address(c, in)) && in.rn != 15) {
                    ++skipped;
                    continue;
                }
            }
            c.r[15] = in.addr;
            c.cycles = 1 << 30;
            c.ret_depth = 0;
            if (memory) {
                before = iwram;
            }
            const anygba_cpu start = c;

            anygba::interp::execute(sys, in);
            const anygba_cpu expected = c;
            if (memory) {
                interp_memory = iwram;
                iwram = before;
            }

            c = start;
            e.fn(&c);
            ++states;

            std::string diff = describe(expected, c);
            if (memory && iwram != interp_memory) {
                diff += " memory";
            }
            if (!diff.empty()) {
                ++failures;
                if (reported.insert(i).second && reported.size() <= 40) {
                    char head[96];
                    std::snprintf(head, sizeof head, "%08x %s: ", e.raw, e.thumb ? "thumb" : "arm");
                    MESSAGE(head << anygba::decode::disassemble(in) << " | interp/generated:" << diff);
                }
            }
            if (memory) {
                iwram = before;
            }
        }
    }
    MESSAGE(anygba_fixture_count << " instructions, " << states << " states compared, " << skipped
                                 << " states skipped (address outside the sandbox), " << failures << " mismatches");
    CHECK(states >= 1000000);
    CHECK(failures == 0);
}

TEST_CASE("a branch to a label the walk never decoded leaves through r[15]") {
    // Thumb `b #0x03000100` in IWRAM; the target is a label but was never decoded.
    anygba::analysis::Function fn;
    fn.entry = 0x03000000;
    fn.mode = anygba::analysis::Mode::Thumb;
    fn.insns.emplace(0x03000000, anygba::decode::decode_thumb(0xE07E, 0x03000000));
    fn.labels = {0x03000000, 0x03000100};
    const std::string c = anygba::codegen::emit_function(fn, {});
    CHECK(c.find("goto L_03000100") == std::string::npos);
    CHECK(c.find("c->r[15] = 0x03000100u;") != std::string::npos);
    CHECK(c.find("AG_ENTER(") == std::string::npos);  // RAM code: ROM patches never cover it
    CHECK(c.find("AG_ENTER_RAM(c, 0x03000000u, 1);") != std::string::npos);
}

TEST_CASE("ROM functions start with AG_ENTER and report their code footprint") {
    anygba::analysis::Function fn;
    fn.entry = 0x08000100;
    fn.mode = anygba::analysis::Mode::Thumb;
    fn.insns.emplace(0x08000100, anygba::decode::decode_thumb(0x4801, 0x08000100));  // ldr r0, [pc, #4]
    fn.insns.emplace(0x08000102, anygba::decode::decode_thumb(0x4770, 0x08000102));  // bx lr
    fn.labels = {0x08000100};
    const std::string c = anygba::codegen::emit_function(fn, {});
    CHECK(c.find("AG_ENTER(c, 0x08000100u);") != std::string::npos);
    const auto code = anygba::codegen::code_footprint(fn, nullptr);
    CHECK(code.begin == 0x08000100);
    CHECK(code.end == 0x08000104);
}
