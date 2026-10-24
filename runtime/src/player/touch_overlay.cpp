#include "player/touch_overlay.h"

#include "input_map.h"

#include <anyport/platform/timing.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_system.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace anygba::player {

namespace {

namespace fs = std::filesystem;
using anyport::platform::FRect;
using assist::Item;
using assist::Point;
using touch::Control;

constexpr std::uint64_t kMenuHoldNs = 800'000'000;
constexpr int kStickNoise = 16000;  // gamepad axis travel that counts as "using the gamepad"
constexpr float kPi = 3.14159265f;
constexpr int kBackCell = assist::kSlots;  // the slot picker's last cell

SDL_FColor colour(float grey, float alpha) { return {grey, grey, grey, alpha}; }
SDL_FColor rgba(float r, float g, float b, float a) { return {r, g, b, a}; }
SDL_FColor with_alpha(SDL_FColor c, float a) { return {c.r, c.g, c.b, a}; }
const SDL_FColor kAccent = rgba(0.24f, 0.55f, 1.0f, 1.0f);

// ---- primitives: everything is triangles through SDL_RenderGeometry ----

void geometry(SDL_Renderer* r, const std::vector<SDL_Vertex>& v, const std::vector<int>& index) {
    if (v.empty() || index.empty()) return;
    SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), index.data(), static_cast<int>(index.size()));
}

SDL_Vertex vertex(Point p, SDL_FColor c) { return {{p.x, p.y}, c, {0, 0}}; }

// A shape that every point sees from `centre` (convex polygons, stars, rounded rectangles).
void fan(SDL_Renderer* r, Point centre, const std::vector<Point>& outline, SDL_FColor inner, SDL_FColor outer) {
    std::vector<SDL_Vertex> v{vertex(centre, inner)};
    std::vector<int> index;
    for (const Point& p : outline) v.push_back(vertex(p, outer));
    const int n = static_cast<int>(outline.size());
    for (int i = 0; i < n; ++i) index.insert(index.end(), {0, i + 1, (i + 1) % n + 1});
    geometry(r, v, index);
}

std::vector<Point> rounded_outline(const FRect& b, float radius) {
    radius = std::min({radius, b.w / 2, b.h / 2});
    constexpr int kCorner = 8;
    std::vector<Point> out;
    const std::array<Point, 4> centres = {Point{b.x + b.w - radius, b.y + radius}, Point{b.x + b.w - radius, b.y + b.h - radius},
                                          Point{b.x + radius, b.y + b.h - radius}, Point{b.x + radius, b.y + radius}};
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= kCorner; ++i) {
            const float a = (static_cast<float>(c) - 1 + static_cast<float>(i) / kCorner) * kPi / 2;
            out.push_back({centres[static_cast<std::size_t>(c)].x + radius * std::cos(a),
                           centres[static_cast<std::size_t>(c)].y + radius * std::sin(a)});
        }
    }
    return out;
}

// Filled rounded rectangle, shaded from `top` to `bottom`.
void fill_rounded(SDL_Renderer* r, const FRect& b, float radius, SDL_FColor top, SDL_FColor bottom) {
    const auto outline = rounded_outline(b, radius);
    std::vector<SDL_Vertex> v{vertex({b.x + b.w / 2, b.y + b.h / 2},
                                     rgba((top.r + bottom.r) / 2, (top.g + bottom.g) / 2, (top.b + bottom.b) / 2, (top.a + bottom.a) / 2))};
    for (const Point& p : outline) {
        const float t = b.h > 0 ? std::clamp((p.y - b.y) / b.h, 0.0f, 1.0f) : 0;
        v.push_back(vertex(p, rgba(top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t,
                                   top.b + (bottom.b - top.b) * t, top.a + (bottom.a - top.a) * t)));
    }
    std::vector<int> index;
    const int n = static_cast<int>(outline.size());
    for (int i = 0; i < n; ++i) index.insert(index.end(), {0, i + 1, (i + 1) % n + 1});
    geometry(r, v, index);
}

