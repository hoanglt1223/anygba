#include "anygba/system.hpp"
#include "support/corpus.h"

#include <doctest.h>

#include <string>

namespace {

struct RomResult {
    bool idle = false;
    std::uint32_t r12 = 0;
    std::uint64_t frames = 0;
};

// Runs a jsmolka ROM with no recompiled code at all, so every instruction goes through
// the reference interpreter. The ROM reports the first failing test number in r12.
RomResult run_interpreted(const std::vector<std::uint8_t>& rom) {
    anygba::Options options;
    options.log_misses = false;
    anygba::System sys(rom, nullptr, options);
    RomResult result;
    sys.on_idle_loop = [&](anygba::System& s) {
        result.idle = true;
        s.stop();
    };
    sys.on_vblank = [](anygba::System& s) {
        if (s.frames() >= 600) {
            s.stop();
        }
    };
    sys.run();
    result.r12 = sys.cpu().r[12];
    result.frames = sys.frames();
    return result;
}

}  // namespace

TEST_CASE("the reference interpreter alone passes jsmolka arm, thumb and memory") {
    for (const char* name : {"arm", "thumb", "memory"}) {
        CAPTURE(name);
        const auto rom = anygba::test::read_corpus_file(std::string("gba-tests/") + name + "/" + name + ".gba");
        if (!rom) {
            MESSAGE("SKIPPED: corpus not fetched (run tests/fetch-corpus)");
            return;
        }
        const RomResult r = run_interpreted(*rom);
        CHECK(r.idle);
        CHECK(r.r12 == 0);
    }
}

TEST_CASE("an unknown SWI stops the run with its number") {
    // mov r0, #0 ; swi 0x2B0000 (ARM comment field: function 0x2B in bits 23-16, past the BIOS table)
    std::vector<std::uint8_t> rom(0x200, 0);
    const std::uint32_t code[] = {0xE3A00000, 0xEF2B0000};
    for (std::size_t i = 0; i < 2; ++i) {
        for (int b = 0; b < 4; ++b) {
            rom[i * 4 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(code[i] >> (8 * b));
        }
    }
    anygba::Options options;
    options.log_misses = false;
    anygba::System sys(rom, nullptr, options);
    CHECK_THROWS_WITH_AS(sys.run(), "unknown BIOS call SWI 0x2B at 0x08000004", anygba::Error);
}
