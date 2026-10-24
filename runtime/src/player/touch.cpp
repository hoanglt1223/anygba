#include "player/touch.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace anygba::player::touch {

namespace {

constexpr std::uint16_t kA = 1u << 0, kB = 1u << 1, kSelect = 1u << 2, kStart = 1u << 3, kRight = 1u << 4,
                        kLeft = 1u << 5, kUp = 1u << 6, kDown = 1u << 7, kR = 1u << 8, kL = 1u << 9;

constexpr float kGbaWidth = 240, kGbaHeight = 160;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool equals_nocase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return (x | 0x20) == (y | 0x20);
           });
}

FRect centred(float cx, float cy, float w, float h) { return {cx - w / 2, cy - h / 2, w, h}; }

float centre_x(const FRect& r) { return r.x + r.w / 2; }
float centre_y(const FRect& r) { return r.y + r.h / 2; }

bool inside(const FRect& r, float x, float y, float margin) {
    return x >= r.x - margin && x < r.x + r.w + margin && y >= r.y - margin && y < r.y + r.h + margin;
}

std::uint16_t dpad_at(const FRect& r, float x, float y) {
    const float radius = r.w / 2;
    const float dx = x - centre_x(r), dy = y - centre_y(r);
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < radius * 0.2f || dist > radius * 1.25f) return 0;
    // Screen y grows downwards; eight 45-degree sectors starting at Right, clockwise.
    constexpr float kPi = 3.14159265f;
    const int sector = static_cast<int>(std::lround(std::atan2(dy, dx) / (kPi / 4))) & 7;
    constexpr std::array<std::uint16_t, 8> kDirections = {
        kRight, kRight | kDown, kDown, kDown | kLeft, kLeft, kLeft | kUp, kUp, kUp | kRight};
    return kDirections[static_cast<std::size_t>(sector)];
}

bool in_circle(const FRect& r, float x, float y) {
    const float dx = x - centre_x(r), dy = y - centre_y(r);
    const float radius = r.w * 0.6f;  // a little past the drawn edge
    return dx * dx + dy * dy <= radius * radius;
}

// "<Control>X", "<Control>Y" or "<Control>Size"; false for any other name.
bool adjust_setting(Settings& s, std::string_view name, int n) {
    for (int i = 0; i < kControls; ++i) {
        const std::string_view key = key_name(static_cast<Control>(i));
        if (name.size() <= key.size() || !equals_nocase(name.substr(0, key.size()), key)) continue;
        const std::string_view field = name.substr(key.size());
        Adjust& a = s.adjust[static_cast<std::size_t>(i)];
        if (equals_nocase(field, "X")) {
            a.x = std::clamp(n, -1000, 1000);
        } else if (equals_nocase(field, "Y")) {
            a.y = std::clamp(n, -1000, 1000);
        } else if (equals_nocase(field, "Size")) {
            a.size = std::clamp(n, 50, 200);
        } else {
            continue;
        }
        return true;
    }
    return false;
}

}  // namespace

Settings parse_settings(std::string_view text) {
    Settings s;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (const auto comment = line.find_first_of("#;"); comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view name = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        int n = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (ec != std::errc{} || end != value.data() + value.size()) continue;
        if (adjust_setting(s, name, n)) continue;
        if (equals_nocase(name, "Size")) {
            s.size = std::clamp(n, 50, 200);
        } else if (equals_nocase(name, "Opacity")) {
            s.opacity = std::clamp(n, 10, 100);
        } else if (equals_nocase(name, "OffsetX")) {
            s.offset_x = std::clamp(n, -100, 100);
        } else if (equals_nocase(name, "OffsetY")) {
            s.offset_y = std::clamp(n, -100, 100);
        } else if (equals_nocase(name, "Haptics")) {
            s.haptics = n != 0;
        } else if (equals_nocase(name, "FloatingButton")) {
            s.floating_button = n != 0;
        } else if (equals_nocase(name, "ShowControls")) {
            s.show_controls = n != 0;
        } else if (equals_nocase(name, "ButtonEdge")) {
            s.button_edge = static_cast<Edge>(std::clamp(n, 0, 3));
        } else if (equals_nocase(name, "ButtonPos")) {
            s.button_pos = std::clamp(n, 0, 1000);
        }
    }
    return s;
}