// A band of width `t` along a polyline (one quad per segment).
void stroke(SDL_Renderer* r, const std::vector<Point>& points, bool closed, float t, SDL_FColor c) {
    std::vector<SDL_Vertex> v;
    std::vector<int> index;
    const std::size_t n = points.size();
    const std::size_t segments = closed ? n : (n ? n - 1 : 0);
    for (std::size_t i = 0; i < segments; ++i) {
        const Point a = points[i], b = points[(i + 1) % n];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        if (len <= 0) continue;
        // Extend each end by half the width so joints of thick lines have no notch.
        const float ux = (b.x - a.x) / len * t / 2, uy = (b.y - a.y) / len * t / 2;
        const float nx = -uy, ny = ux;
        const int base = static_cast<int>(v.size());
        v.push_back(vertex({a.x - ux + nx, a.y - uy + ny}, c));
        v.push_back(vertex({a.x - ux - nx, a.y - uy - ny}, c));
        v.push_back(vertex({b.x + ux - nx, b.y + uy - ny}, c));
        v.push_back(vertex({b.x + ux + nx, b.y + uy + ny}, c));
        index.insert(index.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    geometry(r, v, index);
}

std::vector<Point> arc(Point c, float radius, float from, float to, int segments = 32) {
    std::vector<Point> out;
    for (int i = 0; i <= segments; ++i) {
        const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(segments);
        out.push_back({c.x + radius * std::cos(a), c.y + radius * std::sin(a)});
    }
    return out;
}

void fill_circle(SDL_Renderer* r, Point c, float radius, SDL_FColor colour) {
    auto outline = arc(c, radius, 0, 2 * kPi, 32);
    outline.pop_back();
    fan(r, c, outline, colour, colour);
}

void ring(SDL_Renderer* r, Point c, float radius, float t, SDL_FColor colour, float from = 0, float to = 2 * kPi) {
    std::vector<SDL_Vertex> v;
    std::vector<int> index;
    constexpr int kSegments = 40;
    for (int i = 0; i <= kSegments; ++i) {
        const float a = from + (to - from) * static_cast<float>(i) / kSegments;
        v.push_back(vertex({c.x + (radius - t / 2) * std::cos(a), c.y + (radius - t / 2) * std::sin(a)}, colour));
        v.push_back(vertex({c.x + (radius + t / 2) * std::cos(a), c.y + (radius + t / 2) * std::sin(a)}, colour));
        if (i > 0) {
            const int b = 2 * i;
            index.insert(index.end(), {b - 2, b - 1, b + 1, b - 2, b + 1, b});
        }
    }
    geometry(r, v, index);
}

void triangle(SDL_Renderer* r, Point a, Point b, Point c, SDL_FColor colour) {
    geometry(r, {vertex(a, colour), vertex(b, colour), vertex(c, colour)}, {0, 1, 2});
}

void fill_rect(SDL_Renderer* r, const FRect& rect, SDL_FColor c) {
    SDL_SetRenderDrawColorFloat(r, c.r, c.g, c.b, c.a);
    const SDL_FRect f{rect.x, rect.y, rect.w, rect.h};
    SDL_RenderFillRect(r, &f);
}

// SDL's built-in 8x8 font at a whole scale about `height` pixels tall, no wider than `max_width`.
void text(SDL_Renderer* r, float cx, float cy, float height, const std::string& s, SDL_FColor c, float max_width = 0) {
    if (s.empty()) return;
    const float chars = static_cast<float>(s.size());
    float scale = std::floor(height / 8);
    if (max_width > 0) scale = std::min(scale, std::floor(max_width / (8 * chars)));
    scale = std::max(1.0f, scale);
    const float w = chars * 8 * scale;
    SDL_SetRenderScale(r, scale, scale);
    SDL_SetRenderDrawColorFloat(r, c.r, c.g, c.b, c.a);
    SDL_RenderDebugText(r, (cx - w / 2) / scale, (cy - 4 * scale) / scale, s.c_str());
    SDL_SetRenderScale(r, 1, 1);
}

// Frosted card: soft shadow, a slightly lighter top, a hairline border.
void card(SDL_Renderer* r, const FRect& b, float radius, float alpha) {
    for (int i = 3; i >= 1; --i) {
        const float grow = radius * 0.18f * static_cast<float>(i);
        fill_rounded(r, {b.x - grow, b.y - grow * 0.6f, b.w + 2 * grow, b.h + 2 * grow}, radius + grow, colour(0, 0.07f * alpha),
                     colour(0, 0.1f * alpha));
    }
    fill_rounded(r, b, radius, rgba(0.20f, 0.22f, 0.27f, 0.82f * alpha), rgba(0.09f, 0.10f, 0.13f, 0.88f * alpha));
    stroke(r, rounded_outline(b, radius), true, std::max(1.0f, radius * 0.04f), colour(1, 0.14f * alpha));
}

// ---- icons, drawn in a box of side `s` centred on `c` ----

void icon(SDL_Renderer* r, Item item, Point c, float s, SDL_FColor col, bool flag) {
    const auto p = [&](float x, float y) { return Point{c.x + x * s, c.y + y * s}; };
    const float t = std::max(1.5f, s * 0.085f);
    const auto line = [&](float x0, float y0, float x1, float y1) { stroke(r, {p(x0, y0), p(x1, y1)}, false, t, col); };
    const auto tray = [&] { stroke(r, {p(-0.4f, 0.1f), p(-0.4f, 0.4f), p(0.4f, 0.4f), p(0.4f, 0.1f)}, false, t, col); };
    switch (item) {
    case Item::SaveState:
        tray();
        line(0, -0.45f, 0, 0.05f);
        triangle(r, p(-0.2f, -0.02f), p(0.2f, -0.02f), p(0, 0.22f), col);
        break;
    case Item::LoadState:
        tray();
        line(0, 0.22f, 0, -0.25f);
        triangle(r, p(-0.2f, -0.2f), p(0.2f, -0.2f), p(0, -0.45f), col);
        break;
    case Item::FastForward:
        triangle(r, p(-0.42f, -0.3f), p(-0.42f, 0.3f), p(0, 0), col);
        triangle(r, p(0, -0.3f), p(0, 0.3f), p(0.42f, 0), col);
        break;
    case Item::FastForwardSpeed:
        ring(r, p(0, 0.08f), s * 0.4f, t, col, kPi * 0.8f, kPi * 2.2f);
        line(0, 0.08f, 0.22f, -0.16f);
        fill_circle(r, p(0, 0.08f), t * 1.2f, col);
        break;
    case Item::Cheats: {
        std::vector<Point> star;
        for (int i = 0; i < 10; ++i) {
            const float a = -kPi / 2 + static_cast<float>(i) * kPi / 5;
            const float radius = (i % 2 ? 0.2f : 0.47f) * s;
            star.push_back({c.x + radius * std::cos(a), c.y + 0.04f * s + radius * std::sin(a)});
        }
        fan(r, {c.x, c.y + 0.04f * s}, star, col, col);
        break;
    }
    case Item::Screenshot:
        stroke(r, rounded_outline({c.x - 0.44f * s, c.y - 0.24f * s, 0.88f * s, 0.62f * s}, 0.1f * s), true, t, col);
        fill_rounded(r, {c.x - 0.16f * s, c.y - 0.38f * s, 0.32f * s, 0.16f * s}, 0.04f * s, col, col);
        ring(r, p(0, 0.07f), 0.16f * s, t, col);
        break;
    case Item::Filter:
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                const float a = (x + y) % 2 ? 0.55f : 1.0f;
                fill_rounded(r, {c.x + (-0.42f + 0.29f * static_cast<float>(x)) * s, c.y + (-0.42f + 0.29f * static_cast<float>(y)) * s,
                                 0.25f * s, 0.25f * s},
                             0.05f * s, with_alpha(col, col.a * a), with_alpha(col, col.a * a));
            }
        }
        break;
    case Item::Widescreen:
        stroke(r, rounded_outline({c.x - 0.46f * s, c.y - 0.28f * s, 0.92f * s, 0.56f * s}, 0.08f * s), true, t, col);
        line(-0.22f, 0, 0.22f, 0);
        triangle(r, p(-0.32f, 0), p(-0.18f, -0.12f), p(-0.18f, 0.12f), col);
        triangle(r, p(0.32f, 0), p(0.18f, -0.12f), p(0.18f, 0.12f), col);
        break;
    case Item::EditControls:
        line(-0.3f, 0, 0.3f, 0);
        line(0, -0.3f, 0, 0.3f);
        triangle(r, p(-0.46f, 0), p(-0.28f, -0.14f), p(-0.28f, 0.14f), col);
        triangle(r, p(0.46f, 0), p(0.28f, -0.14f), p(0.28f, 0.14f), col);
        triangle(r, p(0, -0.46f), p(-0.14f, -0.28f), p(0.14f, -0.28f), col);
        triangle(r, p(0, 0.46f), p(-0.14f, 0.28f), p(0.14f, 0.28f), col);
        break;
    case Item::ShowControls:
        stroke(r, rounded_outline({c.x - 0.46f * s, c.y - 0.26f * s, 0.92f * s, 0.52f * s}, 0.22f * s), true, t, col);
        line(-0.3f, 0, -0.1f, 0);
        line(-0.2f, -0.1f, -0.2f, 0.1f);
        fill_circle(r, p(0.14f, 0.06f), t * 0.9f, col);
        fill_circle(r, p(0.28f, -0.06f), t * 0.9f, col);
        if (flag) line(-0.42f, 0.42f, 0.42f, -0.42f);  // hidden
        break;
    case Item::GameMenu:
        for (const float y : {-0.26f, 0.0f, 0.26f}) line(-0.38f, y, 0.38f, y);
        break;
    case Item::Settings:
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * kPi / 4;
            stroke(r, {p(0.26f * std::cos(a), 0.26f * std::sin(a)), p(0.44f * std::cos(a), 0.44f * std::sin(a))}, false, t * 1.6f, col);
        }
        ring(r, c, 0.27f * s, t * 1.4f, col);
        break;
    case Item::Exit:
        ring(r, p(0, 0.04f), 0.36f * s, t, col, -kPi / 2 + 0.75f, 1.5f * kPi - 0.75f);
        line(0, -0.46f, 0, -0.04f);
        break;
    case Item::Count: break;
    }
}

