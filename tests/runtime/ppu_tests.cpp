#include "anygba/system.hpp"
#include "support/corpus.h"
#include "support/headless_run.h"
#include "support/png.h"

#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <vector>

using anygba::System;

namespace {

const std::filesystem::path kGolden = std::filesystem::path(ANYGBA_GOLDEN_DIR) / "ppu";

struct GoldenCase {
    std::string name;
    std::string rom;
    std::uint64_t frame = 0;
    std::size_t max_diff = 0;
    bool oracle_darken_rounding = false;
    std::vector<anygba::test::KeyHold> keys;
};

std::vector<GoldenCase> load_cases() {
    std::ifstream f(kGolden / "cases.txt");
    REQUIRE(f.good());
    std::vector<GoldenCase> cases;
    std::string line;
    while (std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        GoldenCase c;
        if (!(in >> c.name >> c.rom >> c.frame)) {
            continue;
        }
        std::string token;
        while (in >> token) {
            if (token.starts_with("max-diff=")) {
                c.max_diff = std::stoul(token.substr(9));
            } else if (token == "oracle-darken-rounding") {
                c.oracle_darken_rounding = true;
            } else {
                c.keys.push_back(anygba::test::parse_key_hold(token));
            }
        }
        cases.push_back(std::move(c));
    }
    return cases;
}

struct GoldenResult {
    std::size_t best_diff = SIZE_MAX;
    std::uint64_t best_frame = 0;
    std::vector<std::uint16_t> best_picture;
    std::string error;
};

// mGBA's 16-bit (RGB565) renderer, which the libretro oracle uses, darkens the red and
// green fields with the remainder kept below the field and truncated, so those channels can
// end up one lower than c - (c*EVY >> 4), which AnyGBA uses for all three (as mGBA does for
// blue). Accepts exactly that difference and nothing else.
bool darken_rounding_only(std::uint16_t ours, std::uint16_t ref) {
    const int dr = (ours & 31) - (ref & 31);
    const int dg = ((ours >> 5) & 31) - ((ref >> 5) & 31);
    const int db = ((ours >> 10) & 31) - ((ref >> 10) & 31);
    return db == 0 && (dr == 0 || dr == 1) && (dg == 0 || dg == 1);
}

constexpr std::uint64_t kEarlier = 12;  // AnyGBA frames that may precede mGBA's for the same picture
constexpr std::uint64_t kLater = 2;

GoldenResult run_case(const GoldenCase& c, std::vector<std::uint8_t> rom) {
    GoldenResult r;
    try {
        const auto want = anygba::test::read_png_rgb555(kGolden / (c.name + ".png"), anygba::kScreenWidth,
                                                        anygba::kScreenHeight);
        const std::uint64_t first = c.frame > kEarlier ? c.frame - kEarlier : 1;
        anygba::test::run_headless(std::move(rom), c.frame + kLater, c.keys, [&](std::uint64_t f, System& s) {
            if (f < first || r.best_diff == 0) {
                return;
            }
            const auto& got = s.framebuffer();
            std::size_t diff = 0;
            for (std::size_t i = 0; i < got.size(); ++i) {
                diff += got[i] != want[i] && !(c.oracle_darken_rounding && darken_rounding_only(got[i], want[i]));
            }
            if (diff < r.best_diff) {
                r.best_diff = diff;
                r.best_frame = f;
                r.best_picture.assign(got.begin(), got.end());
            }
        });
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

}  // namespace

TEST_CASE("PPU output matches mGBA golden pictures of the tonc demos and jsmolka PPU tests") {
    const auto cases = load_cases();
    std::vector<std::future<GoldenResult>> runs;
    std::vector<const GoldenCase*> ran;
    for (const GoldenCase& c : cases) {
        auto rom = anygba::test::read_corpus_file(c.rom);
        if (!rom) {
            continue;
        }
        ran.push_back(&c);
        runs.push_back(std::async(std::launch::async, run_case, std::cref(c), std::move(*rom)));
    }
    if (ran.empty()) {
        MESSAGE("test corpus not fetched; skipping the PPU golden comparison");
        return;
    }
    std::size_t exact = 0;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const GoldenCase& c = *ran[i];
        const GoldenResult r = runs[i].get();
        CAPTURE(c.name);
        REQUIRE_MESSAGE(r.error.empty(), r.error);
        if (r.best_diff > c.max_diff) {
            const std::filesystem::path actual = std::filesystem::absolute("ppu-golden-" + c.name + ".png");
            anygba::test::write_png(actual, r.best_picture, anygba::kScreenWidth, anygba::kScreenHeight);
            MESSAGE("closest frame " << r.best_frame << " differs in " << r.best_diff << " pixels; written to "
                                     << actual.string());
        }
        CHECK(r.best_diff <= c.max_diff);
        exact += r.best_diff == 0;
    }
    MESSAGE(exact << " of " << ran.size() << " golden pictures match mGBA exactly or within the documented oracle rounding");
}

TEST_CASE("PPU forced blank shows white and BG2 reference points latch at VBlank") {
    std::vector<std::uint8_t> rom(0x200, 0);
    const std::uint32_t idle = 0xEAFFFFFE;  // b .
    std::memcpy(rom.data(), &idle, 4);
    anygba::Options options;
    options.log_misses = false;
    System sys(rom, nullptr, options);
    int vblanks = 0;
    sys.on_vblank = [&vblanks](System& s) {
        if (++vblanks == 2) s.stop();
    };
    sys.run();
    CHECK(std::all_of(sys.framebuffer().begin(), sys.framebuffer().end(), [](std::uint16_t p) { return p == 0x7FFF; }));

    // Mode 3, BG2 on, a red pixel at (0,0) and a blue one at (0,10). Scrolling BG2Y by 10
    // mid-frame moves the picture from the next line on; the next frame starts from the
    // latched register value.
    sys.write16(0x06000000, 0x001F);
    sys.write16(0x06000000 + 10 * 480, 0x7C00);
    sys.io_write16(0x000, 0x0403);
    sys.io_write16(0x02C, 10 << 8);  // BG2Y: applies immediately
    vblanks = 0;
    sys.run();
    CHECK(sys.framebuffer()[0] == 0x7C00);
    CHECK(sys.framebuffer()[150 * 240] == 0);  // past the bitmap's bottom edge: backdrop
}
