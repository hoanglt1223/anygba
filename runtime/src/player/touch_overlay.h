#pragma once

#include "player/touch.h"

#include <anyport/platform/window.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>

struct SDL_Haptic;
union SDL_Event;

// Touch controls on top of the game window: reads finger events, draws the buttons, and
// hides itself while a gamepad or keyboard is in use. Finger events may arrive on another
// thread (Android delivers them from the UI thread), so they only touch state under a lock;
// update() and the getters run on the game thread once per host frame.
namespace anygba::player {

// SDL_SendAndroidMessage() command that asks the activity for its touch settings dialog.
inline constexpr std::uint32_t kTouchSettingsMessage = 0x8001;

class TouchOverlay final : public anyport::platform::ScreenOverlay {
public:
    // `settings_file` (anygba-touch.ini) is read now and again whenever it changes.
    explicit TouchOverlay(std::optional<std::filesystem::path> settings_file);
    ~TouchOverlay() override;

    TouchOverlay(const TouchOverlay&) = delete;
    TouchOverlay& operator=(const TouchOverlay&) = delete;

    anyport::platform::FRect picture_rect(float width, float height, anyport::platform::FRect safe) override;
    void draw(SDL_Renderer* renderer, float width, float height) override;

    // Once per host frame: haptics, the Menu tap/hold, settings reload.
    void update();
    // GBA buttons held by touch, KEYINPUT bit order.
    std::uint16_t gba_keys() const { return held_ & touch::kGbaMask; }
    // input::Hotkey bits: Menu for one frame after a short tap, FastForward while held.
    std::uint32_t hotkeys() const;

private:
    static bool on_event(void* self, SDL_Event* event);
    void handle(const SDL_Event& event);
    void reload_settings();
    void buzz();

    std::optional<std::filesystem::path> settings_file_;
    std::filesystem::file_time_type settings_time_{};

    mutable std::mutex mutex_;  // guards everything below up to held_
    touch::Settings settings_;
    touch::Layout layout_{};
    float width_ = 0, height_ = 0;
    std::map<std::int64_t, std::uint16_t> fingers_;
    bool hidden_ = false;

    std::uint16_t held_ = 0;
    std::uint16_t prev_ = 0;
    bool menu_tap_ = false;
    bool menu_hold_sent_ = false;
    std::uint64_t menu_since_ns_ = 0;
    std::uint64_t frames_ = 0;
    SDL_Haptic* haptic_ = nullptr;
    bool haptic_failed_ = false;
};

}  // namespace anygba::player
