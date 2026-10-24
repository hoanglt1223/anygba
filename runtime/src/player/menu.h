#pragma once

#include "cheats/cheat_list.h"
#include "input_map.h"
#include "player/osd.h"
#include "anygba/video_options.h"

#include <cstdint>
#include <string>
#include <vector>

// The in-game menu (docs/user/USAGE.md "Menu"): save and load state slots, speed, the cheat
// tree with value pickers and typed codes, and the Controls page for rebinding. The game is
// paused while it is open; it is driven by the GBA buttons (d-pad, A, B, L, R, Select) from
// any keyboard or gamepad mapping, plus typed keys for codes and rebinding.
namespace anygba::player {

struct MenuInput {
    std::uint16_t gba_held = 0;          // GBA buttons held, from the mapping
    std::vector<std::string> keys;       // key names pressed this frame
    std::vector<input::Button> pads;     // gamepad buttons pressed this frame
    std::uint32_t pad_held = 0;          // gamepad buttons held, one bit per Button
    bool ctrl = false, alt = false, shift = false;
    bool menu_hotkey = false;            // the Menu hotkey was pressed this frame
};

enum class MenuAction : std::uint8_t { Close, SaveState, LoadState, SpeedChanged, CheatsChanged, MappingChanged, VideoChanged, SettingsChanged };

// The Settings page's switches (docs/user/AI-CONTROL.md); the player applies them.
struct MenuSettings {
    bool ai_control = false;
    bool readonly_agent = false;
    bool journal = false;
};

class Menu {
public:
    Menu(cheats::CheatList& cheats, input::Mapping& mapping, int& speed, int& slot)
        : cheats_(cheats), mapping_(mapping), speed_(speed), slot_(slot) {}

    void open();
    void close() { open_ = false; }
    // Shows the Settings page; null hides it.
    void set_settings(MenuSettings* settings) { settings_ = settings; }
    bool is_open() const { return open_; }
    // Shows the Video page, which edits these settings (owned by the caller).
    void set_video(video::Options* options) { video_ = options; }
    // One host frame. Returns what the player has to do.
    std::vector<MenuAction> update(const MenuInput& in);
    void draw(Picture picture) const;
    // The visible text, one string per row, title first (headless runs print it).
    std::vector<std::string> lines() const;

private:
    enum class Page : std::uint8_t { Main, Cheats, Picker, CodeEntry, Controls, Capture, Video, Settings, Speed };
    struct Row {
        std::string text;
        std::string value;  // right-aligned part
        int index = -1;     // cheat index, category marker, control target...
    };

    std::vector<Row> rows() const;
    std::string title() const;
    std::string help() const;
    std::uint16_t pressed_with_repeat(std::uint16_t held);

    void update_main(std::uint16_t press, std::vector<MenuAction>& out);
    void update_cheats(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out);
    void update_picker(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out);
    void update_entry(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out);
    void update_controls(std::uint16_t press, std::vector<MenuAction>& out);
    void update_settings(std::uint16_t press, std::vector<MenuAction>& out);
    void update_capture(const MenuInput& in, std::vector<MenuAction>& out);
    void update_video(std::uint16_t press, std::vector<MenuAction>& out);
    void update_speed(std::uint16_t press, std::vector<MenuAction>& out);
    void back_to_main();
    void bind(input::Device device, const std::string& binding, std::vector<MenuAction>& out);

    // Cheat tree: children of `path_` ("" = root, else "Items/").
    struct Node {
        std::string label;
        bool category = false;
        std::string path;  // category: full prefix with trailing '/'
        int cheat = -1;
    };
    std::vector<Node> nodes() const;
    void say(std::string text) { message_ = std::move(text); }

    cheats::CheatList& cheats_;
    input::Mapping& mapping_;
    int& speed_;
    int& slot_;
    video::Options* video_ = nullptr;
    MenuSettings* settings_ = nullptr;

    bool open_ = false;
    Page page_ = Page::Main;
    int cursor_ = 0;
    int scroll_ = 0;
    std::vector<int> cursor_stack_;
    std::string path_;
    int picker_cheat_ = -1;
    int picker_index_ = 0;           // into choices, or the value itself for ranges
    std::uint32_t picker_value_ = 0;
    int entry_format_ = 0;           // index into the entry format list
    std::vector<std::string> entry_lines_;
    std::string entry_current_;      // hex digits typed for the current line
    input::Target capture_target_;
    std::string pending_modifier_;
    std::string message_;
    std::uint16_t last_held_ = 0;
    int repeat_frames_ = 0;
};

}  // namespace anygba::player
