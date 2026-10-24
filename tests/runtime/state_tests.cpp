#include "anygba/system.hpp"
#include "support/corpus.h"
#include "support/headless_run.h"

#include <doctest.h>

#include <map>
#include <string>
#include <vector>

namespace {

anygba::Options quiet() {
    anygba::Options o;
    o.log_misses = false;
    return o;
}

// Saves at frame `save_at`, plays on to `load_at`, loads, and plays to `load_at` again: the
// frames, sound and complete machine state after the load must repeat those after the save.
void check_round_trip(const char* rom_path, std::uint64_t save_at, std::uint64_t load_at) {
    const auto rom = anygba::test::read_corpus_file(rom_path);
    if (!rom) {
        MESSAGE("corpus not fetched; skipping ", rom_path);
        return;
    }
    CAPTURE(rom_path);
    anygba::System sys(*rom, nullptr, quiet());
    struct Frame {
        std::string picture;
        std::vector<std::int16_t> sound;
        std::vector<std::uint8_t> state;
    };
    std::map<std::uint64_t, Frame> first;
    std::map<std::uint64_t, Frame> second;
    std::vector<std::uint8_t> image;
    bool loaded = false;
    std::uint64_t saved_frame = 0;
    sys.on_vblank = [&](anygba::System& s) {
        const std::uint64_t f = s.frames();
        Frame& frame = (loaded ? second : first)[f];
        frame.picture = anygba::test::frame_hash(s.framebuffer());
        s.drain_audio(frame.sound);
        frame.state = s.save_state();
        if (!loaded && f == save_at) {
            s.request_safe_point([&](anygba::System& t) {
                image = t.save_state();
                saved_frame = t.frames();
            });
        }
        if (!loaded && f == load_at) {
            s.request_safe_point([&](anygba::System& t) {
                t.load_state(image);
                loaded = true;
            });
        }
        if (loaded && f == load_at) {
            s.stop();
        }
    };
    sys.run();
    REQUIRE(loaded);
    // The safe point can come a frame after the request (a game asleep in VBlankIntrWait).
    CHECK(saved_frame >= save_at);
    CHECK(saved_frame <= save_at + 1);
    for (std::uint64_t f = saved_frame + 1; f <= load_at; ++f) {
        CAPTURE(f);
        REQUIRE(second.contains(f));
        CHECK(second[f].picture == first[f].picture);
        CHECK(second[f].state == first[f].state);
        // The first frame's sound includes samples made before the save point, which a
        // load does not replay.
        if (f > saved_frame + 1) {
            CHECK(second[f].sound == first[f].sound);
        }
    }
}

}  // namespace

TEST_CASE("save state round trip repeats the same frames, sound and state (interpreter)") {
    check_round_trip("tonc/bin/irq_demo.gba", 40, 100);
    check_round_trip("tonc/bin/snd1_demo.gba", 30, 90);
    check_round_trip("tonc/bin/m7_demo.gba", 20, 60);
}

TEST_CASE("load_state rejects other ROMs, damaged files and other versions, and changes nothing") {
    const auto rom = anygba::test::read_corpus_file("tonc/bin/irq_demo.gba");
    if (!rom) {
        MESSAGE("corpus not fetched; skipping");
        return;
    }
    anygba::System sys(*rom, nullptr, quiet());
    sys.write32(0x02000000, 0x12345678);
    const std::vector<std::uint8_t> good = sys.save_state();
    sys.write32(0x02000000, 0xCAFEF00D);

    std::vector<std::uint8_t> damaged = good;
    damaged[damaged.size() / 2] ^= 0x40;
    CHECK_THROWS_WITH_AS(sys.load_state(damaged), doctest::Contains("damaged"), anygba::Error);

    std::vector<std::uint8_t> other_version = good;
    other_version[8] = 99;
    CHECK_THROWS_WITH_AS(sys.load_state(other_version), doctest::Contains("version"), anygba::Error);

    std::vector<std::uint8_t> truncated(good.begin(), good.begin() + 50);
    CHECK_THROWS_AS(sys.load_state(truncated), anygba::Error);

    std::vector<std::uint8_t> other_rom_bytes = *rom;
    other_rom_bytes[0x200] ^= 1;
    anygba::System other(other_rom_bytes, nullptr, quiet());
    CHECK_THROWS_WITH_AS(other.load_state(good), doctest::Contains("another ROM"), anygba::Error);

    CHECK(sys.read32(0x02000000) == 0xCAFEF00D);
    sys.load_state(good);
    CHECK(sys.read32(0x02000000) == 0x12345678);
}

TEST_CASE("two runs of the same ROM produce identical state files") {
    const auto rom = anygba::test::read_corpus_file("tonc/bin/snd1_demo.gba");
    if (!rom) {
        MESSAGE("corpus not fetched; skipping");
        return;
    }
    std::vector<std::uint8_t> states[2];
    for (auto& state : states) {
        anygba::System sys(*rom, nullptr, quiet());
        sys.on_vblank = [&state](anygba::System& s) {
            if (s.frames() == 20) {
                state = s.save_state();
                s.stop();
            }
        };
        sys.run();
    }
    CHECK(states[0] == states[1]);
}
