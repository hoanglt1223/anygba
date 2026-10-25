#include "analysis/functions.h"
#include "io/miss_log.h"
#include "support/corpus.h"
#include "support/elf_symbols.h"
#include "support/headless_run.h"

#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <vector>

using namespace anygba::analysis;

namespace {

// Hand-assembled ROMs; each word or halfword is commented with its assembly.
class Rom {
public:
    explicit Rom(std::size_t size = 0x400) : bytes_(size, 0) {}
    Rom& w32(std::uint32_t addr, std::uint32_t value) {
        const std::size_t off = addr - kRomBase;
        for (int i = 0; i < 4; ++i) {
            bytes_.at(off + i) = static_cast<std::uint8_t>(value >> (8 * i));
        }
        return *this;
    }
    Rom& w16(std::uint32_t addr, std::uint16_t value) {
        const std::size_t off = addr - kRomBase;
        bytes_.at(off) = static_cast<std::uint8_t>(value);
        bytes_.at(off + 1) = static_cast<std::uint8_t>(value >> 8);
        return *this;
    }
    Image image() const { return Image(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

Discovery run(const Image& image, std::vector<EntryPoint> extra = {}, bool literals = true) {
    DiscoveryOptions options;
    options.entries.push_back({kRomBase, Mode::Arm, EntrySource::RomEntry});
    options.entries.insert(options.entries.end(), extra.begin(), extra.end());
    options.scan_literal_pools = literals;
    return discover(image, options);
}

constexpr std::uint32_t A(std::uint32_t off) { return kRomBase + off; }

}  // namespace

TEST_CASE("discovery follows BL and stops at returns") {
    const Image image = Rom()
                            .w32(A(0x00), 0xE3A00001)   // mov r0, #1
                            .w32(A(0x04), 0xEB000001)   // bl 0x08000010
                            .w32(A(0x08), 0xEAFFFFFE)   // b .
                            .w32(A(0x10), 0xE12FFF1E)   // bx lr
                            .image();
    const Discovery d = run(image);
    REQUIRE(d.functions.size() == 2);
    const Function& main = d.functions.at(A(0));
    CHECK(main.insns.size() == 3);
    CHECK(main.labels.contains(A(0x08)));
    CHECK_FALSE(main.hit_undefined);
    const Function& callee = d.functions.at(A(0x10));
    CHECK(callee.source == EntrySource::Call);
    CHECK(callee.insns.size() == 1);
}

TEST_CASE("discovery tracks ARM to Thumb through BX of a literal") {
    const Image image = Rom()
                            .w32(A(0x00), 0xE59F0004)  // ldr r0, [pc, #4]   (=0x08000011)
                            .w32(A(0x04), 0xE12FFF10)  // bx r0
                            .w32(A(0x0C), A(0x11))     // literal: Thumb 0x08000010
                            .w16(A(0x10), 0xF000)      // bl 0x08000018 (prefix)
                            .w16(A(0x12), 0xF802)      //               (suffix)
                            .w16(A(0x14), 0xE7FE)      // b .
                            .w16(A(0x18), 0x4770)      // bx lr
                            .image();
    const Discovery d = run(image);
    REQUIRE(d.functions.contains(A(0x10)));
    REQUIRE(d.functions.contains(A(0x18)));
    CHECK(d.functions.at(A(0x10)).mode == Mode::Thumb);
    CHECK(d.functions.at(A(0x10)).source == EntrySource::Branch);
    CHECK(d.functions.at(A(0x18)).mode == Mode::Thumb);
    CHECK(d.functions.at(A(0x10)).insns.at(A(0x10)).op == anygba::decode::Op::BL);
}

TEST_CASE("discovery finds the IRQ handler stored to the BIOS vector") {
    SUBCASE("absolute 0x03007FFC") {
        const Image image = Rom()
                                .w32(A(0x00), 0xE59F0008)  // ldr r0, [pc, #8]  (=handler)
                                .w32(A(0x04), 0xE59F1008)  // ldr r1, [pc, #8]  (=0x03007ffc)
                                .w32(A(0x08), 0xE5810000)  // str r0, [r1]
                                .w32(A(0x0C), 0xEAFFFFFE)  // b .
                                .w32(A(0x10), A(0x20))
                                .w32(A(0x14), 0x03007FFC)
                                .w32(A(0x20), 0xE12FFF1E)  // bx lr
                                .image();
        const Discovery d = run(image, {}, false);
        REQUIRE(d.functions.contains(A(0x20)));
        CHECK(d.functions.at(A(0x20)).source == EntrySource::IrqHandler);
    }
    SUBCASE("mirror at REG_BASE - 4") {
        const Image image = Rom()
                                .w32(A(0x00), 0xE59F0008)  // ldr r0, [pc, #8]  (=handler)
                                .w32(A(0x04), 0xE3A01301)  // mov r1, #0x04000000
                                .w32(A(0x08), 0xE5010004)  // str r0, [r1, #-4]
                                .w32(A(0x0C), 0xEAFFFFFE)  // b .
                                .w32(A(0x10), A(0x20))
                                .w32(A(0x20), 0xE12FFF1E)  // bx lr
                                .image();
        const Discovery d = run(image, {}, false);
        REQUIRE(d.functions.contains(A(0x20)));
        CHECK(d.functions.at(A(0x20)).source == EntrySource::IrqHandler);
    }
}

TEST_CASE("discovery resolves ARM jump tables") {
    SUBCASE("ldrls pc, [pc, r0, lsl #2]") {
        const Image image = Rom()
                                .w32(A(0x00), 0xE3500002)  // cmp r0, #2
                                .w32(A(0x04), 0x979FF100)  // ldrls pc, [pc, r0, lsl #2]
                                .w32(A(0x08), 0xEA000005)  // b 0x08000024
                                .w32(A(0x0C), A(0x18))
                                .w32(A(0x10), A(0x1C))
                                .w32(A(0x14), A(0x20))
                                .w32(A(0x18), 0xE12FFF1E)  // bx lr
                                .w32(A(0x1C), 0xE12FFF1E)
                                .w32(A(0x20), 0xE12FFF1E)
                                .w32(A(0x24), 0xE12FFF1E)
                                .image();
        const Discovery d = run(image, {}, false);
        const Function& f = d.functions.at(A(0));
        REQUIRE(f.jump_tables.size() == 1);
        CHECK(f.jump_tables[0].kind == JumpTable::Kind::LoadPc);
        CHECK(f.jump_tables[0].targets == std::vector<std::uint32_t>{A(0x18), A(0x1C), A(0x20)});
        CHECK(f.labels.contains(A(0x1C)));
        CHECK_FALSE(f.insns.contains(A(0x0C)));  // the table is data
        CHECK(f.insns.contains(A(0x24)));
    }
    SUBCASE("addls pc, pc, r0, lsl #2") {
        const Image image = Rom()
                                .w32(A(0x00), 0xE3500001)  // cmp r0, #1
                                .w32(A(0x04), 0x908FF100)  // addls pc, pc, r0, lsl #2
                                .w32(A(0x08), 0xEA000002)  // b 0x08000018
                                .w32(A(0x0C), 0xEA000000)  // b 0x08000014
                                .w32(A(0x10), 0xEA000000)  // b 0x08000018
                                .w32(A(0x14), 0xE12FFF1E)  // bx lr
                                .w32(A(0x18), 0xE12FFF1E)  // bx lr
                                .image();
        const Discovery d = run(image, {}, false);
        const Function& f = d.functions.at(A(0));
        REQUIRE(f.jump_tables.size() == 1);
        CHECK(f.jump_tables[0].kind == JumpTable::Kind::AddPcBranches);
        CHECK(f.jump_tables[0].targets == std::vector<std::uint32_t>{A(0x0C), A(0x10)});
        CHECK(f.insns.contains(A(0x14)));
        CHECK(f.insns.contains(A(0x18)));
    }
}

TEST_CASE("discovery resolves the GCC Thumb switch idiom") {
    const Image image = Rom()
                            .w32(A(0x00), 0xEAFFFFFE)  // b .
                            .w16(A(0x100), 0x2802)     // cmp r0, #2
                            .w16(A(0x102), 0xD807)     // bhi 0x08000114
                            .w16(A(0x104), 0x0080)     // lsls r0, r0, #2
                            .w16(A(0x106), 0x4902)     // ldr r1, [pc, #8]  (=table)
                            .w16(A(0x108), 0x1840)     // adds r0, r0, r1
                            .w16(A(0x10A), 0x6800)     // ldr r0, [r0]
                            .w16(A(0x10C), 0x4687)     // mov pc, r0
                            .w32(A(0x110), A(0x11C))
                            .w16(A(0x114), 0x4770)     // bx lr (default)
                            .w16(A(0x116), 0x4770)     // case 0
                            .w16(A(0x118), 0x4770)     // case 1
                            .w16(A(0x11A), 0x4770)     // case 2
                            .w32(A(0x11C), A(0x117))
                            .w32(A(0x120), A(0x119))
                            .w32(A(0x124), A(0x11B))
                            .image();
    const Discovery d = run(image, {{A(0x100), Mode::Thumb, EntrySource::Config}}, false);
    const Function& f = d.functions.at(A(0x100));
    REQUIRE(f.jump_tables.size() == 1);
    CHECK(f.jump_tables[0].table_addr == A(0x11C));
    CHECK(f.jump_tables[0].targets == std::vector<std::uint32_t>{A(0x116), A(0x118), A(0x11A)});
    CHECK(f.insns.contains(A(0x11A)));
    CHECK_FALSE(f.insns.contains(A(0x11C)));
}

TEST_CASE("a branch to another function's entry is a tail call") {
    const Image image = Rom()
                            .w32(A(0x00), 0xEB000002)  // bl 0x08000010
                            .w32(A(0x04), 0xEB000003)  // bl 0x08000018
                            .w32(A(0x08), 0xEAFFFFFE)  // b .
                            .w32(A(0x10), 0xE3A00000)  // mov r0, #0
                            .w32(A(0x14), 0xEAFFFFFF)  // b 0x08000018
                            .w32(A(0x18), 0xE12FFF1E)  // bx lr
                            .image();
    const Discovery d = run(image);
    const Function& f = d.functions.at(A(0x10));
    CHECK(f.insns.size() == 2);
    CHECK_FALSE(f.insns.contains(A(0x18)));
}

TEST_CASE("paths end after calls to functions that never return") {
    const Image image = Rom()
                            .w32(A(0x00), 0xEB000002)  // bl 0x08000010   (panic)
                            .w32(A(0x04), 0xF0F0F0F0)  // literal pool data, never executed
                            .w32(A(0x10), 0xE3A00000)  // mov r0, #0
                            .w32(A(0x14), 0xEAFFFFFE)  // b .
                            .image();
    const Discovery d = run(image, {}, false);
    const Function& main = d.functions.at(A(0));
    CHECK_FALSE(main.hit_undefined);
    CHECK(main.insns.size() == 1);
    CHECK(d.functions.at(A(0x10)).noreturn);
}

TEST_CASE("literal pool pointers become functions only if they decode cleanly") {
    const Image image = Rom()
                            .w32(A(0x00), 0xE59F0004)  // ldr r0, [pc, #4]  (=0x08000020, code)
                            .w32(A(0x04), 0xE59F1004)  // ldr r1, [pc, #4]  (=0x08000030, data)
                            .w32(A(0x08), 0xEAFFFFFE)  // b .
                            .w32(A(0x0C), A(0x20))
                            .w32(A(0x10), A(0x30))
                            .w32(A(0x20), 0xE2800001)  // add r0, r0, #1
                            .w32(A(0x24), 0xE12FFF1E)  // bx lr
                            .w32(A(0x30), 0xF0F0F0F0)  // NV condition: undefined on ARMv4T
                            .image();
    const Discovery d = run(image);
    REQUIRE(d.functions.contains(A(0x20)));
    CHECK(d.functions.at(A(0x20)).source == EntrySource::LiteralPool);
    CHECK_FALSE(d.functions.contains(A(0x30)));
    CHECK_FALSE(run(image, {}, false).functions.contains(A(0x20)));
}

TEST_CASE("a mov lr, pc before an indirect jump is a call") {
    const Image image = Rom()
                            .w32(A(0x00), 0xE1A0E00F)  // mov lr, pc
                            .w32(A(0x04), 0xE12FFF13)  // bx r3
                            .w32(A(0x08), 0xE3A00001)  // mov r0, #1
                            .w32(A(0x0C), 0xEAFFFFFE)  // b .
                            .image();
    const Discovery d = run(image);
    const Function& f = d.functions.at(A(0));
    CHECK(f.insns.contains(A(0x08)));
    CHECK(f.insns.contains(A(0x0C)));
}

TEST_CASE("code copied to RAM is walked from its ROM bytes") {
    Image image = Rom()
                      .w32(A(0x00), 0xE59F0000)  // ldr r0, [pc, #0]  (=0x03000000)
                      .w32(A(0x04), 0xE12FFF10)  // bx r0
                      .w32(A(0x08), 0x03000000)
                      .w32(A(0x100), 0xE3A00002)  // mov r0, #2   (runs at 0x03000000)
                      .w32(A(0x104), 0xE12FFF1E)  // bx lr
                      .image();
    CHECK_FALSE(run(image).functions.contains(0x03000000));
    image.add_region({A(0x100), 0x03000000, 0x100});
    const Discovery d = run(image);
    REQUIRE(d.functions.contains(0x03000000));
    CHECK(d.functions.at(0x03000000).insns.size() == 2);
    CHECK_THROWS_AS(image.add_region({A(0x100), 0x04000000, 4}), std::invalid_argument);
}

TEST_CASE("miss log round trip, validation, and region inference") {
    using namespace anygba::io;
    const auto records = parse_miss_log("# runtime misses\n0x03000000 arm rom=0x08000100\r\n0x02000010 thumb\n"
                                        "0x03000000 arm rom=0x08000100\n\n",
                                        "test.log");
    REQUIRE(records.size() == 2);
    CHECK(records[0].rom_src == A(0x100));
    CHECK(records[1].mode == Mode::Thumb);
    CHECK_FALSE(records[1].rom_src);
    CHECK(format_miss_record(records[0]) == "0x03000000 arm rom=0x08000100");
    CHECK(parse_miss_log(format_miss_record(records[1]), "x")[0].addr == 0x02000010);

    CHECK_THROWS_WITH_AS(parse_miss_log("0x03000002 arm\n", "m.log"), "m.log:1: misaligned address for its mode",
                         MissLogError);
    CHECK_THROWS_AS(parse_miss_log("zz arm\n", "m.log"), MissLogError);
    CHECK_THROWS_AS(parse_miss_log("0x03000000 mips\n", "m.log"), MissLogError);
    CHECK_THROWS_AS(parse_miss_log("0x03000000 arm rom=0x03000000\n", "m.log"), MissLogError);

    Image image = Rom()
                      .w32(A(0x100), 0xE3A00002)  // mov r0, #2
                      .w32(A(0x104), 0xE12FFF1E)  // bx lr
                      .image();
    std::vector<EntryPoint> entries;
    apply_miss_log({records[0]}, image, entries);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source == EntrySource::MissLog);
    CHECK(image.read32(0x03000004) == 0xE12FFF1E);
}

namespace {

struct Coverage {
    std::size_t code_bytes = 0;
    std::size_t covered = 0;
    std::size_t data_hits = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> gaps;  // uncovered code ranges
};

// Code ranges from ARM mapping symbols ($a/$t start code, $d starts data) in executable sections.
Coverage measure(const anygba::test::ElfFile& elf, const Discovery& d) {
    std::map<std::uint32_t, char> marks;
    for (const auto& s : elf.symbols) {
        if ((s.name == "$a" || s.name == "$t" || s.name == "$d" || s.name.starts_with("$a.") ||
             s.name.starts_with("$t.") || s.name.starts_with("$d.")) &&
            s.section < elf.sections.size() && (elf.sections[s.section].flags & anygba::test::kShfExecinstr)) {
            marks[s.value] = s.name[1];
        }
    }
    std::map<std::uint32_t, std::uint32_t> section_end;
    for (const auto& sec : elf.sections) {
        if ((sec.flags & anygba::test::kShfExecinstr) && sec.size) {
            section_end[sec.addr] = sec.addr + sec.size;
        }
    }
    std::map<std::uint32_t, std::uint8_t> found;
    for (const auto& [entry, fn] : d.functions) {
        for (const auto& [addr, in] : fn.insns) {
            for (std::uint32_t i = 0; i < in.size; ++i) {
                found[addr + i] = 1;
            }
        }
    }
    Coverage c;
    for (auto it = marks.begin(); it != marks.end(); ++it) {
        auto next = std::next(it);
        auto sec = section_end.upper_bound(it->first);
        if (sec == section_end.begin()) {
            continue;
        }
        const std::uint32_t end = std::min(next == marks.end() ? 0xFFFFFFFFu : next->first, std::prev(sec)->second);
        for (std::uint32_t a = it->first; a < end; ++a) {
            const bool hit = found.contains(a);
            if (it->second == 'd') {
                c.data_hits += hit ? 1 : 0;
            } else {
                ++c.code_bytes;
                c.covered += hit ? 1 : 0;
                if (!hit) {
                    if (!c.gaps.empty() && c.gaps.back().second == a) {
                        c.gaps.back().second = a + 1;
                    } else {
                        c.gaps.emplace_back(a, a + 1);
                    }
                }
            }
        }
    }
    return c;
}

}  // namespace

TEST_CASE("discovery on armwrestler against its ELF symbols") {
    const auto rom = anygba::test::read_corpus_file("armwrestler/armwrestler-gba-fixed.gba");
    const auto elf_bytes = anygba::test::read_corpus_file("armwrestler/armwrestler-gba-fixed.elf");
    if (!rom || !elf_bytes) {
        MESSAGE("SKIPPED: corpus not fetched (run tests/fetch-corpus)");
        return;
    }
    const auto elf = anygba::test::parse_elf(*elf_bytes);
    const Image image(*rom);
    const Discovery d = run(image);

    std::size_t funcs = 0;
    std::size_t found = 0;
    for (const auto& s : elf.symbols) {
        if (s.type != anygba::test::kSttFunc || s.value < kRomBase) {
            continue;
        }
        ++funcs;
        const std::uint32_t addr = s.value & ~1u;
        const bool hit = d.functions.contains(addr);
        found += hit ? 1 : 0;
        if (!hit) {
            MESSAGE("not discovered: " << s.name);
        }
    }
    const Coverage c = measure(elf, d);
    const double pct = c.code_bytes ? 100.0 * static_cast<double>(c.covered) / static_cast<double>(c.code_bytes) : 0.0;
    MESSAGE("armwrestler: " << d.functions.size() << " functions; ELF functions found " << found << "/" << funcs
                            << "; code bytes covered " << c.covered << "/" << c.code_bytes << " (" << pct
                            << "%); bytes decoded inside $d data " << c.data_hits);
    for (const auto& [begin, end] : c.gaps) {
        char range[40];
        std::snprintf(range, sizeof range, "0x%08x-0x%08x", begin, end);
        MESSAGE("uncovered code " << range);
    }
    CHECK(c.code_bytes > 0);
    CHECK(pct >= 95.0);
}

// tonc-bin ships no ELFs; this measures ROM/ELF pairs built from the tonc sources
// (devkitARM). Point ANYGBA_ELF_DIR at a tree of `<name>.elf` + `<name>.gba` files.
// Linked libraries bring many functions nothing calls, so the ground truth is the set of
// ELF functions the interpreter actually called in a 300-frame run.
TEST_CASE("discovery on ELF/ROM pairs from ANYGBA_ELF_DIR") {
    const char* dir = std::getenv("ANYGBA_ELF_DIR");
    if (!dir) {
        MESSAGE("SKIPPED: ANYGBA_ELF_DIR not set");
        return;
    }
    std::size_t executed_total = 0;
    std::size_t static_total = 0;
    std::size_t feedback_total = 0;
    std::size_t pairs = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (entry.path().extension() != ".elf") {
            continue;
        }
        auto gba = entry.path();
        gba.replace_extension(".gba");
        if (!std::filesystem::exists(gba)) {
            continue;
        }
        const auto elf = anygba::test::parse_elf(anygba::test::read_file(entry.path()));
        const auto rom = anygba::test::read_file(gba);
        std::set<std::uint32_t> called;
        try {
            anygba::test::run_headless(rom, 300, {}, [&](std::uint64_t, anygba::System& s) { called = s.misses(); });
        } catch (const std::exception& e) {
            MESSAGE(gba.stem().string() << ": interpreter stopped: " << e.what());
        }
        const Image image(rom);
        const Discovery d = run(image);
        std::vector<EntryPoint> feedback;
        for (const auto& s : elf.symbols) {
            if (s.type == anygba::test::kSttFunc && s.value >= kRomBase && called.contains(s.value & ~1u)) {
                feedback.push_back({s.value & ~1u, (s.value & 1) ? Mode::Thumb : Mode::Arm, EntrySource::MissLog});
            }
        }
        const Discovery after = run(image, feedback);
        std::size_t found = 0;
        std::size_t found_after = 0;
        for (const auto& e : feedback) {
            found += d.functions.contains(e.addr) ? 1 : 0;
            found_after += after.functions.contains(e.addr) ? 1 : 0;
            if (!d.functions.contains(e.addr)) {
                char addr[16];
                std::snprintf(addr, sizeof addr, "0x%08x", e.addr);
                MESSAGE(gba.stem().string() << ": called but not discovered statically: " << addr);
            }
        }
        MESSAGE(gba.stem().string() << ": called ROM functions found " << found << "/" << feedback.size()
                                    << " statically, " << found_after << " after feedback");
        executed_total += feedback.size();
        static_total += found;
        feedback_total += found_after;
        ++pairs;
    }
    REQUIRE(pairs > 0);
    REQUIRE(executed_total > 0);
    const double pct = 100.0 * static_cast<double>(static_total) / static_cast<double>(executed_total);
    MESSAGE(pairs << " programs: called ROM functions found " << static_total << "/" << executed_total << " (" << pct
                  << "%) statically, " << feedback_total << " after one feedback pass");
    // Measured 83.8% on the 34 tonc programs (devkitARM, GCC 16.1): the rest are reached only
    // through pointers built at run time or from RAM code, which is what feedback is for.
    CHECK(pct >= 80.0);
    CHECK(feedback_total == executed_total);
}

TEST_CASE("entries the image does not hold stay with the interpreter") {
    std::vector<std::uint8_t> rom(0x100, 0);
    const std::uint8_t bx_lr[] = {0x1E, 0xFF, 0x2F, 0xE1};
    std::copy(std::begin(bx_lr), std::end(bx_lr), rom.begin());
    const Image image(rom);
    const Discovery d = run(image, {{0x06014000, Mode::Arm, EntrySource::MissLog}});
    CHECK_FALSE(d.functions.contains(0x06014000));
    CHECK(d.functions.contains(kRomBase));
    CHECK(std::any_of(d.notes.begin(), d.notes.end(),
                      [](const std::string& n) { return n.find("0x06014000") != std::string::npos; }));
}

TEST_CASE("call targets in a [[skip]] range or on undefined bytes get no bodiless function") {
    const Image image = Rom()
                            .w32(A(0x00), 0xEB000002)  // bl 0x08000010   (inside the skip range)
                            .w32(A(0x04), 0xEB000005)  // bl 0x08000020   (undefined first word)
                            .w32(A(0x08), 0xEAFFFFFE)  // b .
                            .w32(A(0x10), 0xE12FFF1E)  // bx lr
                            .w32(A(0x20), 0xE7F000F0)  // udf
                            .image();
    DiscoveryOptions options;
    options.entries.push_back({kRomBase, Mode::Arm, EntrySource::RomEntry});
    options.skip.push_back({A(0x10), A(0x18)});
    const Discovery d = discover(image, options);
    CHECK_FALSE(d.functions.contains(A(0x10)));
    CHECK_FALSE(d.functions.contains(A(0x20)));
    for (const auto& [addr, fn] : d.functions) {
        CHECK_FALSE(fn.insns.empty());
        CHECK_FALSE(fn.tail_calls.contains(A(0x10)));
    }
    const auto noted = [&](const char* text) {
        return std::any_of(d.notes.begin(), d.notes.end(),
                           [&](const std::string& n) { return n.find(text) != std::string::npos; });
    };
    CHECK(noted("0x08000010 (call) lies in a [[skip]] range"));
    CHECK(noted("0x08000020 (call) does not start with a valid instruction"));
}
