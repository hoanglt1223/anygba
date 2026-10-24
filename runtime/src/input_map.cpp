#include "input_map.h"

#include "player/speed.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace anygba::input {

namespace {

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

constexpr std::string_view kModifiers[] = {"Ctrl", "Alt", "Shift"};
constexpr int kRight = 4, kLeft = 5, kUp = 6, kDown = 7;

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

std::vector<std::string> split(std::string_view value, char separator) {
    std::vector<std::string> items;
    while (!value.empty()) {
        const auto at = value.find(separator);
        const std::string_view item = trim(value.substr(0, at));
        if (!item.empty()) {
            items.emplace_back(item);
        }
        if (at == std::string_view::npos) {
            break;
        }
        value.remove_prefix(at + 1);
    }
    return items;
}

std::optional<Button> pad_button(std::string_view name) {
    for (const auto& [n, b] : kPadNames) {
        if (n == name) return b;
    }
    return std::nullopt;
}

std::string canonical_key(std::string_view name) {
    const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
    if (code == SDL_SCANCODE_UNKNOWN) {
        return {};
    }
    return SDL_GetScancodeName(code);
}

bool is_modifier_key(std::string_view key) {
    return key == "Left Shift" || key == "Right Shift" || key == "Left Ctrl" || key == "Right Ctrl" ||
           key == "Left Alt" || key == "Right Alt";
}

struct KeyChord {
    bool ctrl = false, alt = false, shift = false;
    std::string key;
};

// "Shift+F1" -> {shift, "F1"}. A key name that itself contains '+' ("Keypad +") stays whole.
std::optional<KeyChord> parse_key_chord(std::string_view text) {
    KeyChord chord;
    for (;;) {
        bool matched = false;
        for (const std::string_view mod : kModifiers) {
            if (text.size() > mod.size() + 1 && text.substr(0, mod.size()) == mod && text[mod.size()] == '+') {
                (mod == "Ctrl" ? chord.ctrl : mod == "Alt" ? chord.alt : chord.shift) = true;
                text.remove_prefix(mod.size() + 1);
                matched = true;
            }
        }
        if (!matched) break;
    }
    chord.key = canonical_key(trim(text));
    if (chord.key.empty()) {
        return std::nullopt;
    }
    return chord;
}

std::optional<std::vector<Button>> parse_pad_chord(std::string_view text) {
    std::vector<Button> buttons;
    for (const std::string& part : split(text, '+')) {
        const auto b = pad_button(part);
        if (!b) return std::nullopt;
        buttons.push_back(*b);
    }
    if (buttons.empty()) return std::nullopt;
    return buttons;
}

bool modifiers_match(const KeyChord& chord, const Source& source) {
    const bool ctrl = source.key_held("Left Ctrl") || source.key_held("Right Ctrl");
    const bool alt = source.key_held("Left Alt") || source.key_held("Right Alt");
    const bool shift = source.key_held("Left Shift") || source.key_held("Right Shift");
    return ctrl == chord.ctrl && alt == chord.alt && shift == chord.shift;
}

bool key_chord_held(std::string_view text, const Source& source) {
    const auto chord = parse_key_chord(text);
    if (!chord || !source.key_held(chord->key)) return false;
    return is_modifier_key(chord->key) || modifiers_match(*chord, source);
}

bool pad_chord_held(std::string_view text, const anyport::platform::PadState& pad) {
    const auto buttons = parse_pad_chord(text);
    return buttons && std::all_of(buttons->begin(), buttons->end(), [&](Button b) { return pad.held(b); });
}

}  // namespace