std::string format_settings(const Settings& s) {
    std::string out = "# AnyGBA touch controls (docs/user/ANDROID.md)\n"
                      "Size = " + std::to_string(s.size) + "\n"
                      "Opacity = " + std::to_string(s.opacity) + "\n"
                      "OffsetX = " + std::to_string(s.offset_x) + "\n"
                      "OffsetY = " + std::to_string(s.offset_y) + "\n"
                      "Haptics = " + std::to_string(s.haptics ? 1 : 0) + "\n"
                      "FloatingButton = " + std::to_string(s.floating_button ? 1 : 0) + "\n"
                      "ShowControls = " + std::to_string(s.show_controls ? 1 : 0) + "\n"
                      "ButtonEdge = " + std::to_string(static_cast<int>(s.button_edge)) + "\n"
                      "ButtonPos = " + std::to_string(s.button_pos) + "\n";
    for (int i = 0; i < kControls; ++i) {
        const Adjust& a = s.adjust[static_cast<std::size_t>(i)];
        if (a == Adjust{}) continue;
        const std::string key = key_name(static_cast<Control>(i));
        out += key + "X = " + std::to_string(a.x) + "\n" + key + "Y = " + std::to_string(a.y) + "\n" + key +
               "Size = " + std::to_string(a.size) + "\n";
    }
    return out;
}

Layout layout(float width, float height, FRect safe, const Settings& settings, float picture_width) {
    if (safe.w <= 0 || safe.h <= 0) safe = {0, 0, width, height};
    if (!(picture_width >= kGbaWidth)) picture_width = kGbaWidth;
    Layout out;
    out.safe = safe;
    out.menu_button = !settings.floating_button;
    out.show_controls = settings.show_controls;
    const bool landscape = width >= height;
    float scale = landscape ? std::min(safe.w / picture_width, safe.h / kGbaHeight) : safe.w / picture_width;
    if (scale >= 1) scale = std::floor(scale);
    const float pw = picture_width * scale, ph = kGbaHeight * scale;
    FRect area = safe;  // where the controls go
    if (landscape) {
        out.picture = {safe.x + (safe.w - pw) / 2, safe.y + (safe.h - ph) / 2, pw, ph};
    } else {
        out.picture = {safe.x + (safe.w - pw) / 2, safe.y, pw, ph};
        area = {safe.x, safe.y + ph, safe.w, std::max(safe.h - ph, 0.0f)};
    }

    // A button unit, capped so the bottom row (d-pad, Select/Start, B/A) and the column (L/R
    // over the d-pad) never overlap however large the controls are set.
    const float wanted = std::min(area.w, area.h) * 0.12f * static_cast<float>(settings.size) / 100.0f;
    const float u = std::min({wanted, area.w / 11.0f, area.h / 5.2f});
    const float ox = static_cast<float>(settings.offset_x) / 100.0f * 2 * u;
    const float oy = static_cast<float>(settings.offset_y) / 100.0f * 2 * u;
    const float left = area.x, right = area.x + area.w, top = area.y, bottom = area.y + area.h;
    const float mid = area.x + area.w / 2;
    const float margin = 0.4f * u;

    auto& c = out.controls;
    const auto set = [&c](Control id, FRect r) { c[static_cast<std::size_t>(id)] = r; };
    set(Control::DPad, centred(left + margin + 1.5f * u + ox, bottom - margin - 1.5f * u - oy, 3 * u, 3 * u));
    const float ax = right - margin - 0.65f * u - ox, ay = bottom - margin - 2.1f * u - oy;
    set(Control::A, centred(ax, ay, 1.3f * u, 1.3f * u));
    set(Control::B, centred(ax - 1.6f * u, ay + 0.9f * u, 1.3f * u, 1.3f * u));
    set(Control::L, {left + 0.2f * u, top + 0.2f * u, 2.4f * u, 0.9f * u});
    set(Control::R, {right - 0.2f * u - 2.4f * u, top + 0.2f * u, 2.4f * u, 0.9f * u});
    set(Control::Select, centred(mid - 0.95f * u, bottom - 0.5f * u, 1.5f * u, 0.6f * u));
    set(Control::Start, centred(mid + 0.95f * u, bottom - 0.5f * u, 1.5f * u, 0.6f * u));
    set(Control::Menu, centred(mid - 0.75f * u, top + 0.5f * u, 1.2f * u, 0.6f * u));
    set(Control::FastForward, centred(mid + 0.75f * u, top + 0.5f * u, 1.2f * u, 0.6f * u));

    out.unit = u;
    for (int i = 0; i < kControls; ++i) {
        const Adjust& a = settings.adjust[static_cast<std::size_t>(i)];
        FRect& r = c[static_cast<std::size_t>(i)];
        const float k = static_cast<float>(a.size) / 100.0f;
        const float w = std::min(r.w * k, safe.w), h = std::min(r.h * k, safe.h);
        const float x = centre_x(r) + static_cast<float>(a.x) / 100.0f * u - w / 2;
        const float y = centre_y(r) + static_cast<float>(a.y) / 100.0f * u - h / 2;
        r = {std::clamp(x, safe.x, safe.x + safe.w - w), std::clamp(y, safe.y, safe.y + safe.h - h), w, h};
    }
    return out;
}

