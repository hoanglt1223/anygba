#pragma once

#include "cheats/cheat_list.h"
#include "input_map.h"
#include "ui/anyui/menu_view.hpp"
#include "ui/anyui/strings.hpp"
#include "anygba/video_options.h"

#include <array>
#include <initializer_list>
#include <cstdint>
#include <string>
#include <vector>

// The in-game menu (docs/user/USAGE.md "Menu"): a rail of tabs (Game, Save states, Video,
// Audio, Controls, Cheats, AI, Settings) and the current tab's page. The game is paused
// while it is open. It is driven by the GBA buttons (d-pad, A, B, L, R, Select) from any
// keyboard or gamepad mapping, typed keys (codes, search, rebinding, Tab, Page Up/Down)
// and pointer clicks on what MenuView drew. Rows are identified by string ids; actions tell
// the player what to do. view() describes the page for MenuView, lines() as text for
// headless runs.
namespace anygba::player {

struct MenuInput {
    std::uint16_t gba_held = 0;          // GBA buttons held, from the mapping
    std::vector<std::string> keys;       // key names pressed this frame
    std::vector<input::Button> pads;     // gamepad buttons pressed this frame
    std::uint32_t pad_held = 0;          // gamepad buttons held, one bit per Button
    bool ctrl = false, alt = false, shift = false;
    bool menu_hotkey = false;            // the Menu hotkey was pressed this frame
};

enum class MenuAction : std::uint8_t { Close, SaveState, LoadState, SpeedChanged, CheatsChanged, MappingChanged, VideoChanged, SettingsChanged,
                                       ChooseSaveFolder, ExportSave, ImportSave, DeleteState, Reset, Exit, StatsChanged };

// The AI page's switches (docs/user/AI-CONTROL.md); the player applies them.
struct MenuSettings {
    bool ai_control = false;
    bool readonly_agent = false;
    bool journal = false;
};

// What the Save states page shows for one slot; the player fills it in.
struct SlotInfo {
    bool used = false;
    std::string when;                  // "2026-10-24 21:05"
    std::vector<std::uint32_t> thumb;  // RGBA (anyui pixel order), may be empty
    int thumb_width = 0, thumb_height = 0;
};
inline constexpr int kStateSlots = 9;

// The translations (ui/strings.tsv), parsed on first use.
anyui::Strings& menu_strings();

class Menu {
public:
    Menu(cheats::CheatList& cheats, input::Mapping& mapping, int& speed, int& slot)
        : cheats_(cheats), mapping_(mapping), speed_(speed), slot_(slot) {}

    void open();
    // Opens straight on the Cheats tab.
    void open_cheats();
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // Shows the AI tab; null hides it.
    void set_settings(MenuSettings* settings) { settings_ = settings; }
    // Shows the Video tab, which edits these settings (owned by the caller).
    void set_video(video::Options* options) { video_ = options; }
    void set_slots(const std::array<SlotInfo, kStateSlots>& slots) { slots_ = slots; }
    void set_game_title(std::string title) { game_title_ = std::move(title); }
    // A message from the player (results of actions), shown at the bottom of the menu.
    void say(std::string text, bool error = false) {
        message_ = std::move(text);
        message_error_ = error;
    }

    // One host frame. Returns what the player has to do.
    std::vector<MenuAction> update(const MenuInput& in);
    // A click (or tap) on something MenuView drew.
    std::vector<MenuAction> click(anyui::Hit hit);

    anyui::Frame view() const;
    // The visible text, title first (headless runs print it).
    std::vector<std::string> lines() const;

private:
    enum class Page : std::uint8_t { Game, States, Video, Audio, Controls, Capture, Cheats, Picker, CodeEntry, Ai, Settings };
    struct TabInfo {
        Page page;
        const char* id;
        anyui::Icon icon;
    };

    std::vector<TabInfo> tabs() const;
    std::vector<anyui::Row> rows() const;
    std::string title() const;
    std::string crumb() const;
    std::string hint() const;
    std::string tr(std::string_view key) const;
    std::string fmt(std::string_view key, std::initializer_list<std::string_view> args) const;
    std::uint16_t pressed_with_repeat(std::uint16_t held);
    bool at_tab_root() const;

    void go_tab(int index);
    void move_cursor(int delta);
    void back(std::vector<MenuAction>& out);
    void activate(const anyui::Row& row, std::vector<MenuAction>& out);
    void step(const anyui::Row& row, int dir, std::vector<MenuAction>& out);
    void close_menu(std::vector<MenuAction>& out);

    void update_states(std::uint16_t press, std::vector<MenuAction>& out);
    void slot_action(const std::string& id, std::vector<MenuAction>& out);
    void update_picker(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out);
    void update_entry(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out);
    void update_capture(const MenuInput& in, std::vector<MenuAction>& out);
    bool update_search(const MenuInput& in);
    void open_picker(int cheat);
    void apply_picker(std::vector<MenuAction>& out);
    void bind(input::Device device, const std::string& binding, std::vector<MenuAction>& out);
    std::vector<anyui::Row> slot_popup() const;

    // Cheat tree: children of `path_` ("" = root, else "Items/"), or the search results.
    struct Node {
        std::string label;
        bool category = false;
        std::string path;  // category: full prefix with trailing '/'
        int cheat = -1;
    };
    std::vector<Node> nodes() const;
    void info(std::string text) { say(std::move(text), false); }

    cheats::CheatList& cheats_;
    input::Mapping& mapping_;
    int& speed_;
    int& slot_;
    video::Options* video_ = nullptr;
    MenuSettings* settings_ = nullptr;
    std::array<SlotInfo, kStateSlots> slots_{};
    std::string game_title_;

    bool open_ = false;
    Page page_ = Page::Game;
    bool rail_ = false;  // the tab rail has the focus
    int cursor_ = 0;
    int scroll_ = 0;
    std::vector<int> cursor_stack_;
    std::string path_;
    std::string search_;
    bool searching_ = false;
    bool popup_ = false;  // the selected slot's actions are shown
    int popup_cursor_ = 0;
    std::string confirm_;  // a row id waiting for a second A
    int picker_cheat_ = -1;
    int picker_index_ = 0;           // into choices, or the value itself for ranges
    std::uint32_t picker_value_ = 0;
    int entry_format_ = 0;           // index into the entry format list
    std::vector<std::string> entry_lines_;
    std::string entry_current_;      // hex digits typed for the current line
    input::Target capture_target_;
    std::string pending_modifier_;
    std::string message_;
    bool message_error_ = false;
    std::uint16_t last_held_ = 0;
    int repeat_frames_ = 0;
};

}  // namespace anygba::player