void back_icon(SDL_Renderer* r, Point c, float s, SDL_FColor col) {
    const float t = std::max(1.5f, s * 0.085f);
    stroke(r, {{c.x - 0.3f * s, c.y}, {c.x + 0.38f * s, c.y}}, false, t, col);
    triangle(r, {c.x - 0.44f * s, c.y}, {c.x - 0.2f * s, c.y - 0.2f * s}, {c.x - 0.2f * s, c.y + 0.2f * s}, col);
}

std::uint32_t hotkey_bit(input::Hotkey h) { return 1u << static_cast<int>(h); }

}  // namespace

TouchOverlay::TouchOverlay(std::optional<fs::path> settings_file) : settings_file_(std::move(settings_file)) {
    reload_settings();
    button_.wake(anyport::platform::now_ns());
    if (!SDL_AddEventWatch(&TouchOverlay::on_event, this)) {
        std::fprintf(stderr, "warning: touch controls disabled: %s\n", SDL_GetError());
    }
}

TouchOverlay::~TouchOverlay() {
    SDL_RemoveEventWatch(&TouchOverlay::on_event, this);
    if (haptic_) SDL_CloseHaptic(haptic_);
}

bool TouchOverlay::on_event(void* self, SDL_Event* event) {
    static_cast<TouchOverlay*>(self)->handle(*event);
    return true;
}