std::uint16_t buttons_at(const Layout& l, float x, float y) {
    if (!l.show_controls) {
        return l.menu_button && inside(l[Control::Menu], x, y, l[Control::Menu].h * 0.15f) ? kMenu : 0;
    }
    if (const std::uint16_t d = dpad_at(l[Control::DPad], x, y)) return d;
    if (in_circle(l[Control::A], x, y)) return kA;
    if (in_circle(l[Control::B], x, y)) return kB;
    const float margin = l[Control::Start].h * 0.15f;
    if (inside(l[Control::L], x, y, margin)) return kL;
    if (inside(l[Control::R], x, y, margin)) return kR;
    if (inside(l[Control::Select], x, y, margin)) return kSelect;
    if (inside(l[Control::Start], x, y, margin)) return kStart;
    if (l.menu_button && inside(l[Control::Menu], x, y, margin)) return kMenu;
    if (inside(l[Control::FastForward], x, y, margin)) return kFastForward;
    return 0;
}

Control control_at(const Layout& l, float x, float y) {
    // Last drawn first, so the topmost of two overlapping controls wins.
    for (int i = kControls - 1; i >= 0; --i) {
        const auto c = static_cast<Control>(i);
        if (c == Control::Menu && !l.menu_button) continue;
        if (inside(l[c], x, y, 0)) return c;
    }
    return Control::Count;
}

const char* key_name(Control control) {
    switch (control) {
    case Control::DPad: return "DPad";
    case Control::A: return "A";
    case Control::B: return "B";
    case Control::L: return "L";
    case Control::R: return "R";
    case Control::Select: return "Select";
    case Control::Start: return "Start";
    case Control::Menu: return "Menu";
    case Control::FastForward: return "FastForward";
    case Control::Count: break;
    }
    return "";
}

const char* label(Control control) {
    switch (control) {
    case Control::DPad: return "";
    case Control::A: return "A";
    case Control::B: return "B";
    case Control::L: return "L";
    case Control::R: return "R";
    case Control::Select: return "SELECT";
    case Control::Start: return "START";
    case Control::Menu: return "MENU";
    case Control::FastForward: return ">>";
    case Control::Count: break;
    }
    return "";
}

}  // namespace anygba::player::touch
