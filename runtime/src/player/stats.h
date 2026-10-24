#pragma once

#include "player/osd.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The stats overlay (Ctrl+F): performance and setup figures in the top-left corner.
namespace anygba::player {

struct Stats {
    double host_fps = 0;         // pictures shown per second
    double gba_fps = 0;          // emulated frames per second
    int target_speed = 100;      // percent; kUnlimited (0) = unthrottled
    bool fast_forward = false;   // the fast-forward hotkey is held
    double frame_ms = 0;         // host time to emulate one frame, waits excluded
    std::optional<double> audio_fill;  // percent of the target queue; nullopt without audio
    std::uint64_t underruns = 0;       // DirectSound FIFO underruns since start
    double interpreted = 0;            // percent of emulated cycles run in the interpreter
    std::string filter;                // video filter spec
    std::string output;                // output resolution ("720x480") or "headless"
    std::size_t cheats = 0;
    std::string save;                  // save type name
    bool rtc = false;
};

// Measured speed in percent of the GBA's 59.73 frames per second.
double speed_percent(double gba_fps);
std::vector<std::string> stats_lines(const Stats& stats);
// Draws the lines over a shaded box in the top-left corner.
void draw_stats(Picture picture, const std::vector<std::string>& lines);

}  // namespace anygba::player
