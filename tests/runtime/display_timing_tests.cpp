#include "anygba/system.hpp"
#include "player/display_timing.h"
#include "video/settings.h"

#include <doctest.h>

#include <cmath>
#include <map>

using anygba::player::Cadence;

namespace {

// Checks the cadence of the GBA on a `display_hz` monitor over `frames` frames: each frame
// stays up for floor or ceil of display/game vblanks, the counts average to the ratio, and the
// schedule never drifts a whole vblank from the ideal (frames are evenly spread).
std::map<unsigned, std::uint64_t> check_cadence(double display_hz, std::uint64_t frames) {
    const Cadence c(anygba::kFramesPerSecond, display_hz);
    const double ratio = display_hz / anygba::kFramesPerSecond;
    std::map<unsigned, std::uint64_t> counts;
    for (std::uint64_t n = 0; n < frames; ++n) {
        const unsigned v = c.vblanks(n);
        ++counts[v];
        CHECK((v == static_cast<unsigned>(std::floor(ratio)) || v == static_cast<unsigned>(std::ceil(ratio))));
        const double ideal = static_cast<double>(n) * ratio;
        CHECK(std::abs(static_cast<double>(c.first_vblank(n)) - ideal) < 1.0);
    }
    CHECK(static_cast<double>(c.first_vblank(frames)) / static_cast<double>(frames) == doctest::Approx(ratio).epsilon(1e-3));
    return counts;
}

}  // namespace

TEST_CASE("pacing: 59.73 fps cadence on 60, 75, 120, 144 and 165 Hz monitors") {
    const std::uint64_t frames = 59727;  // about 1000 s of play
    // 60 Hz: one vblank per frame, two for about one frame in 220 (every 3.7 s).
    auto c60 = check_cadence(60, frames);
    CHECK(c60[2] == doctest::Approx(frames * (60 / anygba::kFramesPerSecond - 1)).epsilon(0.01));
    // 75 Hz: 1-1-1-2 with one more 2 now and then (1.2557 vblanks per frame).
    auto c75 = check_cadence(75, frames);
    CHECK(c75.size() == 2);
    // 120 Hz: two per frame, three for one frame in 220.
    auto c120 = check_cadence(120, frames);
    CHECK(c120[3] == doctest::Approx(frames * (120 / anygba::kFramesPerSecond - 2)).epsilon(0.01));
    check_cadence(144, frames);  // 2.411: mostly 2-3-2-2-3
    check_cadence(165, frames);  // 2.763: mostly 3-3-3-2
    const Cadence c(anygba::kFramesPerSecond, 75);
    CHECK(c.vblanks(0) == 2);  // frame 0 at vblank 0, frame 1 at ceil(1.2557) = 2
    CHECK(c.vblanks(1) == 1);
    CHECK(c.position(0) == 0.0);
    CHECK(c.position(2) == doctest::Approx(2 * anygba::kFramesPerSecond / 75));
}

TEST_CASE("pacing: syncing the game to the monitor only within 1%") {
    using anygba::player::synced_game_hz;
    const double gba = anygba::kFramesPerSecond;
    CHECK(synced_game_hz(gba, 60) == 60);          // +0.46%
    CHECK(synced_game_hz(gba, 59.94) == 59.94);
    CHECK(synced_game_hz(gba, 120) == 60);         // two vblanks per frame
    CHECK(synced_game_hz(gba, 239.76) == 59.94);
    CHECK(synced_game_hz(gba, 144) == gba);        // 72 or 48: too far
    CHECK(synced_game_hz(gba, 75) == gba);
    CHECK(synced_game_hz(gba, 50) == gba);
    CHECK(synced_game_hz(gba, 0) == gba);          // unknown monitor
    const Cadence synced(60, 120);
    for (std::uint64_t n = 0; n < 1000; ++n) CHECK(synced.vblanks(n) == 2);  // no judder at all
}

TEST_CASE("pacing: fps cap intervals, monitor rates and audio steering") {
    using anygba::player::vsync_interval;
    CHECK(vsync_interval(144, 0) == 1);
    CHECK(vsync_interval(60, 60) == 1);
    CHECK(vsync_interval(59.94, 60) == 1);
    CHECK(vsync_interval(60, 30) == 2);
    CHECK(vsync_interval(144, 60) == 3);   // 48 shown per second: at most the cap
    CHECK(vsync_interval(120, 60) == 2);
    CHECK(anygba::player::same_rate(59.95, 60));
    CHECK_FALSE(anygba::player::same_rate(144, 120));
    CHECK_FALSE(anygba::player::same_rate(60, 0));
    using anygba::player::audio_ratio;
    CHECK(audio_ratio(1.0, 1000, 1000) == 1.0);
    CHECK(audio_ratio(1.0, 2000, 1000) == doctest::Approx(1.005));  // too much queued: play faster
    CHECK(audio_ratio(1.0, 0, 1000) == doctest::Approx(0.995));
    CHECK(audio_ratio(1.0, 100000, 1000) == doctest::Approx(1.005));  // clamped
    CHECK(audio_ratio(60 / anygba::kFramesPerSecond, 1000, 1000) == doctest::Approx(1.00456).epsilon(1e-4));
}

TEST_CASE("pacing: refresh, VRR, sync, menu fps and fps cap settings") {
    namespace video = anygba::video;
    video::Options o;
    CHECK(o.refresh == 0);
    CHECK(o.menu_fps == 30);
    CHECK(o.fps_cap == 0);
    CHECK_FALSE(video::apply_serialized(o, "refresh=144;vrr=true;sync-to-monitor=on;menu-fps=60;fps-cap=30"));
    CHECK(o.refresh == 144);
    CHECK(o.vrr);
    CHECK(o.sync_to_monitor);
    CHECK(o.menu_fps == 60);
    CHECK(o.fps_cap == 30);
    video::Options back;
    CHECK_FALSE(video::apply_serialized(back, video::serialize(o)));
    CHECK(back == o);
    CHECK_FALSE(video::apply_setting(o, "refresh", "auto"));
    CHECK(o.refresh == 0);
    CHECK_FALSE(video::apply_setting(o, "fps-cap", "off"));
    CHECK(video::apply_setting(o, "refresh", "100"));
    CHECK(video::apply_setting(o, "menu-fps", "off"));
    CHECK(video::apply_setting(o, "fps-cap", "45"));

    video::Options m;
    const auto items = video::menu_items(m);
    REQUIRE(items.size() == 23);
    CHECK(items[18].label == "Refresh");
    CHECK(items[18].value == "Auto (match monitor)");
    video::menu_step(m, 18, 1);
    CHECK(m.refresh == 60);
    video::menu_step(m, 18, -1);
    video::menu_step(m, 18, -1);
    CHECK(m.refresh == 240);
    video::menu_step(m, 22, 1);
    CHECK(m.fps_cap == 30);
    CHECK(video::menu_items(m)[22].value == "30");
}
