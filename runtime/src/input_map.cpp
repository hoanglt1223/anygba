#include "input_map.h"

#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_keyboard.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace anygba::input {

namespace {

using anyport::platform::Button;

constexpr std::pair<std::string_view, Button> kPadNames[] = {
    {"South", Button::South},
    {"East", Button::East},
    {"West", Button::West},
    {"North", Button::North},
    {"Back", Button::Back},
    {"Guide", Button::Guide},
    {"Start", Button::Start},
    {"LeftStick", Button::LeftStick},
    {"RightStick", Button::RightStick},
    {"LeftShoulder", Button::LeftShoulder},
    {"RightShoulder", Button::RightShoulder},
    {"DpadUp", Button::DpadUp},
    {"DpadDown", Button::DpadDown},
    {"DpadLeft", Button::DpadLeft},
    {"DpadRight", Button::DpadRight},
    {"LeftTrigger", Button::LeftTrigger},
    {"RightTrigger", Button::RightTrigger},
};

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

std::vector<std::string> split_list(std::string_view value) {
    std::vector<std::string> items;
    while (!value.empty()) {
        const auto comma = value.find(',');
        const std::string_view item = trim(value.substr(0, comma));
        if (!item.empty()) {
            items.emplace_back(item);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return items;
}

const char* pad_name(Button b) {
    for (const auto& [name, button] : kPadNames) {
        if (button == b) return name.data();
    }
    return "?";
}

}  // namespace

Mapping default_mapping() {
    Mapping m;
    m.keys = {{{"X"}, {"Z"}, {"Backspace"}, {"Return"}, {"Right"}, {"Left"}, {"Up"}, {"Down"}, {"W"}, {"Q"}}};
    m.pad = {{{Button::South},
              {Button::East},
              {Button::Back},
              {Button::Start},
              {Button::DpadRight},
              {Button::DpadLeft},
              {Button::DpadUp},
              {Button::DpadDown},
              {Button::RightShoulder},
              {Button::LeftShoulder}}};
    m.fast_forward = {"Tab"};
    return m;
}

Mapping parse_mapping(std::string_view text, std::string_view source_name) {
    Mapping m = default_mapping();
    enum class Section { None, Keyboard, Gamepad } section = Section::None;
    std::size_t line_no = 0;
    const auto fail = [&](const std::string& message) {
        throw std::runtime_error(std::string(source_name) + ":" + std::to_string(line_no) + ": " + message);
    };
    while (!text.empty()) {
        ++line_no;
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[') {
            if (line == "[keyboard]") {
                section = Section::Keyboard;
            } else if (line == "[gamepad]") {
                section = Section::Gamepad;
            } else {
                fail("unknown section " + std::string(line) + " (expected [keyboard] or [gamepad])");
            }
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            fail("expected 'Button = ...'");
        }
        if (section == Section::None) {
            fail("binding outside a [keyboard] or [gamepad] section");
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::vector<std::string> values = split_list(line.substr(eq + 1));
        if (key == "FastForward") {
            if (section != Section::Keyboard) {
                fail("FastForward can only be bound in [keyboard]");
            }
            for (const auto& v : values) {
                if (SDL_GetScancodeFromName(v.c_str()) == SDL_SCANCODE_UNKNOWN) fail("unknown key name '" + v + "'");
            }
            m.fast_forward = values;
            continue;
        }
        int index = -1;
        for (int i = 0; i < kGbaButtons; ++i) {
            if (kGbaButtonNames[i] == key) index = i;
        }
        if (index < 0) {
            fail("unknown GBA button '" + std::string(key) + "' (A, B, Select, Start, Right, Left, Up, Down, R, L, FastForward)");
        }
        if (section == Section::Keyboard) {
            for (const auto& v : values) {
                if (SDL_GetScancodeFromName(v.c_str()) == SDL_SCANCODE_UNKNOWN) fail("unknown key name '" + v + "'");
            }
            m.keys[index] = values;
        } else {
            m.pad[index].clear();
            for (const auto& v : values) {
                bool found = false;
                for (const auto& [name, button] : kPadNames) {
                    if (name == v) {
                        m.pad[index].push_back(button);
                        found = true;
                    }
                }
                if (!found) fail("unknown gamepad button '" + v + "'");
            }
        }
    }
    return m;
}

Mapping load_mapping(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open input mapping " + path.string());
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parse_mapping(text.str(), path.string());
}

std::string format_mapping(const Mapping& m) {
    std::string out = "[keyboard]\n";
    const auto join = [](const auto& items, auto&& to_string) {
        std::string s;
        for (const auto& item : items) {
            s += (s.empty() ? "" : ", ") + std::string(to_string(item));
        }
        return s;
    };
    for (int i = 0; i < kGbaButtons; ++i) {
        out += std::string(kGbaButtonNames[i]) + " = " + join(m.keys[i], [](const std::string& k) { return k; }) + "\n";
    }
    out += "FastForward = " + join(m.fast_forward, [](const std::string& k) { return k; }) + "\n\n[gamepad]\n";
    for (int i = 0; i < kGbaButtons; ++i) {
        out += std::string(kGbaButtonNames[i]) + " = " + join(m.pad[i], [](Button b) { return pad_name(b); }) + "\n";
    }
    return out;
}

std::uint16_t gba_keys(const Mapping& m, const anyport::platform::Input& input) {
    const anyport::platform::PadState pad = input.state();
    std::uint16_t keys = 0;
    for (int i = 0; i < kGbaButtons; ++i) {
        bool held = false;
        for (const auto& k : m.keys[i]) held = held || input.key_held(k);
        for (const Button b : m.pad[i]) held = held || pad.held(b);
        if (held) keys = static_cast<std::uint16_t>(keys | (1u << i));
    }
    return keys;
}

bool fast_forward_held(const Mapping& m, const anyport::platform::Input& input) {
    for (const auto& k : m.fast_forward) {
        if (input.key_held(k)) return true;
    }
    return false;
}

}  // namespace anygba::input
