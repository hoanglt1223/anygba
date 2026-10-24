#include "player/script.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace anygba::player {

namespace {

using anyport::platform::Axis;
using anyport::platform::Button;

std::string canonical_key(const std::string& name) {
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? std::string() : std::string(SDL_GetScancodeName(code));
}

int gba_index(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i < input::kGbaButtons; ++i) {
        std::string n(input::kGbaButtonNames[i]);
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n == name) return i;
    }
    return -1;
}

std::vector<std::string> split_plus(const std::string& chord) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= chord.size()) {
        const auto plus = chord.find('+', start);
        std::string part = chord.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        // "Keypad +": a trailing '+' after a space belongs to the key name.
        if (plus != std::string::npos && plus + 1 == chord.size()) {
            parts.push_back(chord.substr(start));
            break;
        }
        parts.push_back(part);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return parts;
}

}  // namespace

Script Script::parse(std::string_view text, std::string_view source_name) {
    Script s;
    std::istringstream in{std::string(text)};
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto fail = [&](const std::string& message) {
            throw std::runtime_error(std::string(source_name) + ":" + std::to_string(line_no) + ": " + message);
        };
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream words(line);
        std::uint64_t frame = 0;
        std::string verb;
        if (!(words >> frame) || frame == 0 || !(words >> verb)) fail("expected '<frame> <action> ...'");
        std::string rest;
        std::getline(words >> std::ws, rest);

        if (verb == "type") {
            std::uint64_t f = frame;
            for (const char ch : rest) {
                if (!std::isalnum(static_cast<unsigned char>(ch))) {
                    fail(std::string("type: only letters and digits can be typed, got '") + ch + "'");
                }
                const std::string key(1, static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
                s.add({f, Event::Kind::KeyDown, key});
                s.add({f + 1, Event::Kind::KeyUp, key});
                f += 2;
            }
            continue;
        }
        if (verb == "stick") {
            std::istringstream xy(rest);
            int x = 0, y = 0;
            if (!(xy >> x >> y) || x < -32768 || x > 32767 || y < -32768 || y > 32767) fail("stick needs x and y in -32768..32767");
            Event e{frame, Event::Kind::Stick, {}};
            e.x = x;
            e.y = y;
            s.add(e);
            continue;
        }
        if (verb == "connect" || verb == "disconnect") {
            if (rest.empty()) fail(verb + " needs a controller name");
            s.add({frame, verb == "connect" ? Event::Kind::Connect : Event::Kind::Disconnect, rest});
            continue;
        }
        if (verb != "key" && verb != "pad" && verb != "gba") fail("unknown action '" + verb + "'");
        std::istringstream args(rest);
        std::string how;
        std::string name;
        args >> how;
        std::getline(args >> std::ws, name);
        if ((how != "down" && how != "up" && how != "tap") || name.empty()) {
            fail(verb + " needs down, up or tap and a name");
        }
        std::vector<std::string> names;
        for (const std::string& part : split_plus(name)) {
            std::string n = part;
            if (verb == "key") {
                // Chord modifiers stand for their left-hand keys.
                if (n == "Shift" || n == "Ctrl" || n == "Alt") n = "Left " + n;
                n = canonical_key(n);
                if (n.empty()) fail("unknown key name '" + part + "'");
            } else if (verb == "pad") {
                bool known = false;
                for (int b = 0; b < static_cast<int>(Button::Count); ++b) {
                    known = known || input::pad_button_name(static_cast<Button>(b)) == n;
                }
                if (!known) fail("unknown gamepad button '" + part + "'");
            } else if (gba_index(n) < 0) {
                fail("unknown GBA button '" + part + "'");
            }
            names.push_back(n);
        }
        const Event::Kind down = verb == "key" ? Event::Kind::KeyDown : verb == "pad" ? Event::Kind::PadDown : Event::Kind::GbaDown;
        const Event::Kind up = verb == "key" ? Event::Kind::KeyUp : verb == "pad" ? Event::Kind::PadUp : Event::Kind::GbaUp;
        for (const auto& n : names) {
            if (how != "up") s.add({frame, down, n});
            if (how == "up") s.add({frame, up, n});
            if (how == "tap") s.add({frame + 1, up, n});
        }
    }
    return s;
}

