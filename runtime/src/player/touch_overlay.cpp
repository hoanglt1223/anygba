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

namespace anygba::player {

namespace {

namespace fs = std::filesystem;
using anyport::platform::FRect;
using touch::Control;

constexpr std::uint64_t kMenuHoldNs = 800'000'000;
constexpr int kStickNoise = 16000;  // gamepad axis travel that counts as "using the gamepad"

SDL_FColor colour(float grey, float alpha) { return {grey, grey, grey, alpha}; }

void fill_circle(SDL_Renderer* r, float cx, float cy, float radius, SDL_FColor c) {
    constexpr int kSegments = 32;
    std::array<SDL_Vertex, kSegments + 1> v{};
    std::array<int, kSegments * 3> index{};
    v[0] = {{cx, cy}, c, {0, 0}};
    for (int i = 0; i < kSegments; ++i) {
        const float a = static_cast<float>(i) * 6.2831853f / kSegments;
        v[static_cast<std::size_t>(i + 1)] = {{cx + radius * std::cos(a), cy + radius * std::sin(a)}, c, {0, 0}};
        index[static_cast<std::size_t>(i * 3)] = 0;
        index[static_cast<std::size_t>(i * 3 + 1)] = i + 1;
        index[static_cast<std::size_t>(i * 3 + 2)] = (i + 1) % kSegments + 1;
    }
    SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), index.data(), static_cast<int>(index.size()));
}

void fill_rect(SDL_Renderer* r, const FRect& rect, SDL_FColor c) {
    SDL_SetRenderDrawColorFloat(r, c.r, c.g, c.b, c.a);
    const SDL_FRect f{rect.x, rect.y, rect.w, rect.h};
    SDL_RenderFillRect(r, &f);
}

// SDL's built-in 8x8 font, scaled so the text is about `height` pixels tall.
void text(SDL_Renderer* r, float cx, float cy, float height, const char* s, float alpha) {
    const float scale = std::max(1.0f, std::floor(height / 8));
    const float w = static_cast<float>(std::char_traits<char>::length(s)) * 8 * scale;
    SDL_SetRenderScale(r, scale, scale);
    SDL_SetRenderDrawColorFloat(r, 1, 1, 1, alpha);
    SDL_RenderDebugText(r, (cx - w / 2) / scale, (cy - 4 * scale) / scale, s);
    SDL_SetRenderScale(r, 1, 1);
}

std::uint32_t hotkey_bit(input::Hotkey h) { return 1u << static_cast<int>(h); }

}  // namespace

TouchOverlay::TouchOverlay(std::optional<fs::path> settings_file) : settings_file_(std::move(settings_file)) {
    reload_settings();
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
    switch (e.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION: {
        if (e.type == SDL_EVENT_FINGER_DOWN) hidden_ = false;
        const auto id = static_cast<std::int64_t>(e.tfinger.fingerID);
        fingers_[id] = touch::buttons_at(layout_, e.tfinger.x * width_, e.tfinger.y * height_);
        break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        fingers_.erase(static_cast<std::int64_t>(e.tfinger.fingerID));
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_KEY_DOWN:
        hidden_ = true;
        fingers_.clear();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(static_cast<int>(e.gaxis.value)) > kStickNoise) {
            hidden_ = true;
            fingers_.clear();
        }
        break;
    default: break;
    }
}

FRect TouchOverlay::picture_rect(float width, float height, FRect safe) {
    const std::lock_guard lock(mutex_);
    width_ = width;
    height_ = height;
    layout_ = touch::layout(width, height, safe, settings_);
    return layout_.picture;
}

void TouchOverlay::draw(SDL_Renderer* r, float, float) {
    touch::Layout l;
    float alpha = 0;
    {
        const std::lock_guard lock(mutex_);
        if (hidden_) return;
        l = layout_;
        alpha = static_cast<float>(settings_.opacity) / 100.0f;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const auto shade = [&](std::uint16_t bits) { return colour(held_ & bits ? 0.85f : 0.35f, alpha); };

    const FRect& d = l[Control::DPad];
    const float arm = d.w / 3;
    fill_rect(r, {d.x + arm, d.y, arm, d.h}, colour(0.35f, alpha));
    fill_rect(r, {d.x, d.y + arm, d.w, arm}, colour(0.35f, alpha));
    constexpr std::uint16_t kRight = 1u << 4, kLeft = 1u << 5, kUp = 1u << 6, kDown = 1u << 7;
    if (held_ & kUp) fill_rect(r, {d.x + arm, d.y, arm, arm}, shade(kUp));
    if (held_ & kDown) fill_rect(r, {d.x + arm, d.y + 2 * arm, arm, arm}, shade(kDown));
    if (held_ & kLeft) fill_rect(r, {d.x, d.y + arm, arm, arm}, shade(kLeft));
    if (held_ & kRight) fill_rect(r, {d.x + 2 * arm, d.y + arm, arm, arm}, shade(kRight));

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
        const FRect& b = l[control];
        if (control == Control::A || control == Control::B) {
            fill_circle(r, b.x + b.w / 2, b.y + b.h / 2, b.w / 2, shade(bits));
        } else {
            fill_rect(r, b, shade(bits));
        }
        text(r, b.x + b.w / 2, b.y + b.h / 2, b.h * 0.45f, touch::label(control), std::min(1.0f, alpha * 1.6f));
    }
}

void TouchOverlay::update() {
    std::uint16_t held = 0;
    bool haptics = false;
    {
        const std::lock_guard lock(mutex_);
        if (!hidden_) {
            for (const auto& [id, bits] : fingers_) held = static_cast<std::uint16_t>(held | bits);
        }
        haptics = settings_.haptics;
    }
    prev_ = held_;
    held_ = held;
    const std::uint16_t pressed = held_ & ~prev_;
    if (pressed && haptics) buzz();

    const std::uint64_t now = anyport::platform::now_ns();
    menu_tap_ = false;
    if (pressed & touch::kMenu) {
        menu_since_ns_ = now;
        menu_hold_sent_ = false;
    }
    if (held_ & touch::kMenu) {
        if (!menu_hold_sent_ && now - menu_since_ns_ >= kMenuHoldNs) {
            menu_hold_sent_ = true;
#if defined(__ANDROID__)
            if (!SDL_SendAndroidMessage(kTouchSettingsMessage, 0)) {
                std::fprintf(stderr, "warning: cannot open the touch settings: %s\n", SDL_GetError());
            }
#endif
        }
    } else if ((prev_ & touch::kMenu) && !menu_hold_sent_) {
        menu_tap_ = true;  // released before the hold time: the game menu
    }

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
    settings_ = touch::parse_settings(text);  // the layout follows at the next picture_rect()
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
