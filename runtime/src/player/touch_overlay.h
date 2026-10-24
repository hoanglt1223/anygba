#pragma once

#include "player/assist.h"
#include "player/touch.h"

#include <anyport/platform/window.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SDL_Haptic;
union SDL_Event;

// Touch controls on top of the game window: reads finger events, draws the buttons, the
// floating menu button and its quick menu, and hides itself while a gamepad or keyboard is
// in use. Finger events may arrive on another thread (Android delivers them from the UI
// thread), so they only touch state under a lock; update() and the getters run on the game
// thread once per host frame.
namespace anygba::player {

// SDL_SendAndroidMessage() commands to the activity: the touch settings dialog, and
// "publish the newest file in files/screenshots to the gallery".
inline constexpr std::uint32_t kTouchSettingsMessage = 0x8001;
inline constexpr std::uint32_t kScreenshotMessage = 0x8002;

// What the quick menu shows next to its entries; the player fills it in.
struct TouchStatus {
    bool fast_forward = false;      // the fast-forward toggle is on
    std::string fast_forward_speed; // "300%", "unlimited"
    int slot = 1;                   // the slot a quick save writes
    std::uint16_t saved_slots = 0;  // bit n-1: slot n holds a state
    std::string filter;             // short name of the screen effect
    std::string widescreen;         // "off", "16:10", "16:9"
    bool game_menu_open = false;
    bool operator==(const TouchStatus&) const = default;
};

// A quick menu choice the player carries out. SaveState writes the current slot (also the
// long press); LoadState names its slot.
struct TouchAction {
    assist::Item item = assist::Item::Count;
    int slot = 0;
};

class TouchOverlay final : public anyport::platform::ScreenOverlay {
public:
    // `settings_file` (anygba-touch.ini) is read now and again whenever it changes, and
    // written when the floating button or the layout editor changes it.
    explicit TouchOverlay(std::optional<std::filesystem::path> settings_file);
    ~TouchOverlay() override;

    TouchOverlay(const TouchOverlay&) = delete;
    TouchOverlay& operator=(const TouchOverlay&) = delete;

    anyport::platform::FRect picture_rect(float width, float height, anyport::platform::FRect safe) override;
    void draw(SDL_Renderer* renderer, float width, float height) override;

    // Once per host frame: haptics, the Menu tap/hold, the long press, settings reload/write.
    void update();
    // GBA buttons held by touch, KEYINPUT bit order.
    std::uint16_t gba_keys() const { return held_ & touch::kGbaMask; }
    // input::Hotkey bits: Menu for one frame after a short tap, FastForward while held.
    std::uint32_t hotkeys() const;
    // Quick menu choices since the last call.
    std::vector<TouchAction> take_actions();
    // The quick menu or the layout editor is open: the game should wait.
    bool paused() const;
    void set_status(const TouchStatus& status);
    // Width of the game picture in GBA pixels (240, or wider in widescreen).
    void set_picture_width(int width);

private:
    enum class Mode : std::uint8_t { Play, Panel, Slots, Edit };

    static bool on_event(void* self, SDL_Event* event);
    void handle(const SDL_Event& event);
    void finger_down(std::int64_t id, float x, float y, std::uint64_t now);
    void finger_move(std::int64_t id, float x, float y, std::uint64_t now);
    void finger_up(std::int64_t id, float x, float y, std::uint64_t now, bool cancelled);
    void choose(assist::Item item);
    void use_tool(assist::Tool tool);
    void relayout();
    void reload_settings();
    void write_settings(const touch::Settings& settings);
    void buzz();

    void draw_controls(SDL_Renderer* r, const touch::Layout& l, float alpha, bool editing, touch::Control selected) const;
    void draw_button(SDL_Renderer* r, assist::Point c, float d, float alpha) const;
    void draw_panel(SDL_Renderer* r, float w, float h, const TouchStatus& status, const touch::Settings& s,
                    Mode mode, int pressed) const;
    void draw_toolbar(SDL_Renderer* r, float w, float h, touch::Control selected) const;

    std::optional<std::filesystem::path> settings_file_;
    std::filesystem::file_time_type settings_time_{};

    mutable std::mutex mutex_;  // guards everything below up to held_
    touch::Settings settings_;
    touch::Layout layout_{};
    float width_ = 0, height_ = 0;
    anyport::platform::FRect safe_{};
    float picture_width_ = 240;
    std::map<std::int64_t, std::uint16_t> fingers_;
    bool hidden_ = false;
    Mode mode_ = Mode::Play;
    assist::Button button_;
    std::map<std::int64_t, int> panel_presses_;  // finger -> cell (or tool) it went down on
    touch::Control selected_ = touch::Control::Count;
    std::optional<std::int64_t> drag_finger_;
    assist::Point drag_from_{};
    touch::Adjust drag_start_{};
    TouchStatus status_;
    std::vector<TouchAction> actions_;
    bool settings_dirty_ = false;
    bool open_settings_ = false;
    bool long_press_ = false;

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
