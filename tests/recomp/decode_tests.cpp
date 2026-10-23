#include "decode/arm.h"
#include "decode/disasm.h"
#include "decode/thumb.h"

#include <capstone/capstone.h>
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace anygba::decode;

namespace {

constexpr std::uint32_t kBase = 0x08000000;

class Capstone {
public:
    explicit Capstone(cs_mode mode) : mode_(mode) {
        REQUIRE(cs_open(CS_ARCH_ARM, mode, &handle_) == CS_ERR_OK);
        insn_ = cs_malloc(handle_);
    }
    ~Capstone() {
        cs_free(insn_, 1);
        cs_close(&handle_);
    }
    Capstone(const Capstone&) = delete;
    Capstone& operator=(const Capstone&) = delete;

    // Returns the text of the first instruction if it decodes and spans exactly `size` bytes.
    std::optional<std::string> disasm(std::uint32_t value, std::size_t size, std::uint32_t addr) {
        std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                                 static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
        const std::uint8_t* code = bytes;
        std::size_t left = size;
        std::uint64_t address = addr;
        if (!cs_disasm_iter(handle_, &code, &left, &address, insn_) || insn_->size != size) {
            return std::nullopt;
        }
        mnemonic_ = insn_->mnemonic;
        std::string text = mnemonic_;
        if (insn_->op_str[0] != '\0') {
            text += ' ';
            text += insn_->op_str;
        }
        return text;
    }
    const std::string& last_mnemonic() const { return mnemonic_; }

