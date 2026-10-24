#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>

// On-screen text for the player: a built-in 5x7 font drawn straight into the 240x160 RGB555
// picture, and timed messages (speed, state slots, cheats, gamepads).
namespace anygba::player {

using Picture = std::span<std::uint16_t>;  // 240x160, RGB555 with red in bits 0-4

inline constexpr int kCellWidth = 6;   // glyph plus one column of spacing
inline constexpr int kCellHeight = 9;  // glyph plus two rows of spacing
inline constexpr int kColumns = 240 / kCellWidth;

namespace color {
inline constexpr std::uint16_t kWhite = 0x7FFF;
inline constexpr std::uint16_t kGray = 0x56B5;
inline constexpr std::uint16_t kYellow = 0x03FF;
inline constexpr std::uint16_t kGreen = 0x03E0;
inline constexpr std::uint16_t kRed = 0x001F;
inline constexpr std::uint16_t kCyan = 0x7FE0;
}  // namespace color

// UTF-8 text folded to the font's printable ASCII: accented Latin letters lose their accent,
// anything else becomes '?'.
std::string to_ascii(std::string_view utf8);

// Draws `text` (ASCII) with its top-left corner at (x, y), clipped to the picture.
void draw_text(Picture picture, int x, int y, std::string_view text, std::uint16_t color);
// Darkens a rectangle so text over the game stays readable.
void shade(Picture picture, int x, int y, int width, int height);

class Osd {
public:
    // With `echo`, every message is also printed to stdout as "osd: <text>" (headless runs).
    explicit Osd(bool echo) : echo_(echo) {}

    // A message starting with a non-empty `replaces` prefix takes the place of earlier ones with it
    // (progress lines).
    void show(std::string text, int frames = 150, std::string_view replaces = {});
    // One host frame passes.
    void tick();
    // The newest messages, bottom-left, above the game picture.
    void draw(Picture picture) const;
    bool empty() const { return messages_.empty(); }

private:
    struct Message {
        std::string text;
        int frames_left = 0;
    };
    bool echo_;
    std::deque<Message> messages_;
};

}  // namespace anygba::player