std::string hotkey_name(Hotkey hotkey) {
    const int i = static_cast<int>(hotkey);
    if (i < 9) return "SaveState" + std::to_string(i + 1);
    if (i < 18) return "LoadState" + std::to_string(i - 8);
    switch (hotkey) {
    case Hotkey::SpeedUp: return "SpeedUp";
    case Hotkey::SpeedDown: return "SpeedDown";
    case Hotkey::Menu: return "Menu";
    case Hotkey::FastForward: return "FastForward";
    case Hotkey::CheatButton: return "CheatButton";
    case Hotkey::AiControl: return "AiControl";
    case Hotkey::Journal: return "Journal";
    case Hotkey::Stats: return "Stats";
    case Hotkey::Smoothing: return "Smoothing";
    default: return "?";
    }
}

const char* pad_button_name(Button b) {
    for (const auto& [name, button] : kPadNames) {
        if (button == b) return name.data();
    }
    return "?";
}

Mapping default_mapping() {
    Mapping m;
    m.keys = {{{"Z"}, {"X"}, {"Backspace"}, {"Return"}, {"Right"}, {"Left"}, {"Up"}, {"Down"}, {"S"}, {"A"}}};
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
    for (int slot = 1; slot <= 9; ++slot) {
        m.hotkeys[static_cast<int>(save_slot_key(slot))] = {"Shift+F" + std::to_string(slot)};
        m.hotkeys[static_cast<int>(load_slot_key(slot))] = {"F" + std::to_string(slot)};
    }
    const auto set = [&m](Hotkey h, std::vector<std::string> keys, std::vector<std::string> pad) {
        m.hotkeys[static_cast<int>(h)] = std::move(keys);
        m.pad_hotkeys[static_cast<int>(h)] = std::move(pad);
    };
    set(Hotkey::SpeedUp, {"="}, {"Back+DpadUp"});
    set(Hotkey::SpeedDown, {"-"}, {"Back+DpadDown"});
    set(Hotkey::Menu, {"Escape"}, {"Back+Start"});
    set(Hotkey::FastForward, {"Space"}, {"RightTrigger"});
    set(Hotkey::CheatButton, {"F12"}, {});
    set(Hotkey::AiControl, {"F10"}, {});
    set(Hotkey::Journal, {"F11"}, {});
    set(Hotkey::Stats, {"Ctrl+F"}, {});
    set(Hotkey::Smoothing, {"Ctrl+I"}, {});
    m.pad_hotkeys[static_cast<int>(Hotkey::SaveState1)] = {"Back+RightShoulder"};
    m.pad_hotkeys[static_cast<int>(Hotkey::LoadState1)] = {"Back+LeftShoulder"};
    return m;
}

