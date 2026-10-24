#include "anygba/system.hpp"
#include "support/corpus.h"
#include "support/headless_run.h"
#include "video/motion.h"
#include "video/settings.h"

#include <doctest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

using anygba::video::MotionInterpolator;
using anygba::video::wrapped_delta;

TEST_CASE("smoothing: wrapped deltas take the short way round") {
    CHECK(wrapped_delta(10, 14, 512) == 4);
    CHECK(wrapped_delta(510, 2, 512) == 4);
    CHECK(wrapped_delta(2, 510, 512) == -4);
    CHECK(wrapped_delta(250, 3, 256) == 9);
}

namespace {

struct MotionCase {
    std::string rom;
    std::uint64_t frame;  // compared against the frame before it
    std::vector<std::string> keys;
    std::string half_hash;  // the picture half way between the two frames
};

// Runs `c` with smoothing recording every line; at c.frame checks that the pictures right
// after the previous frame and right before the current one are those real frames, and returns
// the hash of the picture half way.
std::string run_case(const MotionCase& c, bool& smooth) {
    auto rom = anygba::test::read_corpus_file(c.rom);
    if (!rom) return {};
    std::vector<anygba::test::KeyHold> keys;
    for (const auto& k : c.keys) keys.push_back(anygba::test::parse_key_hold(k));
    auto owned = std::make_unique<MotionInterpolator>();  // about 1 MB of line state
    MotionInterpolator& motion = *owned;
    std::vector<std::uint16_t> prev;
    std::string half;
    anygba::test::run_headless(std::move(*rom), c.frame, keys, [&](std::uint64_t frame, anygba::System& sys) {
        if (!sys.on_line_rendered) {
            sys.on_line_rendered = [&motion](anygba::System& s, unsigned line) { motion.on_line(s, line); };
            return;
        }
        motion.end_frame();
        if (frame == c.frame) {
            smooth = motion.smooth();
            if (!smooth) return;
            const auto& vram = sys.memory().vram;
            const auto near_prev = motion.render(0.01, vram);
            CHECK(std::vector<std::uint16_t>(near_prev.begin(), near_prev.end()) == prev);
            const auto near_cur = motion.render(0.99, vram);
            CHECK(std::equal(near_cur.begin(), near_cur.end(), sys.framebuffer().begin()));
            half = anygba::test::frame_hash(motion.render(0.5, vram));
        }
        prev.assign(sys.framebuffer().begin(), sys.framebuffer().end());
    });
    return half;
}

}  // namespace

TEST_CASE("smoothing: pictures between frames of scrolling and moving homebrew") {
    // Hashes of the half-way pictures: background scroll (brin_demo, sbb_reg) and a sprite
    // moving over a static background (obj_demo).
    const std::vector<MotionCase> cases = {
        {"tonc/bin/brin_demo.gba", 60, {"right:20-70", "down:30-70"}, "39157a6087aca89f705f586d69a4187caa8c0bf1eb93a9049567e9c7b9bab31c"},
        {"tonc/bin/sbb_reg.gba", 60, {"right:20-70"}, "0450490a46eb9461a190214798fd881775702c2fe37c57092f808802311794f3"},
        {"tonc/bin/obj_demo.gba", 60, {"left:20-70", "up:30-70"}, "37b49130271e1e495fb26f3c055ee1ffa4db9f8817fa96af0668e85345250c34"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.rom);
        bool smooth = false;
        const std::string half = run_case(c, smooth);
        if (!anygba::test::read_corpus_file(c.rom)) {
            MESSAGE("test corpus not fetched; skipping");
            return;
        }
        CHECK(smooth);
        MESSAGE(c.rom << " half-way hash " << half);
        if (!c.half_hash.empty()) CHECK(half == c.half_hash);
    }
}

TEST_CASE("smoothing: setting, menu row and default") {
    anygba::video::Options o;
    CHECK_FALSE(o.smoothing);
    CHECK_FALSE(anygba::video::apply_setting(o, "smoothing", "on"));
    CHECK(o.smoothing);
    CHECK(anygba::video::serialize(o) == "smoothing=true");
    const auto items = anygba::video::menu_items(o);
    REQUIRE(items.size() == 23);
    CHECK(items[17].label == "Smoothing");
    anygba::video::menu_step(o, 17, 1);
    CHECK_FALSE(o.smoothing);
}