Script Script::from_keys(std::string_view spec) {
    Script s;
    std::string text(spec);
    std::istringstream items(text);
    std::string item;
    while (std::getline(items, item, ',')) {
        const auto colon = item.find(':');
        const auto dash = item.find('-', colon == std::string::npos ? 0 : colon);
        if (colon == std::string::npos || dash == std::string::npos) {
            throw std::runtime_error("--keys: expected BUTTON:FIRST-LAST, got '" + item + "'");
        }
        const std::string name = item.substr(0, colon);
        char* end = nullptr;
        const auto first = std::strtoull(item.c_str() + colon + 1, &end, 10);
        const auto last = std::strtoull(item.c_str() + dash + 1, &end, 10);
        if (gba_index(name) < 0 || first == 0 || last < first || *end != '\0') {
            throw std::runtime_error("--keys: bad entry '" + item + "'");
        }
        s.add({first, Event::Kind::GbaDown, name});
        s.add({last + 1, Event::Kind::GbaUp, name});
    }
    return s;
}

void Script::append(const Script& other) {
    events_.insert(events_.end(), other.events_.begin(), other.events_.end());
    sorted_ = false;
}

std::uint64_t Script::last_frame() const {
    std::uint64_t last = 0;
    for (const Event& e : events_) last = std::max(last, e.frame);
    return last;
}

void Script::advance(std::uint64_t frame) {
    if (!sorted_) {
        std::stable_sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) { return a.frame < b.frame; });
        sorted_ = true;
        next_ = 0;
        while (next_ < events_.size() && events_[next_].frame < frame) ++next_;
    }
    for (; next_ < events_.size() && events_[next_].frame <= frame; ++next_) {
        const Event& e = events_[next_];
        switch (e.kind) {
        case Event::Kind::KeyDown:
            if (keys_.insert(e.name).second) presses_.push_back(e.name);
            break;
        case Event::Kind::KeyUp: keys_.erase(e.name); break;
        case Event::Kind::PadDown:
        case Event::Kind::PadUp:
            for (int b = 0; b < static_cast<int>(Button::Count); ++b) {
                if (input::pad_button_name(static_cast<Button>(b)) == e.name) {
                    const std::uint32_t bit = 1u << b;
                    pad_.buttons = e.kind == Event::Kind::PadDown ? pad_.buttons | bit : pad_.buttons & ~bit;
                }
            }
            break;
        case Event::Kind::GbaDown: gba_ = static_cast<std::uint16_t>(gba_ | (1u << gba_index(e.name))); break;
        case Event::Kind::GbaUp: gba_ = static_cast<std::uint16_t>(gba_ & ~(1u << gba_index(e.name))); break;
        case Event::Kind::Stick:
            pad_.axes[static_cast<std::size_t>(Axis::LeftX)] = static_cast<std::int16_t>(e.x);
            pad_.axes[static_cast<std::size_t>(Axis::LeftY)] = static_cast<std::int16_t>(e.y);
            break;
        case Event::Kind::Connect: gamepad_events_.push_back({true, e.name}); break;
        case Event::Kind::Disconnect: gamepad_events_.push_back({false, e.name}); break;
        }
    }
}

bool Script::key_held(std::string_view key_name) const { return keys_.contains(std::string(key_name)); }

anyport::platform::PadState Script::pad() const { return pad_; }

std::vector<std::string> Script::take_key_presses() { return std::exchange(presses_, {}); }

std::vector<anyport::platform::GamepadEvent> Script::take_gamepad_events() { return std::exchange(gamepad_events_, {}); }

}  // namespace anygba::player
