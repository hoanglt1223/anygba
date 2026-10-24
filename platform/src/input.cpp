#include <anyport/platform/input.h>

#include "sdl_error.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>

namespace anyport::platform {

namespace {

constexpr std::int16_t kTriggerThreshold = 16384;
static_assert(SDL_SCANCODE_COUNT <= 512);

struct GamepadButtonMapping {
    SDL_GamepadButton sdl;
    Button button;
};

constexpr GamepadButtonMapping kGamepadButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, Button::South},
    {SDL_GAMEPAD_BUTTON_EAST, Button::East},
    {SDL_GAMEPAD_BUTTON_WEST, Button::West},
    {SDL_GAMEPAD_BUTTON_NORTH, Button::North},
    {SDL_GAMEPAD_BUTTON_BACK, Button::Back},
    {SDL_GAMEPAD_BUTTON_GUIDE, Button::Guide},
    {SDL_GAMEPAD_BUTTON_START, Button::Start},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, Button::LeftStick},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, Button::RightStick},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Button::LeftShoulder},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Button::RightShoulder},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, Button::DpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Button::DpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Button::DpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Button::DpadRight},
};

constexpr SDL_GamepadAxis kGamepadAxes[] = {
    SDL_GAMEPAD_AXIS_LEFTX,       SDL_GAMEPAD_AXIS_LEFTY,       SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,      SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};
static_assert(std::size(kGamepadAxes) == static_cast<std::size_t>(Axis::Count));

constexpr std::pair<const char*, Button> kDefaultKeys[] = {
    {"Up", Button::DpadUp},       {"Down", Button::DpadDown},
    {"Left", Button::DpadLeft},   {"Right", Button::DpadRight},
    {"X", Button::South},         {"Z", Button::East},
    {"A", Button::West},          {"S", Button::North},
    {"Q", Button::LeftShoulder},  {"W", Button::RightShoulder},
    {"Return", Button::Start},    {"Backspace", Button::Back},
};

constexpr std::uint32_t bit(Button button) {
    return 1u << static_cast<unsigned>(button);
}

int scancode_from_name(std::string_view key_name) {
    const std::string name(key_name);
    return static_cast<int>(SDL_GetScancodeFromName(name.c_str()));
}

}  // namespace

Input::Input() {
    for (const auto& [name, button] : kDefaultKeys) {
        if (!bind_key(name, button)) {
            throw Error(std::string("unknown default key name: ") + name);
        }
    }
}

Input::~Input() {
    for (auto& [id, pad] : gamepads_) {
        SDL_CloseGamepad(pad);
    }
}

bool Input::bind_key(std::string_view key_name, Button button) {
    if (button >= Button::Count) {
        return false;
    }
    const int scancode = scancode_from_name(key_name);
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= static_cast<int>(kScancodeCount)) {
        return false;
    }
    key_bindings_.emplace_back(scancode, button);
    return true;
}

void Input::clear_key_bindings() {
    key_bindings_.clear();
}

bool Input::key_held(std::string_view key_name) const {
    const int scancode = scancode_from_name(key_name);
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= static_cast<int>(kScancodeCount)) {
        return false;
    }
    return keys_down_[static_cast<std::size_t>(scancode)];
}

PadState Input::state() const {
    PadState state;
    for (const auto& [scancode, button] : key_bindings_) {
        if (keys_down_[static_cast<std::size_t>(scancode)]) {
            state.buttons |= bit(button);
        }
    }
    for (const auto& [id, pad] : gamepads_) {
        for (const auto& mapping : kGamepadButtons) {
            if (SDL_GetGamepadButton(pad, mapping.sdl)) {
                state.buttons |= bit(mapping.button);
            }
        }
        // Several pads: keep the most deflected value per axis.
        for (std::size_t i = 0; i < std::size(kGamepadAxes); ++i) {
            const std::int16_t value = SDL_GetGamepadAxis(pad, kGamepadAxes[i]);
            if (std::abs(value) > std::abs(state.axes[i])) {
                state.axes[i] = value;
            }
        }
    }
    if (state.axis(Axis::LeftTrigger) > kTriggerThreshold) {
        state.buttons |= bit(Button::LeftTrigger);
    }
    if (state.axis(Axis::RightTrigger) > kTriggerThreshold) {
        state.buttons |= bit(Button::RightTrigger);
    }
    return state;
}

void Input::handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const auto scancode = static_cast<std::size_t>(event.key.scancode);
        if (scancode < kScancodeCount) {
            keys_down_[scancode] = event.type == SDL_EVENT_KEY_DOWN;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && key_presses_.size() < 64) {
            key_presses_.emplace_back(SDL_GetScancodeName(event.key.scancode));
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        keys_down_.fill(false);
        break;
    case SDL_EVENT_GAMEPAD_ADDED: {
        const SDL_JoystickID id = event.gdevice.which;
        const bool known = std::any_of(gamepads_.begin(), gamepads_.end(),
                                       [id](const auto& entry) { return entry.first == id; });
        if (known) {
            break;
        }
        // A pad that fails to open is skipped rather than aborting the session.
        if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
            gamepads_.emplace_back(id, pad);
            const char* name = SDL_GetGamepadName(pad);
            gamepad_events_.push_back({true, name ? name : "unknown controller"});
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "SDL_OpenGamepad(%u) failed: %s",
                        static_cast<unsigned>(id), SDL_GetError());
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const SDL_JoystickID id = event.gdevice.which;
        const auto it = std::find_if(gamepads_.begin(), gamepads_.end(),
                                     [id](const auto& entry) { return entry.first == id; });
        if (it != gamepads_.end()) {
            const char* name = SDL_GetGamepadName(it->second);
            gamepad_events_.push_back({false, name ? name : "unknown controller"});
            SDL_CloseGamepad(it->second);
            gamepads_.erase(it);
        }
        break;
    }
    default:
        break;
    }
}

std::vector<std::string> Input::take_key_presses() { return std::exchange(key_presses_, {}); }

std::vector<GamepadEvent> Input::take_gamepad_events() { return std::exchange(gamepad_events_, {}); }

int add_gamepad_mappings(const std::string& path_utf8) {
    const int added = SDL_AddGamepadMappingsFromFile(path_utf8.c_str());
    if (added < 0) {
        detail::throw_sdl_error("SDL_AddGamepadMappingsFromFile");
    }
    return added;
}

bool pump_events(Input& input) {
    bool keep_running = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            keep_running = false;
        }
        input.handle_event(event);
    }
    return keep_running;
}

}  // namespace anyport::platform
