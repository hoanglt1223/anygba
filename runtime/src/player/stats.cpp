#include "player/stats.h"

#include "player/speed.h"

#include <algorithm>
#include <cstdio>

namespace anygba::player {

namespace {

constexpr double kGbaFramesPerSecond = 16777216.0 / 280896.0;

template <typename... T>
std::string format(const char* fmt, T... values) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, fmt, values...);
    return buffer;
}

}  // namespace

double speed_percent(double gba_fps) { return gba_fps * 100.0 / kGbaFramesPerSecond; }

std::vector<std::string> stats_lines(const Stats& s) {
    std::vector<std::string> out;
    out.push_back(format("FPS %.1f host, %.1f GBA", s.host_fps, s.gba_fps));
    const std::string target = s.target_speed == kUnlimited ? "unlimited" : std::to_string(s.target_speed) + "%";
    out.push_back(format("Speed %.0f%% of %s%s", speed_percent(s.gba_fps), target.c_str(), s.fast_forward ? " (FF)" : ""));
    out.push_back(format("Frame %.2f ms", s.frame_ms));
    const std::string audio = s.audio_fill ? format("%.0f%%", *s.audio_fill) : std::string("off");
    out.push_back(format("Audio %s, %llu underruns", audio.c_str(), static_cast<unsigned long long>(s.underruns)));
    out.push_back(format("Interpreter %.1f%%", s.interpreted));
    out.push_back("Video " + s.filter + " " + s.output);
    out.push_back(format("Cheats %zu  Save %s  RTC %s", s.cheats, s.save.c_str(), s.rtc ? "on" : "off"));
    return out;
}

void draw_stats(Picture picture, const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    std::size_t widest = 0;
    for (const auto& l : lines) widest = std::max(widest, std::min(l.size(), static_cast<std::size_t>(kColumns - 1)));
    const int height = static_cast<int>(lines.size()) * kCellHeight + 2;
    shade(picture, 0, 0, static_cast<int>(widest) * kCellWidth + 4, height);
    int y = 1;
    for (const auto& l : lines) {
        draw_text(picture, 2, y, to_ascii(l).substr(0, kColumns - 1), color::kGreen);
        y += kCellHeight;
    }
}

}  // namespace anygba::player