Mapping parse_mapping(std::string_view text, std::string_view source_name) {
    Mapping m = default_mapping();
    enum class Section { None, Keyboard, Gamepad, Player } section = Section::None;
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
            } else if (line == "[player]") {
                section = Section::Player;
            } else {
                fail("unknown section " + std::string(line) + " (expected [keyboard], [gamepad] or [player])");
            }
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            fail("expected 'Name = ...'");
        }
        if (section == Section::None) {
            fail("setting outside a [keyboard], [gamepad] or [player] section");
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (section == Section::Player) {
            char* end = nullptr;
            const std::string v(value);
            const long n = std::strtol(v.c_str(), &end, 10);
            if (key == "Speed") {
                if (*end != '\0' || (n != 100 && n != 200 && n != 300 && n != 400)) fail("Speed must be 100, 200, 300 or 400");
                m.speed = static_cast<int>(n);
            } else if (key == "FastForwardSpeed") {
                const auto ff = player::parse_fast_forward(value);
                if (!ff) fail("FastForwardSpeed must be 150, 200, 300, 400, 500, 800 or unlimited");
                m.fast_forward = *ff;
            } else if (key == "Stats") {
                if (value != "on" && value != "off") fail("Stats must be on or off");
                m.stats = value == "on";
            } else if (key == "NormalSpeedMusic") {
                if (value != "on" && value != "off") fail("NormalSpeedMusic must be on or off");
                m.normal_speed_music = value == "on";
            } else if (key == "SaveFolder") {
                m.save_folder = std::string(value);
            } else if (key == "StickDeadzone") {
                if (*end != '\0' || n < 0 || n > 32767) fail("StickDeadzone must be 0 to 32767");
                m.stick_deadzone = static_cast<int>(n);
            } else if (key == "Volume") {
                if (v.empty() || *end != '\0' || n < 0 || n > 100 || n % 10 != 0) fail("Volume must be 0 to 100 in steps of 10");
                m.volume = static_cast<int>(n);
            } else if (key == "Mute") {
                if (value != "on" && value != "off") fail("Mute must be on or off");
                m.mute = value == "on";
            } else if (key == "Language") {
                if (value != "en" && value != "vi") fail("Language must be en or vi");
                m.language = std::string(value);
            } else if (key == "Theme") {
                if (value != "dark" && value != "light") fail("Theme must be dark or light");
                m.light_theme = value == "light";
            } else if (key == "UiScale") {
                if (v.empty() || *end != '\0' || n < 75 || n > 150 || n % 25 != 0) fail("UiScale must be 75, 100, 125 or 150");
                m.ui_scale = static_cast<int>(n);
            } else if (key == "ReduceMotion") {
                if (value != "on" && value != "off") fail("ReduceMotion must be on or off");
                m.reduce_motion = value == "on";
            } else {
                fail("unknown [player] setting '" + std::string(key) +
                     "' (Speed, FastForwardSpeed, NormalSpeedMusic, Stats, StickDeadzone, SaveFolder, Volume, Mute, "
                     "Language, Theme, UiScale, ReduceMotion)");
            }
            continue;
        }
        // Value lists: a key name may contain neither ',' nor be empty.
        const std::vector<std::string> values = split(value, ',');
        int button = -1;
        for (int i = 0; i < kGbaButtons; ++i) {
            if (kGbaButtonNames[i] == key) button = i;
        }
        int hotkey = -1;
        for (int i = 0; i < kHotkeys; ++i) {
            if (hotkey_name(static_cast<Hotkey>(i)) == key) hotkey = i;
        }
        if (button < 0 && hotkey < 0) {
            fail("unknown name '" + std::string(key) +
                 "' (GBA buttons A, B, Select, Start, Right, Left, Up, Down, R, L; hotkeys SaveState1-9, LoadState1-9, "
                 "SpeedUp, SpeedDown, Menu, FastForward, CheatButton, AiControl, Journal, Stats, Smoothing)");
        }
        if (section == Section::Keyboard) {
            std::vector<std::string> keys;
            for (const auto& v : values) {
                if (button >= 0) {
                    const std::string k = canonical_key(v);
                    if (k.empty()) fail("unknown key name '" + v + "'");
                    keys.push_back(k);
                } else {
                    const auto chord = parse_key_chord(v);
                    if (!chord) fail("unknown key name in '" + v + "'");
                    keys.push_back(keyboard_chord(chord->key, chord->ctrl, chord->alt, chord->shift));
                }
            }
            (button >= 0 ? m.keys[button] : m.hotkeys[hotkey]) = std::move(keys);
        } else if (button >= 0) {
            m.pad[button].clear();
            for (const auto& v : values) {
                const auto b = pad_button(v);
                if (!b) fail("unknown gamepad button '" + v + "'");
                m.pad[button].push_back(*b);
            }
        } else {
            std::vector<std::string> chords;
            for (const auto& v : values) {
                const auto buttons = parse_pad_chord(v);
                if (!buttons) fail("unknown gamepad button in '" + v + "'");
                chords.push_back(pad_chord(*buttons));
            }
            m.pad_hotkeys[hotkey] = std::move(chords);
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
    const auto join = [](const std::vector<std::string>& items) {
        std::string s;
        for (const auto& item : items) s += (s.empty() ? "" : ", ") + item;
        return s;
    };
    std::string out = "[keyboard]\n";
    for (int i = 0; i < kGbaButtons; ++i) {
        out += std::string(kGbaButtonNames[i]) + " = " + join(m.keys[i]) + "\n";
    }
    for (int i = 0; i < kHotkeys; ++i) {
        out += hotkey_name(static_cast<Hotkey>(i)) + " = " + join(m.hotkeys[i]) + "\n";
    }
    out += "\n[gamepad]\n";
    for (int i = 0; i < kGbaButtons; ++i) {
        std::vector<std::string> names;
        for (const Button b : m.pad[i]) names.emplace_back(pad_button_name(b));
        out += std::string(kGbaButtonNames[i]) + " = " + join(names) + "\n";
    }
    for (int i = 0; i < kHotkeys; ++i) {
        out += hotkey_name(static_cast<Hotkey>(i)) + " = " + join(m.pad_hotkeys[i]) + "\n";
    }
    out += "\n[player]\nSpeed = " + std::to_string(m.speed) +
           "\nFastForwardSpeed = " + player::format_fast_forward(m.fast_forward, false) +
           "\nNormalSpeedMusic = " + (m.normal_speed_music ? "on" : "off") +
           "\nStats = " + (m.stats ? "on" : "off") +
           "\nStickDeadzone = " + std::to_string(m.stick_deadzone) +
           "\nSaveFolder = " + m.save_folder +
           "\nVolume = " + std::to_string(m.volume) +
           "\nMute = " + (m.mute ? "on" : "off") +
           "\nLanguage = " + m.language +
           "\nTheme = " + (m.light_theme ? "light" : "dark") +
           "\nUiScale = " + std::to_string(m.ui_scale) +
           "\nReduceMotion = " + (m.reduce_motion ? "on" : "off") + "\n";
    return out;
}

std::optional<std::string> save_mapping(const Mapping& mapping, const std::filesystem::path& path) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "# AnyGBA input mapping (docs/user/INPUT_MAPPING.md)\n" << format_mapping(mapping);
        out.close();
        if (!out) return "cannot write " + tmp.string();
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return "cannot replace " + path.string() + ": " + ec.message();
    return std::nullopt;
}