void TouchOverlay::handle(const SDL_Event& e) {
    const std::lock_guard lock(mutex_);
    const std::uint64_t now = anyport::platform::now_ns();
    switch (e.type) {
    case SDL_EVENT_FINGER_DOWN:
        hidden_ = false;
        finger_down(static_cast<std::int64_t>(e.tfinger.fingerID), e.tfinger.x * width_, e.tfinger.y * height_, now);
        break;
    case SDL_EVENT_FINGER_MOTION:
        finger_move(static_cast<std::int64_t>(e.tfinger.fingerID), e.tfinger.x * width_, e.tfinger.y * height_, now);
        break;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        finger_up(static_cast<std::int64_t>(e.tfinger.fingerID), e.tfinger.x * width_, e.tfinger.y * height_, now,
                  e.type == SDL_EVENT_FINGER_CANCELED);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_KEY_DOWN:
        if (mode_ == Mode::Play) hidden_ = true;
        fingers_.clear();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(static_cast<int>(e.gaxis.value)) > kStickNoise) {
            if (mode_ == Mode::Play) hidden_ = true;
            fingers_.clear();
        }
        break;
    default: break;
    }
}

void TouchOverlay::finger_down(std::int64_t id, float x, float y, std::uint64_t now) {
    switch (mode_) {
    case Mode::Play:
        if (settings_.floating_button && button_.down(id, x, y, now)) return;
        fingers_[id] = touch::buttons_at(layout_, x, y);
        return;
    case Mode::Panel:
    case Mode::Slots: {
        const auto g = assist::grid(mode_ == Mode::Panel ? assist::kItems : assist::kSlots + 1, width_, height_, safe_);
        panel_presses_[id] = assist::cell_at(g, x, y);
        return;
    }
    case Mode::Edit: {
        if (const auto tool = assist::tool_at(assist::toolbar(width_, height_, safe_), x, y)) {
            panel_presses_[id] = static_cast<int>(*tool);
            return;
        }
        panel_presses_[id] = -1;
        const Control c = touch::control_at(layout_, x, y);
        selected_ = c;
        if (c != Control::Count && !drag_finger_) {
            drag_finger_ = id;
            drag_from_ = {x, y};
            drag_start_ = settings_.adjust[static_cast<std::size_t>(c)];
        }
        return;
    }
    }
}

void TouchOverlay::finger_move(std::int64_t id, float x, float y, std::uint64_t now) {
    if (mode_ == Mode::Play) {
        if (button_.owns(id)) {
            button_.move(id, x, y, now);
        } else if (fingers_.contains(id)) {
            fingers_[id] = touch::buttons_at(layout_, x, y);
        }
        return;
    }
    if (mode_ == Mode::Edit && drag_finger_ == id && selected_ != Control::Count && layout_.unit > 0) {
        touch::Adjust& a = settings_.adjust[static_cast<std::size_t>(selected_)];
        a.x = std::clamp(drag_start_.x + static_cast<int>(std::lround((x - drag_from_.x) / layout_.unit * 100)), -1000, 1000);
        a.y = std::clamp(drag_start_.y + static_cast<int>(std::lround((y - drag_from_.y) / layout_.unit * 100)), -1000, 1000);
        relayout();
    }
}

