#include "anygba/system.hpp"
#include "support/corpus.h"
#include "support/headless_run.h"
#include "video/hd/frame.h"
#include "video/hd/keys.h"
#include "video/hd/renderer.h"

#include <doctest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using anygba::System;
namespace hd = anygba::video::hd;
using anygba::video::Image;

namespace {

// Demos that cover every PPU feature: text, affine and bitmap BGs, regular, affine and window
// sprites, blending, windows, mosaic and HBlank effects. Keys move or rotate things.
struct Demo {
    const char* rom;
    std::vector<const char*> keys;
};
const std::vector<Demo> kDemos = {
    {"tonc/bin/m7_demo.gba", {"left:20-60", "up:30-90", "a:40-70"}},
    {"tonc/bin/obj_aff.gba", {"l:10-50", "r:30-70", "a:60-90"}},
    {"tonc/bin/bld_demo.gba", {"up:10-60", "a:30-80"}},
    {"tonc/bin/win_demo.gba", {"right:10-60", "down:20-70"}},
    {"tonc/bin/mos_demo.gba", {"up:10-90", "right:20-80"}},
    {"tonc/bin/bm_modes.gba", {"right:30-31", "right:70-71"}},
    {"tonc/bin/prio_demo.gba", {"right:10-70"}},
    {"tonc/bin/sbb_aff.gba", {"right:10-80", "a:20-60"}},
    {"tonc/bin/dma_demo.gba", {}},
    {"tonc/bin/oacombo.gba", {}},
    {"tonc/bin/brin_demo.gba", {"right:10-80", "down:10-50"}},
    {"tonc/bin/m7_ex.gba", {"up:10-90", "left:20-60"}},
    {"tonc/bin/pageflip.gba", {}},
    {"tonc/bin/txt_se2.gba", {}},
};

// Runs a corpus ROM in the interpreter, calling at_frame(system, captured frame) at every
// VBlank from frame `first` on.
void run_captured(const Demo& demo, std::uint64_t first, std::uint64_t last,
                  const std::function<void(System&, const hd::Frame&)>& at_frame) {
    auto rom = anygba::test::read_corpus_file(demo.rom);
    if (!rom) {
        MESSAGE("corpus not fetched; skipping ", std::string(demo.rom));
        return;
    }
    std::vector<anygba::test::KeyHold> keys;
    for (const char* k : demo.keys) keys.push_back(anygba::test::parse_key_hold(k));
    auto recorder = std::make_unique<hd::Recorder>();
    hd::Frame* frame = &recorder->frame();
    bool ready = false;
    anygba::Options options;
    options.log_misses = false;
    System sys(std::move(*rom), nullptr, options);
    sys.on_line_rendered = [&](System& s, unsigned line) { ready = recorder->on_line(s, line); };
    const auto keys_for = [&keys](std::uint64_t f) {
        std::uint16_t mask = 0;
        for (const auto& k : keys) {
            if (f >= k.first && f <= k.last) mask = static_cast<std::uint16_t>(mask | k.mask);
        }
        return mask;
    };
    sys.on_vblank = [&](System& s) {
        if (ready && s.frames() >= first) at_frame(s, *frame);
        ready = false;
        if (s.frames() >= last) s.stop();
        s.set_keys(keys_for(s.frames() + 1));
    };
    sys.run();
}

std::uint32_t expand(std::uint16_t c) { return hd::expand(static_cast<std::uint16_t>(c & 0x7FFF)); }

// Pixels of the 240x160 PPU frame that differ from sub-pixel (0, 0) of the HD picture's middle.
int mismatches(System& sys, const Image& out, int scale, int extra) {
    const auto& fb = sys.framebuffer();
    int bad = 0;
    for (int y = 0; y < anygba::kScreenHeight; ++y) {
        for (int x = 0; x < anygba::kScreenWidth; ++x) {
            const std::uint32_t want = expand(fb[static_cast<std::size_t>(y * anygba::kScreenWidth + x)]);
            const std::uint32_t got = out.row(y * scale)[(x + extra) * scale];
            if (want != got) ++bad;
        }
    }
    return bad;
}

}  // namespace

TEST_CASE("hd renderer: 1x pictures equal the scanline PPU") {
    hd::Renderer renderer;
    Image out;
    for (const Demo& demo : kDemos) {
        int frames = 0, bad_frames = 0;
        run_captured(demo, 2, 100, [&](System& sys, const hd::Frame& frame) {
            renderer.render(frame, {}, out);
            REQUIRE(out.width == anygba::kScreenWidth);
            REQUIRE(out.height == anygba::kScreenHeight);
            ++frames;
            if (mismatches(sys, out, 1, 0) != 0) ++bad_frames;
        });
        INFO(std::string(demo.rom));
        CHECK(frames >= 98);
        CHECK(bad_frames == 0);
    }
}

TEST_CASE("hd renderer: internal scale matches 1x at sub-pixel (0, 0)") {
    hd::Renderer renderer;
    Image out;
    for (const int scale : {2, 3, 4}) {
        for (const Demo& demo : kDemos) {
            int bad_frames = 0;
            run_captured(demo, 2, 70, [&](System& sys, const hd::Frame& frame) {
                hd::RenderConfig cfg;
                cfg.scale = scale;
                renderer.render(frame, cfg, out);
                REQUIRE(out.width == anygba::kScreenWidth * scale);
                if (mismatches(sys, out, scale, 0) != 0) ++bad_frames;
            });
            INFO(std::string(demo.rom), " at ", scale, "x");
            CHECK(bad_frames == 0);
        }
    }
}

TEST_CASE("hd renderer: widescreen keeps the middle 240x160 identical") {
    hd::Renderer renderer;
    Image out;
    for (const int extra : {8, 22}) {
        for (const Demo& demo : kDemos) {
            int bad_frames = 0;
            run_captured(demo, 2, 70, [&](System& sys, const hd::Frame& frame) {
                hd::RenderConfig cfg;
                cfg.extra = extra;
                cfg.scale = 2;
                renderer.render(frame, cfg, out);
                REQUIRE(out.width == (anygba::kScreenWidth + 2 * extra) * 2);
                if (mismatches(sys, out, 2, extra) != 0) ++bad_frames;
            });
            INFO(std::string(demo.rom), " with ", extra, " extra columns");
            CHECK(bad_frames == 0);
        }
    }
}