std::uint16_t gba_keys(const Mapping& m, const Source& source) {
    const anyport::platform::PadState pad = source.pad();
    std::uint32_t chord_buttons = 0;  // pad buttons consumed by a held hotkey chord
    for (const auto& chords : m.pad_hotkeys) {
        for (const auto& chord : chords) {
            const auto buttons = parse_pad_chord(chord);
            if (buttons && buttons->size() > 1 && pad_chord_held(chord, pad)) {
                for (const Button b : *buttons) chord_buttons |= 1u << static_cast<unsigned>(b);
            }
        }
    }
    std::uint16_t keys = 0;
    for (int i = 0; i < kGbaButtons; ++i) {
        bool held = false;
        for (const auto& k : m.keys[i]) held = held || source.key_held(k);
        for (const Button b : m.pad[i]) {
            held = held || (pad.held(b) && !((chord_buttons >> static_cast<unsigned>(b)) & 1u));
        }
        if (held) keys = static_cast<std::uint16_t>(keys | (1u << i));
    }
    const int dz = m.stick_deadzone;
    const int x = pad.axis(anyport::platform::Axis::LeftX);
    const int y = pad.axis(anyport::platform::Axis::LeftY);
    if (x > dz) keys |= 1u << kRight;
    if (x < -dz) keys |= 1u << kLeft;
    if (y < -dz) keys |= 1u << kUp;
    if (y > dz) keys |= 1u << kDown;
    return keys;
}

std::uint32_t hotkeys_held(const Mapping& m, const Source& source) {
    const anyport::platform::PadState pad = source.pad();
    std::uint32_t held = 0;
    for (int i = 0; i < kHotkeys; ++i) {
        bool on = false;
        for (const auto& chord : m.hotkeys[i]) on = on || key_chord_held(chord, source);
        for (const auto& chord : m.pad_hotkeys[i]) on = on || pad_chord_held(chord, pad);
        if (on) held |= 1u << i;
    }
    return held;
}

