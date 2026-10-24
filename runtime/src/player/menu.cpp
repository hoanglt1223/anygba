#include "player/menu.h"

#include "player/speed.h"
#include "video/settings.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace anygba::player {

namespace {

constexpr std::uint16_t kA = 1u << 0, kB = 1u << 1, kSelect = 1u << 2, kRight = 1u << 4, kLeft = 1u << 5,
                        kUp = 1u << 6, kDown = 1u << 7, kR = 1u << 8, kL = 1u << 9;
constexpr int kVisibleRows = 13;
constexpr int kRepeatDelay = 18;
constexpr int kRepeatEvery = 4;

constexpr cheats::Format kEntryFormats[] = {cheats::Format::Auto,         cheats::Format::GameShark,
                                            cheats::Format::ActionReplay, cheats::Format::CodeBreaker,
                                            cheats::Format::Raw,          cheats::Format::GameSharkRaw,
                                            cheats::Format::ActionReplayRaw};
constexpr int kEntryFormatCount = static_cast<int>(sizeof kEntryFormats / sizeof kEntryFormats[0]);

constexpr int kMainResume = 0, kMainSave = 1, kMainLoad = 2, kMainSpeed = 3, kMainCheats = 4, kMainControls = 5;
constexpr int kMainVideo = 6, kMainSettings = 7, kMainOptions = 8;
constexpr int kOptionsFolder = 0, kOptionsDefaultFolder = 1;
constexpr int kCustomGroup = -2;  // tree row: "My codes"
constexpr int kAddCode = -3;      // tree row: "Add code..."
constexpr int kReset = -4;        // controls row: reset to defaults

bool is_hex_key(const std::string& k) {
    return k.size() == 1 && ((k[0] >= '0' && k[0] <= '9') || (k[0] >= 'A' && k[0] <= 'F'));
}

bool is_modifier(const std::string& k) {
    return k == "Left Shift" || k == "Right Shift" || k == "Left Ctrl" || k == "Right Ctrl" || k == "Left Alt" ||
           k == "Right Alt";
}

// "82025E300001" -> "82025E30 0001"
std::string spaced(const std::string& digits) {
    return digits.size() > 8 ? digits.substr(0, 8) + " " + digits.substr(8) : digits;
}

std::string leaf(const std::string& name) {
    const auto slash = name.rfind('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

int target_count() { return input::kGbaButtons + input::kHotkeys; }

input::Target target_at(int i) {
    return i < input::kGbaButtons ? input::Target{false, i} : input::Target{true, i - input::kGbaButtons};
}

}  // namespace

void Menu::open() {
    open_ = true;
    page_ = Page::Main;
    cursor_ = 0;
    scroll_ = 0;
    cursor_stack_.clear();
    path_.clear();
    message_.clear();
    last_held_ = 0xFFFF;  // ignore buttons still held from opening the menu
}

std::uint16_t Menu::pressed_with_repeat(std::uint16_t held) {
    if (last_held_ == 0xFFFF) {
        last_held_ = held;  // first frame: everything held counts as old
        return 0;
    }
    std::uint16_t pressed = static_cast<std::uint16_t>(held & ~last_held_);
    if (held != 0 && held == last_held_) {
        if (++repeat_frames_ >= kRepeatDelay && (repeat_frames_ - kRepeatDelay) % kRepeatEvery == 0) {
            pressed |= held & (kUp | kDown | kLeft | kRight | kL | kR);
        }
    } else {
        repeat_frames_ = 0;
    }
    last_held_ = held;
    return pressed;
}

std::vector<MenuAction> Menu::update(const MenuInput& in) {
    std::vector<MenuAction> out;
    if (!open_) {
        return out;
    }
    const std::uint16_t press = pressed_with_repeat(in.gba_held);
    const bool escape = std::find(in.keys.begin(), in.keys.end(), "Escape") != in.keys.end();
    if (page_ == Page::Capture) {
        update_capture(in, out);
        return out;
    }
    if (page_ == Page::CodeEntry) {
        update_entry(press, in, out);
        return out;
    }
    if (escape || in.menu_hotkey) {
        open_ = false;
        out.push_back(MenuAction::Close);
        return out;
    }
    if (press) {
        message_.clear();
    }
    const int count = static_cast<int>(rows().size());
    if (page_ != Page::Picker && count > 0) {
        if (press & kDown) cursor_ = (cursor_ + 1) % count;
        if (press & kUp) cursor_ = (cursor_ + count - 1) % count;
        if (press & kR) cursor_ = std::min(cursor_ + kVisibleRows, count - 1);
        if (press & kL) cursor_ = std::max(cursor_ - kVisibleRows, 0);
    }
    switch (page_) {
    case Page::Main: update_main(press, out); break;
    case Page::Cheats: update_cheats(press, in, out); break;
    case Page::Picker: update_picker(press, in, out); break;
    case Page::Controls: update_controls(press, out); break;
    case Page::Video: update_video(press, out); break;
    case Page::Settings: update_settings(press, out); break;
    case Page::Options: update_options(press, out); break;
    case Page::Speed: update_speed(press, out); break;
    default: break;
    }
    const int rows_now = static_cast<int>(rows().size());
    cursor_ = std::clamp(cursor_, 0, std::max(rows_now - 1, 0));
    if (cursor_ < scroll_) scroll_ = cursor_;
    if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1;
    return out;
}

void Menu::update_main(std::uint16_t press, std::vector<MenuAction>& out) {
    if (press & kB) {
        open_ = false;
        out.push_back(MenuAction::Close);
        return;
    }
    const int dir = (press & kRight) ? 1 : (press & kLeft) ? -1 : 0;
    if ((cursor_ == kMainSave || cursor_ == kMainLoad) && dir) {
        slot_ = (slot_ + dir + 8) % 9 + 1;
    }
    if (cursor_ == kMainSpeed && dir) {
        const int next = next_speed(speed_, dir);
        if (next != speed_) {
            speed_ = next;
            out.push_back(MenuAction::SpeedChanged);
        }
    }
    if (!(press & kA)) {
        return;
    }
    // Video and Settings rows are optional, so act on the row's id, not its position.
    const auto main_rows = rows();
    if (cursor_ < 0 || cursor_ >= static_cast<int>(main_rows.size())) return;
    switch (main_rows[static_cast<std::size_t>(cursor_)].index) {
    case kMainResume:
        open_ = false;
        out.push_back(MenuAction::Close);
        break;
    case kMainSave:
    case kMainLoad:
        open_ = false;
        out.push_back(cursor_ == kMainSave ? MenuAction::SaveState : MenuAction::LoadState);
        out.push_back(MenuAction::Close);
        break;
    case kMainSpeed:
        cursor_stack_.push_back(cursor_);
        page_ = Page::Speed;
        cursor_ = 0;
        break;
    case kMainOptions:
        cursor_stack_.push_back(cursor_);
        page_ = Page::Options;
        cursor_ = 0;
        break;
    case kMainCheats:
        cursor_stack_.push_back(cursor_);
        page_ = Page::Cheats;
        path_.clear();
        cursor_ = 0;
        break;
    case kMainControls:
        cursor_stack_.push_back(cursor_);
        page_ = Page::Controls;
        cursor_ = 0;
        break;
    case kMainVideo:
        cursor_stack_.push_back(cursor_);
        page_ = Page::Video;
        cursor_ = 0;
        break;
    case kMainSettings:
        cursor_stack_.push_back(cursor_);
        page_ = Page::Settings;
        cursor_ = 0;
        break;
    default: break;
    }
}

void Menu::back_to_main() {
    page_ = Page::Main;
    cursor_ = cursor_stack_.empty() ? 0 : cursor_stack_.back();
    if (!cursor_stack_.empty()) cursor_stack_.pop_back();
}

void Menu::update_speed(std::uint16_t press, std::vector<MenuAction>& out) {
    if (press & kB) {
        back_to_main();
        return;
    }
    const int dir = (press & (kRight | kA)) ? 1 : (press & kLeft) ? -1 : 0;
    if (!dir) return;
    if (cursor_ == 0) {
        // A wraps around so the row can be cycled with one button.
        const int next = (press & kA) && speed_ == kSpeeds.back() ? kSpeeds.front() : next_speed(speed_, dir);
        if (next != speed_) {
            speed_ = next;
            out.push_back(MenuAction::SpeedChanged);
        }
    } else if (cursor_ == 1) {
        const int current = mapping_.fast_forward;
        const int next = (press & kA) && current == kFastForwardSpeeds.back() ? kFastForwardSpeeds.front()
                                                                               : next_fast_forward(current, dir);
        if (next != current) {
            mapping_.fast_forward = next;
            say("Fast-forward " + format_fast_forward(next));
            out.push_back(MenuAction::MappingChanged);
        }
    } else if (cursor_ == 2) {
        mapping_.normal_speed_music = !mapping_.normal_speed_music;
        out.push_back(MenuAction::MappingChanged);
    }
}

void Menu::update_options(std::uint16_t press, std::vector<MenuAction>& out) {
    if (press & kB) {
        back_to_main();
        return;
    }
    if (!(press & kA)) return;
    switch (cursor_) {
    case kOptionsFolder: out.push_back(MenuAction::ChooseSaveFolder); break;
    case kOptionsDefaultFolder:
        if (!mapping_.save_folder.empty()) {
            mapping_.save_folder.clear();
            say("Default save folder, from the next start");
            out.push_back(MenuAction::MappingChanged);
        }
        break;
    default: break;
    }
}

void Menu::update_settings(std::uint16_t press, std::vector<MenuAction>& out) {
    if (press & kB) {
        back_to_main();
        return;
    }
    if (!(press & (kA | kLeft | kRight)) || !settings_) return;
    bool& value = cursor_ == 0 ? settings_->ai_control : cursor_ == 1 ? settings_->readonly_agent : settings_->journal;
    value = !value;
    out.push_back(MenuAction::SettingsChanged);
}

std::vector<Menu::Node> Menu::nodes() const {
    std::vector<Node> out;
    if (path_ == "\x01") {  // the "My codes" group
        for (std::size_t i = 0; i < cheats_.cheats().size(); ++i) {
            if (cheats_.cheats()[i].custom) out.push_back({cheats_.cheats()[i].name, false, {}, static_cast<int>(i)});
        }
        out.push_back({"Add code...", false, {}, kAddCode});
        return out;
    }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < cheats_.cheats().size(); ++i) {
        const cheats::Cheat& c = cheats_.cheats()[i];
        if (c.custom || c.name.compare(0, path_.size(), path_) != 0) continue;
        const std::string rest = c.name.substr(path_.size());
        const auto slash = rest.find('/');
        if (slash != std::string::npos) {
            const std::string category = rest.substr(0, slash);
            if (seen.insert(category).second) out.push_back({category, true, path_ + category + "/", -1});
        } else {
            out.push_back({rest, false, {}, static_cast<int>(i)});
        }
    }
    if (path_.empty()) {
        out.push_back({"My codes", true, "\x01", kCustomGroup});
    }
    return out;
}

void Menu::update_cheats(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out) {
    const std::vector<Node> list = nodes();
    if (press & kB) {
        if (path_.empty()) {
            page_ = Page::Main;
        } else if (path_ == "\x01") {
            path_.clear();
        } else {
            const auto slash = path_.rfind('/', path_.size() - 2);
            path_ = slash == std::string::npos ? std::string() : path_.substr(0, slash + 1);
        }
        cursor_ = cursor_stack_.empty() ? 0 : cursor_stack_.back();
        if (!cursor_stack_.empty()) cursor_stack_.pop_back();
        return;
    }
    if (list.empty() || cursor_ >= static_cast<int>(list.size())) return;
    const Node& node = list[static_cast<std::size_t>(cursor_)];
    if (node.cheat == kAddCode && (press & kA)) {
        page_ = Page::CodeEntry;
        entry_lines_.clear();
        entry_current_.clear();
        entry_format_ = 0;
        return;
    }
    if (node.category) {
        if (press & (kA | kRight)) {
            cursor_stack_.push_back(cursor_);
            path_ = node.path;
            cursor_ = 0;
        }
        return;
    }
    const auto index = static_cast<std::size_t>(node.cheat);
    const cheats::Cheat& c = cheats_.cheats()[index];
    if ((press & kSelect) && c.custom) {
        say("Deleted " + c.name);
        cheats_.remove(index);
        out.push_back(MenuAction::CheatsChanged);
        return;
    }
    const bool open_picker = c.param && ((press & kRight) || ((press & kA) && !c.enabled));
    if (open_picker) {
        page_ = Page::Picker;
        picker_cheat_ = node.cheat;
        picker_value_ = c.value;
        picker_index_ = 0;
        for (std::size_t i = 0; i < c.param->choices.size(); ++i) {
            if (c.param->choices[i].value == c.value) picker_index_ = static_cast<int>(i);
        }
        return;
    }
    if (press & kA) {
        if (auto error = cheats_.set_enabled(index, !c.enabled)) {
            say("Error: " + *error);
        } else {
            say(std::string(cheats_.cheats()[index].enabled ? "On: " : "Off: ") + leaf(c.name));
            out.push_back(MenuAction::CheatsChanged);
        }
    }
    (void)in;
}

void Menu::update_picker(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out) {
    const auto index = static_cast<std::size_t>(picker_cheat_);
    const cheats::Cheat& c = cheats_.cheats()[index];
    const cheats::Param& p = *c.param;
    const auto back = [this] {
        page_ = Page::Cheats;
    };
    if (press & kB) {
        back();
        return;
    }
    if (press & kSelect) {
        cheats_.set_enabled(index, false);
        say("Off: " + leaf(c.name));
        out.push_back(MenuAction::CheatsChanged);
        back();
        return;
    }
    if (!p.choices.empty()) {
        const int n = static_cast<int>(p.choices.size());
        if (press & (kDown | kRight)) picker_index_ = (picker_index_ + 1) % n;
        if (press & (kUp | kLeft)) picker_index_ = (picker_index_ + n - 1) % n;
        if (press & kR) picker_index_ = std::min(picker_index_ + 10, n - 1);
        if (press & kL) picker_index_ = std::max(picker_index_ - 10, 0);
        // A typed letter or digit jumps to the next choice that starts with it.
        for (const std::string& k : in.keys) {
            if (k.size() != 1) continue;
            for (int step = 1; step <= n; ++step) {
                const int i = (picker_index_ + step) % n;
                const std::string& name = p.choices[static_cast<std::size_t>(i)].name;
                if (!name.empty() && std::toupper(static_cast<unsigned char>(name[0])) == k[0]) {
                    picker_index_ = i;
                    break;
                }
            }
        }
        picker_value_ = p.choices[static_cast<std::size_t>(picker_index_)].value;
    } else {
        const std::uint32_t span = p.max - p.min + 1;
        const auto add = [&](std::int64_t d) {
            const std::int64_t v = static_cast<std::int64_t>(picker_value_ - p.min) + d;
            picker_value_ = p.min + static_cast<std::uint32_t>(((v % span) + span) % span);
        };
        if (press & (kUp | kRight)) add(1);
        if (press & (kDown | kLeft)) add(-1);
        if (press & kR) add(10);
        if (press & kL) add(-10);
    }
    if (press & kA) {
        if (auto error = cheats_.set_value(index, picker_value_)) {
            say("Error: " + *error);
            return;
        }
        say("On: " + leaf(c.name) + " = " + p.describe(picker_value_));
        out.push_back(MenuAction::CheatsChanged);
        back();
    }
}

void Menu::update_entry(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out) {
    if (press & kRight) entry_format_ = (entry_format_ + 1) % kEntryFormatCount;
    if (press & kLeft) entry_format_ = (entry_format_ + kEntryFormatCount - 1) % kEntryFormatCount;
    for (const std::string& k : in.keys) {
        if (k == "Escape") {
            page_ = Page::Cheats;
            say("Code entry cancelled");
            return;
        }
        if (k == "Backspace") {
            if (!entry_current_.empty()) {
                entry_current_.pop_back();
            } else if (!entry_lines_.empty()) {
                entry_current_ = entry_lines_.back();
                entry_lines_.pop_back();
                std::erase(entry_current_, ' ');
            }
        } else if (k == "Return" || k == "Keypad Enter") {
            if (!entry_current_.empty()) {
                entry_lines_.push_back(spaced(entry_current_));
                entry_current_.clear();
                message_.clear();
            } else if (!entry_lines_.empty()) {
                const cheats::Format format = kEntryFormats[entry_format_];
                int number = 1;
                for (const auto& c : cheats_.cheats()) number += c.custom ? 1 : 0;
                try {
                    const std::size_t index = cheats_.add_custom("Code " + std::to_string(number), format, entry_lines_);
                    say(std::string("Added Code ") + std::to_string(number) + " (" +
                        cheats::format_label(cheats_.cheats()[index].format) + ")");
                    out.push_back(MenuAction::CheatsChanged);
                    page_ = Page::Cheats;
                } catch (const cheats::CodeError& e) {
                    say(std::string("Error: ") + e.what());
                }
            }
        } else if (is_hex_key(k) || (k.size() == 7 && k.rfind("Keypad ", 0) == 0 && std::isdigit(static_cast<unsigned char>(k[6])))) {
            if (entry_current_.size() < 16) entry_current_ += k.back();
        }
    }
}

void Menu::update_controls(std::uint16_t press, std::vector<MenuAction>& out) {
    if (press & kB) {
        page_ = Page::Main;
        cursor_ = cursor_stack_.empty() ? 0 : cursor_stack_.back();
        if (!cursor_stack_.empty()) cursor_stack_.pop_back();
        return;
    }
    if (!(press & kA)) return;
    if (cursor_ == target_count()) {
        // Only the bindings; the [player] settings stay.
        const input::Mapping defaults = input::default_mapping();
        mapping_.keys = defaults.keys;
        mapping_.pad = defaults.pad;
        mapping_.hotkeys = defaults.hotkeys;
        mapping_.pad_hotkeys = defaults.pad_hotkeys;
        say("Controls reset to defaults");
        out.push_back(MenuAction::MappingChanged);
        return;
    }
    capture_target_ = target_at(cursor_);
    pending_modifier_.clear();
    page_ = Page::Capture;
}

void Menu::bind(input::Device device, const std::string& binding, std::vector<MenuAction>& out) {
    const auto swapped = input::rebind(mapping_, capture_target_, device, binding);
    const std::string bound = input::describe(mapping_, capture_target_, device);
    say(input::target_name(capture_target_) + " = " + bound + (swapped ? " (swapped with " + *swapped + ")" : ""));
    out.push_back(MenuAction::MappingChanged);
    page_ = Page::Controls;
    last_held_ = 0xFFFF;  // the button just bound must not also act in the menu
}

void Menu::update_capture(const MenuInput& in, std::vector<MenuAction>& out) {
    for (const std::string& k : in.keys) {
        if (k == "Escape") {
            say("Cancelled");
            page_ = Page::Controls;
            last_held_ = 0xFFFF;
            return;
        }
        if (is_modifier(k)) {
            pending_modifier_ = k;
            continue;
        }
        bind(input::Device::Keyboard, input::keyboard_chord(k, in.ctrl, in.alt, in.shift), out);
        return;
    }
    // A modifier pressed and released on its own binds that key.
    if (!pending_modifier_.empty() && !in.ctrl && !in.alt && !in.shift) {
        bind(input::Device::Keyboard, pending_modifier_, out);
        return;
    }
    if (!in.pads.empty()) {
        std::vector<input::Button> chord;
        for (int b = 0; b < static_cast<int>(input::Button::Count); ++b) {
            if ((in.pad_held >> b) & 1u) chord.push_back(static_cast<input::Button>(b));
        }
        // The newest button last: a GBA button takes only that one.
        std::erase(chord, in.pads.back());
        chord.push_back(in.pads.back());
        if (!capture_target_.hotkey) {
            chord = {in.pads.back()};
        }
        std::string text;
        for (const auto b : chord) text += (text.empty() ? "" : "+") + std::string(input::pad_button_name(b));
        bind(input::Device::Gamepad, text, out);
    }
}

std::string Menu::title() const {
    switch (page_) {
    case Page::Main: return "AnyGBA";
    case Page::Cheats:
        return path_.empty() ? "Cheats" : path_ == "\x01" ? "Cheats / My codes" : "Cheats / " + path_.substr(0, path_.size() - 1);
    case Page::Picker: {
        const cheats::Cheat& c = cheats_.cheats()[static_cast<std::size_t>(picker_cheat_)];
        return leaf(c.name) + ": " + c.param->label;
    }
    case Page::CodeEntry: return "Add a cheat code";
    case Page::Controls: return "Controls";
    case Page::Capture: return "Rebind " + input::target_name(capture_target_);
    case Page::Video: return "Video";
    case Page::Settings: return "Settings";
    case Page::Options: return "Options";
    case Page::Speed: return "Speed";
    }
    return {};
}

std::string Menu::help() const {
    switch (page_) {
    case Page::Main: return "A select  </> change  B close";
    case Page::Cheats: return "A on/off  > value  Select delete  B back";
    case Page::Picker: return "A apply  L/R page  Select off  B back";
    case Page::CodeEntry: return "Type hex, Enter: next line  Esc cancel";
    case Page::Controls: return "A rebind  B back";
    case Page::Capture: return "Press a key or button  Esc cancel";
    case Page::Video: return "</> change  B back";
    case Page::Settings: return "A on/off  B back";
    case Page::Options: return "A choose  B back";
    case Page::Speed: return "</> change  B back";
    }
    return {};
}

std::vector<Menu::Row> Menu::rows() const {
    std::vector<Row> out;
    switch (page_) {
    case Page::Main:
        out.push_back({"Resume", "", kMainResume});
        out.push_back({"Save state", "< slot " + std::to_string(slot_) + " >", kMainSave});
        out.push_back({"Load state", "< slot " + std::to_string(slot_) + " >", kMainLoad});
        out.push_back({"Speed", "< " + std::to_string(speed_) + "% >", kMainSpeed});
        out.push_back({"Cheats", ">", kMainCheats});
        out.push_back({"Controls", ">", kMainControls});
        if (video_) out.push_back({"Video", ">", kMainVideo});
        if (settings_) out.push_back({"Settings", ">", kMainSettings});
        out.push_back({"Options", ">", kMainOptions});
        break;
    case Page::Speed:
        out.push_back({"Speed", "< " + std::to_string(speed_) + "% >", 0});
        out.push_back({"Fast-forward speed", "< " + format_fast_forward(mapping_.fast_forward) + " >", 1});
        out.push_back({"Normal-speed music", mapping_.normal_speed_music ? "On" : "Off", 2});
        break;
    case Page::Options: {
        // The end of a long path is the part that tells folders apart.
        std::string folder = mapping_.save_folder.empty() ? "default" : to_ascii(mapping_.save_folder);
        if (folder.size() > 22) folder = "..." + folder.substr(folder.size() - 19);
        out.push_back({"Save folder", folder, kOptionsFolder});
        out.push_back({"Use the default folder", "", kOptionsDefaultFolder});
        break;
    }
    case Page::Settings:
        if (settings_) {
            out.push_back({"AI control", settings_->ai_control ? "On" : "Off", 0});
            out.push_back({"Read-only agent", settings_->readonly_agent ? "On" : "Off", 1});
            out.push_back({"Journal", settings_->journal ? "On" : "Off", 2});
        }
        break;
    case Page::Cheats:
        for (const Node& n : nodes()) {
            if (n.category) {
                out.push_back({n.label, ">", -1});
            } else if (n.cheat < 0) {
                out.push_back({n.label, "", n.cheat});
            } else {
                const cheats::Cheat& c = cheats_.cheats()[static_cast<std::size_t>(n.cheat)];
                std::string text = std::string(c.enabled ? "[x] " : "[ ] ") + leaf(n.label);
                std::string value = c.param ? c.param->describe(c.value) : "";
                out.push_back({text, value, n.cheat});
            }
        }
        break;
    case Page::Picker: {
        const cheats::Cheat& c = cheats_.cheats()[static_cast<std::size_t>(picker_cheat_)];
        const cheats::Param& p = *c.param;
        if (p.choices.empty()) {
            out.push_back({"< " + std::to_string(picker_value_) + " >",
                           std::to_string(p.min) + "-" + std::to_string(p.max), 0});
        } else {
            for (std::size_t i = 0; i < p.choices.size(); ++i) {
                out.push_back({p.choices[i].name, std::to_string(p.choices[i].value), static_cast<int>(i)});
            }
        }
        break;
    }
    case Page::CodeEntry:
        out.push_back({"Format", std::string("< ") + cheats::format_label(kEntryFormats[entry_format_]) + " >", 0});
        for (const auto& l : entry_lines_) out.push_back({"  " + l, "", 1});
        out.push_back({"> " + spaced(entry_current_) + "_", "", 2});
        break;
    case Page::Controls:
    case Page::Capture:
        for (int i = 0; i < target_count(); ++i) {
            const input::Target t = target_at(i);
            const std::string kb = input::describe(mapping_, t, input::Device::Keyboard);
            const std::string pad = input::describe(mapping_, t, input::Device::Gamepad);
            out.push_back({input::target_name(t), kb + (pad.empty() ? "" : " | " + pad), i});
        }
        out.push_back({"Reset to defaults", "", kReset});
        break;
    case Page::Video:
        if (video_) {
            int i = 0;
            for (const auto& item : video::menu_items(*video_)) out.push_back({item.label, "< " + item.value + " >", i++});
        }
        break;
    }
    return out;
}

void Menu::update_video(std::uint16_t press, std::vector<MenuAction>& out) {
    if (press & kB) {
        page_ = Page::Main;
        cursor_ = cursor_stack_.empty() ? 0 : cursor_stack_.back();
        if (!cursor_stack_.empty()) cursor_stack_.pop_back();
        return;
    }
    const int dir = (press & (kRight | kA)) ? 1 : (press & kLeft) ? -1 : 0;
    if (dir && video_) {
        video::menu_step(*video_, cursor_, dir);
        out.push_back(MenuAction::VideoChanged);
    }
}

std::vector<std::string> Menu::lines() const {
    std::vector<std::string> out;
    out.push_back("== " + title() + " ==");
    const std::vector<Row> all = rows();
    const int selected = page_ == Page::Picker ? picker_index_ : page_ == Page::CodeEntry ? -1 : cursor_;
    const int first = page_ == Page::Picker ? std::max(0, picker_index_ - kVisibleRows / 2) : scroll_;
    for (int i = first; i < static_cast<int>(all.size()) && i < first + kVisibleRows; ++i) {
        const Row& r = all[static_cast<std::size_t>(i)];
        out.push_back(std::string(i == selected ? "> " : "  ") + r.text + (r.value.empty() ? "" : "  " + r.value));
    }
    if (!message_.empty()) out.push_back("! " + message_);
    return out;
}

void Menu::draw(Picture picture) const {
    if (!open_) return;
    shade(picture, 0, 0, 240, 160);
    shade(picture, 0, 0, 240, 160);
    draw_text(picture, 4, 3, to_ascii(title()).substr(0, kColumns - 1), color::kCyan);

    const std::vector<Row> all = rows();
    const int selected = page_ == Page::Picker ? picker_index_ : page_ == Page::CodeEntry ? -1 : cursor_;
    const int first = page_ == Page::Picker ? std::max(0, picker_index_ - kVisibleRows / 2) : scroll_;
    int y = 3 + kCellHeight + 3;
    for (int i = first; i < static_cast<int>(all.size()) && i < first + kVisibleRows; ++i) {
        const Row& r = all[static_cast<std::size_t>(i)];
        const bool sel = i == selected;
        const std::uint16_t c = sel ? color::kYellow : color::kWhite;
        std::string value = to_ascii(r.value);
        std::string text = to_ascii(r.text);
        const int value_cols = static_cast<int>(value.size());
        const int text_room = kColumns - 2 - (value_cols ? value_cols + 1 : 0);
        if (static_cast<int>(text.size()) > text_room) text = text.substr(0, static_cast<std::size_t>(std::max(text_room, 0)));
        draw_text(picture, 4, y, (sel ? ">" : " ") + text, c);
        if (value_cols) {
            draw_text(picture, 240 - 4 - value_cols * kCellWidth, y, value, sel ? color::kYellow : color::kGray);
        }
        y += kCellHeight;
    }
    const int bottom = 160 - kCellHeight - 1;
    if (!message_.empty()) {
        draw_text(picture, 4, bottom - kCellHeight, to_ascii(message_).substr(0, kColumns - 1), color::kGreen);
    }
    draw_text(picture, 4, bottom, help().substr(0, kColumns - 1), color::kGray);
}

}  // namespace anygba::player
