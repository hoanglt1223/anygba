#pragma once

#include "input_map.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Scripted input for headless runs and tests: keyboard keys, gamepad buttons, the left
// stick and GBA buttons, changed at given host frames (docs/user/USAGE.md "Scripted input").
// Host frames count every VBlank and every frame the menu is open.
namespace anygba::player {

class Script final : public input::Source {
public:
    // One event per line: "<frame> key|pad|gba down|up|tap <name>", "<frame> stick <x> <y>",
    // "<frame> type <text>", "<frame> connect|disconnect <name>". Throws std::runtime_error
    // naming the source and line.
    static Script parse(std::string_view text, std::string_view source_name);
    // anygba-shot's --keys syntax: "a:10-20,start:30-31" (GBA buttons held over frame ranges).
    static Script from_keys(std::string_view spec);
    void append(const Script& other);

    // Applies the events of host frame `frame` (frames start at 1).
    void advance(std::uint64_t frame);
    std::uint64_t last_frame() const;

    bool key_held(std::string_view key_name) const override;
    anyport::platform::PadState pad() const override;
    std::uint16_t gba_held() const { return gba_; }  // buttons pressed by `gba` events
    std::vector<std::string> take_key_presses();
    std::vector<anyport::platform::GamepadEvent> take_gamepad_events();

private:
    struct Event {
        std::uint64_t frame = 0;
        enum class Kind : std::uint8_t { KeyDown, KeyUp, PadDown, PadUp, GbaDown, GbaUp, Stick, Connect, Disconnect };
        Kind kind = Kind::KeyDown;
        std::string name;
        int x = 0, y = 0;
    };
    void add(Event e) { events_.push_back(std::move(e)); }

    std::vector<Event> events_;
    std::size_t next_ = 0;
    bool sorted_ = false;
    std::set<std::string> keys_;
    anyport::platform::PadState pad_;
    std::uint16_t gba_ = 0;
    std::vector<std::string> presses_;
    std::vector<anyport::platform::GamepadEvent> gamepad_events_;
};

}  // namespace anygba::player