std::string target_name(Target target) {
    return target.hotkey ? hotkey_name(static_cast<Hotkey>(target.index)) : std::string(kGbaButtonNames[target.index]);
}

std::string describe(const Mapping& m, Target target, Device device) {
    std::vector<std::string> names;
    if (device == Device::Keyboard) {
        names = target.hotkey ? m.hotkeys[target.index] : m.keys[target.index];
    } else if (target.hotkey) {
        names = m.pad_hotkeys[target.index];
    } else {
        for (const Button b : m.pad[target.index]) names.emplace_back(pad_button_name(b));
    }
    std::string s;
    for (const auto& n : names) s += (s.empty() ? "" : ", ") + n;
    return s;
}

std::string keyboard_chord(std::string_view key, bool ctrl, bool alt, bool shift) {
    const std::string k(key);
    if (is_modifier_key(k)) return k;
    return std::string(ctrl ? "Ctrl+" : "") + (alt ? "Alt+" : "") + (shift ? "Shift+" : "") + k;
}

std::string pad_chord(const std::vector<Button>& buttons) {
    std::vector<Button> sorted = buttons;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::string s;
    for (const Button b : sorted) s += (s.empty() ? "" : "+") + std::string(pad_button_name(b));
    return s;
}

std::optional<std::string> rebind(Mapping& m, Target target, Device device, const std::string& binding) {
    // Every binding of one device as (target, text) for conflict search.
    const auto list_of = [&m, device](Target t) -> std::vector<std::string>& {
        static thread_local std::vector<std::string> scratch;
        if (device == Device::Keyboard) return t.hotkey ? m.hotkeys[t.index] : m.keys[t.index];
        if (t.hotkey) return m.pad_hotkeys[t.index];
        scratch.clear();
        for (const Button b : m.pad[t.index]) scratch.emplace_back(pad_button_name(b));
        return scratch;
    };
    std::string value = binding;
    if (device == Device::Gamepad && !target.hotkey) {
        const auto buttons = parse_pad_chord(binding);
        if (!buttons) throw std::invalid_argument("unknown gamepad binding " + binding);
        value = pad_button_name(buttons->back());
    } else if (device == Device::Keyboard && !target.hotkey) {
        const auto chord = parse_key_chord(binding);  // GBA buttons ignore modifiers
        if (!chord) throw std::invalid_argument("unknown key " + binding);
        value = chord->key;
    }
    const std::vector<std::string> previous = list_of(target);

    std::optional<std::string> swapped;
    const auto visit = [&](Target other) {
        if (other == target) return;
        std::vector<std::string> list = list_of(other);
        const auto it = std::find(list.begin(), list.end(), value);
        if (it == list.end()) return;
        // Same kind of binding only: a GBA button cannot take a key chord.
        const bool can_take = !previous.empty() && (other.hotkey || previous.front().find('+') == std::string::npos);
        if (can_take) {
            *it = previous.front();
        } else {
            list.erase(it);
        }
        swapped = target_name(other);
        if (device == Device::Gamepad && !other.hotkey) {
            m.pad[other.index].clear();
            for (const auto& name : list) {
                if (const auto b = pad_button(name)) m.pad[other.index].push_back(*b);
            }
        } else {
            list_of(other) = list;
        }
    };
    for (int i = 0; i < kGbaButtons; ++i) visit({false, i});
    for (int i = 0; i < kHotkeys; ++i) visit({true, i});

    if (device == Device::Gamepad && !target.hotkey) {
        m.pad[target.index] = {*pad_button(value)};
    } else {
        list_of(target) = {value};
    }
    return swapped;
}

}  // namespace anygba::input