void TouchOverlay::finger_up(std::int64_t id, float x, float y, std::uint64_t now, bool cancelled) {
    switch (mode_) {
    case Mode::Play: {
        fingers_.erase(id);
        if (!button_.owns(id)) return;
        const auto gesture = button_.up(id, now, cancelled);
        if (gesture == assist::Button::Gesture::Dropped) {
            std::tie(settings_.button_edge, settings_.button_pos) = button_.dropped();
            settings_dirty_ = true;
        } else if (gesture == assist::Button::Gesture::Tap) {
            if (status_.game_menu_open) {
                actions_.push_back({Item::GameMenu, 0});  // closes it
            } else {
                mode_ = Mode::Panel;
                fingers_.clear();
            }
        }
        return;
    }
    case Mode::Panel:
    case Mode::Slots: {
        const auto press = panel_presses_.find(id);
        if (press == panel_presses_.end()) return;
        const int down = press->second;
        panel_presses_.erase(press);
        if (cancelled) return;
        const bool panel = mode_ == Mode::Panel;
        const auto g = assist::grid(panel ? assist::kItems : assist::kSlots + 1, width_, height_, safe_);
        const int cell = assist::cell_at(g, x, y);
        const bool on_card = x >= g.card.x && x < g.card.x + g.card.w && y >= g.card.y && y < g.card.y + g.card.h;
        if (cell < 0 || cell != down) {
            if (!on_card && down < 0) mode_ = Mode::Play;  // tap outside: close
            return;
        }
        if (panel) {
            choose(static_cast<Item>(cell));
        } else if (cell == kBackCell) {
            mode_ = Mode::Panel;
        } else {
            actions_.push_back({Item::LoadState, cell + 1});
            mode_ = Mode::Play;
        }
        button_.wake(now);
        return;
    }
    case Mode::Edit: {
        if (drag_finger_ == id) {
            drag_finger_.reset();
            settings_dirty_ = true;
        }
        const auto press = panel_presses_.find(id);
        if (press == panel_presses_.end()) return;
        const int down = press->second;
        panel_presses_.erase(press);
        if (cancelled || down < 0) return;
        if (const auto tool = assist::tool_at(assist::toolbar(width_, height_, safe_), x, y); tool && static_cast<int>(*tool) == down) {
            use_tool(*tool);
        }
        return;
    }
    }
}

void TouchOverlay::choose(Item item) {
    switch (item) {
    case Item::LoadState: mode_ = Mode::Slots; return;
    case Item::EditControls:
        mode_ = Mode::Edit;
        selected_ = Control::Count;
        return;
    case Item::ShowControls:
        settings_.show_controls = !settings_.show_controls;
        settings_dirty_ = true;
        relayout();
        return;
    case Item::Settings:
        open_settings_ = true;
        mode_ = Mode::Play;
        return;
    case Item::FastForwardSpeed:
    case Item::Filter:
    case Item::Widescreen:
        actions_.push_back({item, 0});  // stays open: tap again for the next value
        return;
    default:
        actions_.push_back({item, 0});
        mode_ = Mode::Play;
        return;
    }
}

void TouchOverlay::use_tool(assist::Tool tool) {
    const auto each = [this](auto&& f) {
        if (selected_ != Control::Count) {
            f(settings_.adjust[static_cast<std::size_t>(selected_)]);
        } else {
            for (auto& a : settings_.adjust) f(a);
        }
    };
    switch (tool) {
    case assist::Tool::Smaller: each([](touch::Adjust& a) { a.size = std::max(50, a.size - 10); }); break;
    case assist::Tool::Bigger: each([](touch::Adjust& a) { a.size = std::min(200, a.size + 10); }); break;
    case assist::Tool::Reset: each([](touch::Adjust& a) { a = {}; }); break;
    case assist::Tool::Done:
        mode_ = Mode::Play;
        selected_ = Control::Count;
        break;
    case assist::Tool::Count: break;
    }
    settings_dirty_ = true;
    relayout();
}

void TouchOverlay::relayout() {
    if (width_ > 0 && height_ > 0) layout_ = touch::layout(width_, height_, safe_, settings_, picture_width_);
}

FRect TouchOverlay::picture_rect(float width, float height, FRect safe) {
    const std::lock_guard lock(mutex_);
    width_ = width;
    height_ = height;
    safe_ = safe.w > 0 && safe.h > 0 ? safe : FRect{0, 0, width, height};
    relayout();
    button_.place(settings_.button_edge, settings_.button_pos, safe_, assist::diameter(width, height));
    return layout_.picture;
}

void TouchOverlay::set_picture_width(int width) {
    const std::lock_guard lock(mutex_);
    picture_width_ = static_cast<float>(width);
}

void TouchOverlay::set_status(const TouchStatus& status) {
    const std::lock_guard lock(mutex_);
    status_ = status;
}

bool TouchOverlay::paused() const {
    const std::lock_guard lock(mutex_);
    return mode_ != Mode::Play;
}

std::vector<TouchAction> TouchOverlay::take_actions() {
    const std::lock_guard lock(mutex_);
    return std::exchange(actions_, {});
}