    // Capstone carries Thumb-2 IT-block state from one call to the next; start clean after an IT.
    void reset_after_it() {
        if (mnemonic_.starts_with("it")) {
            const cs_mode mode = mode_;
            cs_free(insn_, 1);
            cs_close(&handle_);
            REQUIRE(cs_open(CS_ARCH_ARM, mode, &handle_) == CS_ERR_OK);
            insn_ = cs_malloc(handle_);
        }
    }

private:
    cs_mode mode_;
    std::string mnemonic_;
    csh handle_ = 0;
    cs_insn* insn_ = nullptr;
};

std::string base_mnemonic(std::string m) {
    static const char* conds[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"};
    for (const char* c : conds) {
        const std::string s(c);
        if (m.size() > s.size() + 1 && m.ends_with(s)) {
            return m.substr(0, m.size() - s.size());
        }
    }
    return m;
}

// Mnemonics Capstone accepts in encodings ARMv4T leaves undefined: later architecture
// versions, coprocessor instructions, and the permanently undefined space.
bool post_v4t_mnemonic(const std::string& mnemonic) {
    static const std::set<std::string> allowed = {
        // ARMv5/v5TE
        "blx", "bkpt", "clz", "qadd", "qsub", "qdadd", "qdsub", "ldrd", "strd", "pld",
        "smlabb", "smlabt", "smlatb", "smlatt", "smlawb", "smlawt", "smulbb", "smulbt", "smultb", "smultt",
        "smulwb", "smulwt", "smlalbb", "smlalbt", "smlaltb", "smlaltt",
        // coprocessor and undefined space
        "cdp", "mcr", "mrc", "mcrr", "mrrc", "ldc", "ldcl", "stc", "stcl", "udf", "trap",
        // ARMv6 and later
        "ldrex", "ldrexb", "ldrexh", "ldrexd", "strex", "strexb", "strexh", "strexd", "nop", "yield", "wfe",
        "wfi", "sev", "sevl", "dbg", "movw", "movt", "mls", "umaal", "smc", "hvc", "eret", "ldrht", "strht",
        "ldrsbt", "ldrsht", "sbfx", "ubfx", "bfi", "bfc", "rbit", "rev", "rev16", "revsh", "sel", "ssat",
        "usat", "ssat16", "usat16", "pkhbt", "pkhtb", "sxtb", "sxth", "sxtb16", "uxtb", "uxth", "uxtb16",
        "sxtab", "sxtah", "sxtab16", "uxtab", "uxtah", "uxtab16", "usad8", "usada8", "smlad", "smladx",
        "smlsd", "smlsdx", "smlald", "smlaldx", "smlsld", "smlsldx", "smmla", "smmlar", "smmls", "smmlsr",
        "smmul", "smmulr", "smuad", "smuadx", "smusd", "smusdx", "sdiv", "udiv", "cbz", "cbnz", "it", "cps",
        "cpsie", "cpsid", "setend", "srsda", "srsdb", "srsia", "srsib", "rfeda", "rfedb", "rfeia", "rfeib",
        "crc32b", "crc32h", "crc32w", "crc32cb", "crc32ch", "crc32cw", "hlt", "esb", "csdb", "ssbb", "pssbb",
        "bxns", "blxns", "lda", "ldab", "ldah", "stl", "stlb", "stlh", "ldaex", "ldaexb", "ldaexh", "ldaexd", "stlex", "stlexb",
        "stlexh", "stlexd", "setpan", "sb", "dmb", "dsb", "isb", "clrex", "pldw", "pli",
    };
    if (allowed.contains(mnemonic) || allowed.contains(base_mnemonic(mnemonic))) {
        return true;
    }
    // ARMv6 parallel add/subtract: {s,q,sh,u,uq,uh}{add16,asx,sax,sub16,add8,sub8}
    static const char* stems[] = {"add16", "asx", "sax", "sub16", "add8", "sub8"};
    const std::string m = base_mnemonic(mnemonic);
    for (const char* stem : stems) {
        if (m.ends_with(stem)) {
            return true;
        }
    }
    return m.starts_with("ldc") || m.starts_with("stc") || m.starts_with("mcr") || m.starts_with("mrc") ||
           m.starts_with("cdp") || m.starts_with("vld") || m.starts_with("vst") || m.starts_with("vm") ||
           m.starts_with("hint") || m.starts_with("it") || m.starts_with("fldm") || m.starts_with("fstm") ||
           m.starts_with("v");  // VFP/NEON live in the coprocessor space
}

struct Tally {
    std::size_t compared = 0;
    std::size_t undefined_both = 0;
    std::size_t post_v4t = 0;
    std::size_t ours_only = 0;
    std::size_t failure_count = 0;
    // A few examples per kind of failure keep the output readable.
    std::map<std::string, std::vector<std::string>> failures;

    void fail(const std::string& kind, std::string message) {
        ++failure_count;
        auto& examples = failures[kind];
        if (examples.size() < 2) {
            examples.push_back(std::move(message));
        }
    }
};

void check_one(Tally& t, Capstone& cs, const Insn& in, std::uint32_t value, std::size_t size,
               bool ours_only_ok) {
    const auto theirs = cs.disasm(value, size, in.addr);
    if (theirs) {
        cs.reset_after_it();
    }
    char hex[16];
    std::snprintf(hex, sizeof hex, size == 2 ? "%04x" : "%08x", value);
    if (in.valid() && theirs) {
        ++t.compared;
        const std::string ours = disassemble(in);
        if (ours != *theirs) {
            if (post_v4t_mnemonic(cs.last_mnemonic())) {
                ++t.post_v4t;  // a later architecture reads this encoding differently
            } else {
                t.fail("differs:" + cs.last_mnemonic(), std::string(hex) + ": ours '" + ours + "' capstone '" + *theirs + "'");
            }
        }
    } else if (in.valid()) {
        if (ours_only_ok) {
            ++t.ours_only;
        } else {
            t.fail("rejected", std::string(hex) + ": ours '" + disassemble(in) + "' capstone rejects it");
        }
    } else if (theirs) {
        const bool arm_nv = size == 4 && !in.thumb && (value >> 28) == 0xF;
        if (arm_nv || post_v4t_mnemonic(cs.last_mnemonic())) {
            ++t.post_v4t;
        } else {
            t.fail("undefined:" + cs.last_mnemonic(), std::string(hex) + ": ours undefined, capstone '" + *theirs + "'");
        }
    } else {
        ++t.undefined_both;
    }
}

void report(const std::string& what, const Tally& t) {
    MESSAGE(what << ": " << t.compared << " compared, " << t.post_v4t << " post-v4T, " << t.ours_only
                 << " v4T-only, " << t.undefined_both << " undefined in both");
    for (const auto& [kind, examples] : t.failures) {
        for (const auto& e : examples) {
            MESSAGE(kind << " | " << e);
        }
    }
    CHECK(t.failure_count == 0);
}

// ARM7TDMI defines an empty register list (transfers PC, base moves by 0x40); Capstone rejects it.
bool empty_reglist(const Insn& in) { return (in.op == Op::LDM || in.op == Op::STM) && in.reglist == 0; }

// ARM7TDMI ignores should-be-zero fields (Rn of MOV/MVN, the low bits of MRS); Capstone
// rejects some of those encodings.
bool arm_only_ok(const Insn& in) {
    return empty_reglist(in) || ((in.op == Op::MOV || in.op == Op::MVN) && in.rn != 0) ||
           (in.op == Op::MRS && (in.raw & 0xFFFu) != 0) || (in.op == Op::MSR && in.psr_mask == 0);
}

}  // namespace

TEST_CASE("Thumb decoder matches Capstone on every halfword") {
    Capstone cs(CS_MODE_THUMB);
    Tally t;
    for (std::uint32_t h = 0; h <= 0xFFFF; ++h) {
        const auto hw = static_cast<std::uint16_t>(h);
        if (h >= 0xF000 && h < 0xF800) {
            // BL prefix: check it as a pair with a suffix that varies with the prefix.
            const auto suffix = static_cast<std::uint16_t>(0xF800u | ((h * 0x2B5u) & 0x7FFu));
            const Insn pair = decode_thumb_at(hw, suffix, kBase);
            REQUIRE(pair.op == Op::BL);
            check_one(t, cs, pair, h | (static_cast<std::uint32_t>(suffix) << 16), 4, false);
            continue;
        }
        const Insn in = decode_thumb(hw, kBase);
        if (in.op == Op::BL_LO) {
            ++t.ours_only;  // a lone suffix has no Capstone equivalent
            continue;
        }
        check_one(t, cs, in, h, 2, empty_reglist(in));
    }
    report("thumb", t);
}

TEST_CASE("ARM decoder matches Capstone on every class with sampled operands") {
    Capstone cs(CS_MODE_ARM);
    Tally t;
    std::mt19937 rng(0xA5C0DE);
    // Bits 27-20 and 7-4 select the instruction class; sample the rest for each of the
    // 4096 classes, under every condition code.
    for (std::uint32_t cls = 0; cls < 4096; ++cls) {
        for (int sample = 0; sample < 64; ++sample) {
            std::uint32_t w = rng();
            w = (w & 0xF00FFF0Fu) | ((cls >> 4) << 20) | ((cls & 0xF) << 4);
            if (sample < 8) {
                w = (w & 0x0FFFFFFFu) | 0xE0000000u;
            }
            const Insn in = decode_arm(w, kBase);
            check_one(t, cs, in, w, 4, arm_only_ok(in));
        }
    }
    report("arm", t);
}

TEST_CASE("ARM decoder edge cases") {
    Capstone cs(CS_MODE_ARM);
    Tally t;
    const std::uint32_t words[] = {
        0xE1A00000, 0xE1A00060, 0xE1B00060, 0xE1A00020, 0xE1A00040, 0xE3A00E10, 0xE3A00102, 0xE3A0010F,
        0xE5110000, 0xE5010000, 0xE49D4004, 0xE52D4004, 0xE8BD0010, 0xE92D0010, 0xE8BD8000, 0xE8F08000,
        0xE12FFF1E, 0xE12FFF10, 0xE10F0000, 0xE14F0000, 0xE129F000, 0xE128F000, 0xE328F20F, 0xE121F001,
        0xE1910092, 0xE1C100B0, 0xE15000B0, 0xE10000B0, 0xE59FF004, 0xE08FF100, 0xE79F0000, 0xE1B0F00E,
        0xE25EF004, 0xE1A0F00E, 0x00000000, 0xEF000005, 0xEF123456, 0xEB000000, 0xEAFFFFFE, 0xE0A10392,
    };
    for (const std::uint32_t w : words) {
        check_one(t, cs, decode_arm(w, kBase), w, 4, false);
    }
    report("arm edge cases", t);

    CHECK_FALSE(decode_arm(0xF0000000, kBase).valid());  // NV condition
    CHECK_FALSE(decode_arm(0xEE000000, kBase).valid());  // coprocessor
    CHECK_FALSE(decode_arm(0xE12FFF30, kBase).valid());  // BLX (ARMv5)
    CHECK_FALSE(decode_arm(0xE1C000D0, kBase).valid());  // LDRD (ARMv5TE)
    CHECK(decode_arm(0xE8900000, kBase).valid());        // empty list is defined on ARM7TDMI
}

TEST_CASE("decoder fields") {
    const Insn b = decode_arm(0xEAFFFFFE, 0x08000100);
    CHECK(b.op == Op::B);
    CHECK(b.imm_value == 0x08000100);

    const Insn lsr = decode_arm(0xE1A00020, kBase);  // lsr r0, r0, #32
    CHECK(lsr.shift == Shift::LSR);
    CHECK(lsr.shift_amount == 32);

    const Insn rrx = decode_arm(0xE1A00060, kBase);
    CHECK(rrx.shift == Shift::RRX);

    const Insn imm = decode_arm(0xE3A0010F, kBase);  // mov r0, #0xc0000003
    CHECK(imm.imm_value == 0xC0000003u);
    CHECK(imm.imm_rot == 2);

    const Insn bl = decode_thumb_at(0xF000, 0xF802, 0x08000010);
    CHECK(bl.op == Op::BL);
    CHECK(bl.size == 4);
    CHECK(bl.imm_value == 0x08000018);

    const Insn lone = decode_thumb_at(0xF000, 0x2001, 0x08000010);
    CHECK(lone.op == Op::BL_HI);
    CHECK(lone.size == 2);

    const Insn pop = decode_thumb(0xBD10, kBase);
    CHECK(pop.op == Op::LDM);
    CHECK(pop.reglist == ((1u << 4) | (1u << 15)));

    const Insn ldm = decode_thumb(0xC902, kBase);  // ldm r1, {r1}: base in list, no writeback
    CHECK_FALSE(ldm.writeback);

    const Insn ldr_pc = decode_thumb(0x4801, 0x08000002);
    CHECK(ldr_pc.pc_align);
    CHECK(ldr_pc.imm_value == 4);
}
