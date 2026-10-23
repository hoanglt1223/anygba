#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;

namespace anyport::platform {

// Host-side virtual pad in SDL gamepad terms (positional face buttons). Mapping these to
// a guest console's buttons belongs to the caller.
enum class Button : std::uint8_t {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftTrigger,
    RightTrigger,
    Count,
};

enum class Axis : std::uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

struct PadState {
    std::uint32_t buttons = 0;
    std::array<std::int16_t, static_cast<std::size_t>(Axis::Count)> axes{};

    bool held(Button button) const noexcept {
        return ((buttons >> static_cast<unsigned>(button)) & 1u) != 0;
    }
    std::int16_t axis(Axis a) const noexcept { return axes[static_cast<std::size_t>(a)]; }
};

// Keyboard plus every connected gamepad, merged into one PadState.
// Default keys: arrows = d-pad, X = South, Z = East, A = West, S = North,
// Q = LeftShoulder, W = RightShoulder, Return = Start, Backspace = Back.
class Input {
public:
    Input();
    ~Input();

    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // Key names follow SDL_GetScancodeFromName ("X", "Left Shift", "Return").
    // Returns false and changes nothing when the name is unknown.
    bool bind_key(std::string_view key_name, Button button);
    void clear_key_bindings();

    bool key_held(std::string_view key_name) const;
    PadState state() const;
    std::size_t gamepad_count() const noexcept { return gamepads_.size(); }

    void handle_event(const SDL_Event& event);

private:
    static constexpr std::size_t kScancodeCount = 512;

    std::vector<std::pair<int, Button>> key_bindings_;
    std::array<bool, kScancodeCount> keys_down_{};
    std::vector<std::pair<std::uint32_t, SDL_Gamepad*>> gamepads_;
};

// Drains pending events into `input`. Returns false once the user asked to quit.
bool pump_events(Input& input);

}  // namespace anyport::platform