void TouchOverlay::draw(SDL_Renderer* r, float width, float height) {
    touch::Layout l;
    touch::Settings s;
    TouchStatus status;
    Mode mode = Mode::Play;
    Control selected = Control::Count;
    Point centre{};
    float diameter = 0, activity = 0;
    int pressed = -1;
    const std::uint64_t now = anyport::platform::now_ns();
    {
        const std::lock_guard lock(mutex_);
        if (hidden_ && mode_ == Mode::Play) return;
        l = layout_;
        s = settings_;
        status = status_;
        mode = mode_;
        selected = selected_;
        centre = button_.centre(now);
        diameter = button_.diameter();
        activity = button_.activity(now);
        if (!panel_presses_.empty()) pressed = panel_presses_.begin()->second;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const float alpha = static_cast<float>(s.opacity) / 100.0f;
    if (mode == Mode::Edit) {
        draw_controls(r, l, std::max(alpha, 0.6f), true, selected);
        draw_toolbar(r, width, height, selected);
        return;
    }
    if (s.show_controls || l.menu_button) draw_controls(r, l, alpha, false, selected);
    if (s.floating_button) {
        // Bright while in use, dimmer (but never lost) when idle.
        const float idle = std::max(0.22f, alpha * 0.7f);
        draw_button(r, centre, diameter, idle + (0.9f - idle) * activity);
    }
    if (mode == Mode::Panel || mode == Mode::Slots) draw_panel(r, width, height, status, s, mode, pressed);
}

void TouchOverlay::draw_controls(SDL_Renderer* r, const touch::Layout& l, float alpha, bool editing, Control selected) const {
    const auto shade = [&](std::uint16_t bits) { return colour(!editing && (held_ & bits) ? 0.85f : 0.35f, alpha); };
    const float label_alpha = std::min(1.0f, alpha * 1.6f);
    const auto outline = [&](Control c, float radius) {
        if (!editing) return;
        const FRect& b = l[c];
        const bool on = c == selected;
        stroke(r, rounded_outline(b, radius), true, std::max(2.0f, l.unit * (on ? 0.08f : 0.04f)),
               on ? kAccent : colour(1, 0.55f));
    };
    const bool buttons = l.show_controls || editing;

    if (buttons) {
        const FRect& d = l[Control::DPad];
        const float arm = d.w / 3;
        constexpr std::uint16_t kRight = 1u << 4, kLeft = 1u << 5, kUp = 1u << 6, kDown = 1u << 7;
        const float round = arm * 0.25f;
        fill_rounded(r, {d.x + arm, d.y, arm, d.h}, round, colour(0.35f, alpha), colour(0.3f, alpha));
        fill_rounded(r, {d.x, d.y + arm, d.w, arm}, round, colour(0.35f, alpha), colour(0.3f, alpha));
        if (held_ & kUp) fill_rect(r, {d.x + arm, d.y, arm, arm}, shade(kUp));
        if (held_ & kDown) fill_rect(r, {d.x + arm, d.y + 2 * arm, arm, arm}, shade(kDown));
        if (held_ & kLeft) fill_rect(r, {d.x, d.y + arm, arm, arm}, shade(kLeft));
        if (held_ & kRight) fill_rect(r, {d.x + 2 * arm, d.y + arm, arm, arm}, shade(kRight));
        outline(Control::DPad, d.w * 0.12f);
    }

    constexpr std::array<std::pair<Control, std::uint16_t>, 8> kButtons = {{
        {Control::A, 1u << 0},
        {Control::B, 1u << 1},
        {Control::L, 1u << 9},
        {Control::R, 1u << 8},
        {Control::Select, 1u << 2},
        {Control::Start, 1u << 3},
        {Control::Menu, touch::kMenu},
        {Control::FastForward, touch::kFastForward},
    }};
    for (const auto& [control, bits] : kButtons) {
        if (control == Control::Menu ? !l.menu_button : !buttons) continue;
        const FRect& b = l[control];
        const bool round = control == Control::A || control == Control::B;
        const float radius = round ? b.w / 2 : b.h * 0.45f;
        fill_rounded(r, b, radius, shade(bits), with_alpha(shade(bits), alpha * 0.85f));
        text(r, b.x + b.w / 2, b.y + b.h / 2, b.h * 0.45f, touch::label(control), colour(1, label_alpha), b.w * 0.9f);
        outline(control, radius);
    }
}

void TouchOverlay::draw_button(SDL_Renderer* r, Point c, float d, float alpha) const {
    const float radius = d / 2;
    for (int i = 2; i >= 1; --i) fill_circle(r, {c.x, c.y + radius * 0.06f}, radius * (1 + 0.07f * static_cast<float>(i)), colour(0, 0.12f * alpha));
    fill_rounded(r, {c.x - radius, c.y - radius, d, d}, radius, rgba(0.24f, 0.26f, 0.31f, 0.85f * alpha),
                 rgba(0.08f, 0.09f, 0.11f, 0.9f * alpha));
    ring(r, c, radius - 0.5f, std::max(1.0f, d * 0.02f), colour(1, 0.18f * alpha));
    fill_circle(r, c, radius * 0.66f, colour(1, 0.16f * alpha));
    fill_circle(r, c, radius * 0.5f, colour(1, 0.28f * alpha));
    fill_circle(r, c, radius * 0.34f, colour(1, 0.92f * alpha));
}

void TouchOverlay::draw_panel(SDL_Renderer* r, float w, float h, const TouchStatus& status, const touch::Settings& s,
                              Mode mode, int pressed) const {
    fill_rect(r, {0, 0, w, h}, colour(0, 0.42f));
    const bool panel = mode == Mode::Panel;
    const auto g = assist::grid(panel ? assist::kItems : assist::kSlots + 1, w, h, safe_);
    if (g.cells.empty()) return;
    card(r, g.card, g.cell * 0.28f, 1);
    const SDL_FColor white = colour(1, 0.95f), dim = colour(1, 0.55f);
    const float label_h = g.cell * 0.13f;
    if (!panel) {
        text(r, g.card.x + g.card.w / 2, g.card.y - label_h * 1.4f, label_h * 1.2f, "Load state", white, g.card.w);
    }
    for (std::size_t i = 0; i < g.cells.size(); ++i) {
        const FRect& cell = g.cells[i];
        const FRect tile{cell.x + cell.w * 0.06f, cell.y + cell.h * 0.06f, cell.w * 0.88f, cell.h * 0.88f};
        const Point centre{tile.x + tile.w / 2, tile.y + tile.h * 0.4f};
        const float icon_size = tile.w * 0.38f;
        const float label_y = tile.y + tile.h * 0.82f;
        bool active = false;
        if (static_cast<int>(i) == pressed) fill_rounded(r, tile, tile.w * 0.22f, colour(1, 0.2f), colour(1, 0.14f));
        if (panel) {
            const auto item = static_cast<Item>(i);
            std::string label = assist::item_label(item);
            switch (item) {
            case Item::SaveState: label += " " + std::to_string(status.slot); break;
            case Item::FastForward: active = status.fast_forward; break;
            case Item::FastForwardSpeed: label = status.fast_forward_speed; break;
            case Item::Filter: label = status.filter; break;
            case Item::Widescreen: label = status.widescreen == "off" ? "Wide off" : status.widescreen; break;
            case Item::ShowControls: label = s.show_controls ? "Hide pad" : "Show pad"; break;
            default: break;
            }
            if (active) fill_rounded(r, tile, tile.w * 0.22f, with_alpha(kAccent, 0.55f), with_alpha(kAccent, 0.4f));
            icon(r, item, centre, icon_size, white, item == Item::ShowControls && !s.show_controls);
            text(r, tile.x + tile.w / 2, label_y, label_h, label, active ? white : colour(1, 0.8f), tile.w * 0.94f);
        } else if (static_cast<int>(i) == kBackCell) {
            back_icon(r, centre, icon_size, white);
            text(r, tile.x + tile.w / 2, label_y, label_h, "Back", colour(1, 0.8f), tile.w * 0.94f);
        } else {
            const int slot = static_cast<int>(i) + 1;
            const bool saved = (status.saved_slots >> i) & 1u;
            if (slot == status.slot) {
                stroke(r, rounded_outline(tile, tile.w * 0.22f), true, std::max(2.0f, tile.w * 0.03f), kAccent);
            }
            text(r, centre.x, centre.y, tile.h * 0.36f, std::to_string(slot), saved ? white : dim);
            text(r, tile.x + tile.w / 2, label_y, label_h, saved ? "saved" : "empty", saved ? colour(1, 0.8f) : dim, tile.w * 0.94f);
        }
    }
}

void TouchOverlay::draw_toolbar(SDL_Renderer* r, float w, float h, Control selected) const {
    const auto bar = assist::toolbar(w, h, safe_);
    const FRect& first = bar.front();
    const FRect& last = bar.back();
    const float pad = first.h * 0.35f;
    const FRect back{first.x - pad, first.y - pad - first.h * 0.6f, last.x + last.w - first.x + 2 * pad, first.h * 1.6f + 2 * pad};
    card(r, back, first.h * 0.5f, 1);
    const std::string hint = selected == Control::Count ? "Drag a control. Sizes apply to all"
                                                        : std::string("Selected: ") + touch::key_name(selected);
    text(r, back.x + back.w / 2, first.y - first.h * 0.35f, first.h * 0.3f, hint, colour(1, 0.8f), back.w * 0.94f);
    constexpr std::array<const char*, assist::kTools> kNames = {"Smaller", "Bigger", "Reset", "Done"};
    for (int i = 0; i < assist::kTools; ++i) {
        const FRect& b = bar[static_cast<std::size_t>(i)];
        const bool done = static_cast<assist::Tool>(i) == assist::Tool::Done;
        fill_rounded(r, b, b.h / 2, done ? with_alpha(kAccent, 0.95f) : colour(1, 0.16f),
                     done ? with_alpha(kAccent, 0.8f) : colour(1, 0.1f));
        text(r, b.x + b.w / 2, b.y + b.h / 2, b.h * 0.36f, kNames[static_cast<std::size_t>(i)], colour(1, 0.95f), b.w * 0.86f);
    }
}

void TouchOverlay::update() {
    std::uint16_t held = 0;
    bool haptics = false;
    bool open_settings = false;
    bool long_press = false;
    std::optional<touch::Settings> to_write;
    const std::uint64_t now = anyport::platform::now_ns();
    {
        const std::lock_guard lock(mutex_);
        if (!hidden_ && mode_ == Mode::Play) {
            for (const auto& [id, bits] : fingers_) held = static_cast<std::uint16_t>(held | bits);
        }
        haptics = settings_.haptics;
        if (settings_.floating_button && button_.tick(now) == assist::Button::Gesture::LongPress) {
            actions_.push_back({Item::SaveState, 0});
            long_press = true;
        }
        open_settings = std::exchange(open_settings_, false);
        if (settings_dirty_ && !drag_finger_ && !button_.held()) {
            settings_dirty_ = false;
            to_write = settings_;
        }
    }
    prev_ = held_;
    held_ = held;
    const std::uint16_t pressed = held_ & ~prev_;
    if ((pressed || long_press) && haptics) buzz();
    if (to_write) write_settings(*to_write);

    menu_tap_ = false;
    if (pressed & touch::kMenu) {
        menu_since_ns_ = now;
        menu_hold_sent_ = false;
    }
    if (held_ & touch::kMenu) {
        if (!menu_hold_sent_ && now - menu_since_ns_ >= kMenuHoldNs) {
            menu_hold_sent_ = true;
            open_settings = true;
        }
    } else if ((prev_ & touch::kMenu) && !menu_hold_sent_) {
        menu_tap_ = true;  // released before the hold time: the game menu
    }
#if defined(__ANDROID__)
    if (open_settings && !SDL_SendAndroidMessage(kTouchSettingsMessage, 0)) {
        std::fprintf(stderr, "warning: cannot open the touch settings: %s\n", SDL_GetError());
    }
#else
    (void)open_settings;
#endif

    if (++frames_ % 30 == 0) reload_settings();
}

std::uint32_t TouchOverlay::hotkeys() const {
    std::uint32_t bits = 0;
    if (menu_tap_) bits |= hotkey_bit(input::Hotkey::Menu);
    if (held_ & touch::kFastForward) bits |= hotkey_bit(input::Hotkey::FastForward);
    return bits;
}

void TouchOverlay::reload_settings() {
    if (!settings_file_) return;
    std::error_code ec;
    const auto time = fs::last_write_time(*settings_file_, ec);
    if (ec || time == settings_time_) return;
    settings_time_ = time;
    std::ifstream in(*settings_file_, std::ios::binary);
    if (!in) return;
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::lock_guard lock(mutex_);
    if (settings_dirty_) return;  // our own change is newer; it is written next
    settings_ = touch::parse_settings(text);  // the layout follows at the next picture_rect()
}

void TouchOverlay::write_settings(const touch::Settings& settings) {
    if (!settings_file_) return;
    const fs::path tmp = settings_file_->string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << touch::format_settings(settings);
        if (!out.flush()) {
            std::fprintf(stderr, "warning: cannot write %s\n", tmp.string().c_str());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, *settings_file_, ec);
    if (ec) {
        std::fprintf(stderr, "warning: cannot replace %s: %s\n", settings_file_->string().c_str(), ec.message().c_str());
        return;
    }
    settings_time_ = fs::last_write_time(*settings_file_, ec);
}

void TouchOverlay::buzz() {
    if (!haptic_ && !haptic_failed_) {
        int count = 0;
        SDL_HapticID* ids = nullptr;
        if (SDL_InitSubSystem(SDL_INIT_HAPTIC)) ids = SDL_GetHaptics(&count);
        if (ids && count > 0) haptic_ = SDL_OpenHaptic(ids[0]);
        SDL_free(ids);
        if (haptic_ && !SDL_InitHapticRumble(haptic_)) {
            SDL_CloseHaptic(haptic_);
            haptic_ = nullptr;
        }
        haptic_failed_ = haptic_ == nullptr;
    }
    if (haptic_) SDL_PlayHapticRumble(haptic_, 0.5f, 20);
}

}  // namespace anygba::player
