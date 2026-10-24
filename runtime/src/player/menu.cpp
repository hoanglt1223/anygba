#include "player/menu.h"

#include "player/speed.h"
#include "video/settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace anygba::player {

namespace {

using anyui::Icon;
using anyui::Row;
using anyui::RowKind;

constexpr std::uint16_t kA = 1u << 0, kB = 1u << 1, kSelect = 1u << 2, kRight = 1u << 4, kLeft = 1u << 5,
                        kUp = 1u << 6, kDown = 1u << 7, kR = 1u << 8, kL = 1u << 9;
constexpr int kVisibleRows = 13;
constexpr int kRepeatDelay = 18;
constexpr int kRepeatEvery = 4;
constexpr int kSlotColumns = 3;

constexpr cheats::Format kEntryFormats[] = {cheats::Format::Auto,         cheats::Format::GameShark,
                                            cheats::Format::ActionReplay, cheats::Format::CodeBreaker,
                                            cheats::Format::Raw,          cheats::Format::GameSharkRaw,
                                            cheats::Format::ActionReplayRaw};
constexpr int kEntryFormatCount = static_cast<int>(sizeof kEntryFormats / sizeof kEntryFormats[0]);
constexpr std::array<int, 4> kUiScales = {75, 100, 125, 150};
constexpr const char* kMine = "\x01";  // cheat tree path of the "My codes" group

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

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int target_count() { return input::kGbaButtons + input::kHotkeys; }

input::Target target_at(int i) {
    return i < input::kGbaButtons ? input::Target{false, i} : input::Target{true, i - input::kGbaButtons};
}

bool starts_with(const std::string& s, std::string_view prefix) { return s.rfind(prefix, 0) == 0; }

int id_number(const std::string& id) {
    const auto dot = id.rfind('.');
    return dot == std::string::npos ? -1 : std::atoi(id.c_str() + dot + 1);
}

// A row set up by `fill` (designated initializers would leave clang warning about the rest).
template <typename Fill>
Row make_row(Fill fill) {
    Row r;
    fill(r);
    return r;
}

bool selectable(const Row& r) { return r.kind != RowKind::Header && r.kind != RowKind::Info; }

}  // namespace

std::vector<Menu::TabInfo> Menu::tabs() const {
    std::vector<TabInfo> t{{Page::Game, "tab.game", Icon::Game}, {Page::States, "tab.states", Icon::States}};
    if (video_) t.push_back({Page::Video, "tab.video", Icon::Video});
    t.push_back({Page::Audio, "tab.audio", Icon::Audio});
    t.push_back({Page::Controls, "tab.controls", Icon::Controls});
    t.push_back({Page::Cheats, "tab.cheats", Icon::Cheats});
    if (settings_) t.push_back({Page::Ai, "tab.ai", Icon::Ai});
    t.push_back({Page::Settings, "tab.settings", Icon::Settings});
    return t;
}

std::string Menu::tr(std::string_view key) const {
    anyui::Strings& s = menu_strings();
    s.set_language(mapping_.language);
    return s.tr(key);
}

std::string Menu::fmt(std::string_view key, std::initializer_list<std::string_view> args) const {
    anyui::Strings& s = menu_strings();
    s.set_language(mapping_.language);
    return s.fmt(key, args);
}

void Menu::open() {
    open_ = true;
    page_ = Page::Game;
    rail_ = false;
    cursor_ = 0;
    scroll_ = 0;
    cursor_stack_.clear();
    path_.clear();
    search_.clear();
    searching_ = false;
    popup_ = false;
    confirm_.clear();
    message_.clear();
    last_held_ = 0xFFFF;  // ignore buttons still held from opening the menu
}

void Menu::open_cheats() {
    open();
    page_ = Page::Cheats;
}

bool Menu::at_tab_root() const {
    switch (page_) {
    case Page::Cheats: return path_.empty() && search_.empty();
    case Page::States: return !popup_;
    case Page::Picker:
    case Page::CodeEntry:
    case Page::Capture: return false;
    default: return true;
    }
}

void Menu::go_tab(int index) {
    const auto t = tabs();
    if (t.empty()) return;
    index = (index % static_cast<int>(t.size()) + static_cast<int>(t.size())) % static_cast<int>(t.size());
    page_ = t[static_cast<std::size_t>(index)].page;
    cursor_ = page_ == Page::States ? slot_ - 1 : 0;
    scroll_ = 0;
    cursor_stack_.clear();
    path_.clear();
    search_.clear();
    searching_ = false;
    popup_ = false;
    confirm_.clear();
    // The first selectable row.
    const auto r = rows();
    while (cursor_ < static_cast<int>(r.size()) && !selectable(r[static_cast<std::size_t>(cursor_)])) ++cursor_;
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

void Menu::close_menu(std::vector<MenuAction>& out) {
    open_ = false;
    out.push_back(MenuAction::Close);
}

void Menu::move_cursor(int delta) {
    const auto r = rows();
    const int n = static_cast<int>(r.size());
    if (n == 0) return;
    const int dir = delta > 0 ? 1 : -1;
    for (int steps = std::abs(delta); steps > 0; --steps) {
        int next = cursor_;
        for (int k = 0; k < n; ++k) {
            next = (next + dir + n) % n;
            if (selectable(r[static_cast<std::size_t>(next)])) break;
        }
        // Page jumps stop at the ends instead of wrapping.
        if (std::abs(delta) > 1 && ((dir > 0 && next < cursor_) || (dir < 0 && next > cursor_))) break;
        cursor_ = next;
    }
    confirm_.clear();
}

std::vector<MenuAction> Menu::update(const MenuInput& in) {
    std::vector<MenuAction> out;
    if (!open_) return out;
    const std::uint16_t press = pressed_with_repeat(in.gba_held);
    const auto has_key = [&](std::string_view k) { return std::find(in.keys.begin(), in.keys.end(), k) != in.keys.end(); };
    if (page_ == Page::Capture) {
        update_capture(in, out);
        return out;
    }
    if (page_ == Page::CodeEntry) {
        update_entry(press, in, out);
        return out;
    }
    if (searching_) {
        if (update_search(in)) return out;
    }
    if (has_key("Escape") || in.menu_hotkey) {
        if (popup_ && has_key("Escape")) {
            popup_ = false;
            return out;
        }
        close_menu(out);
        return out;
    }
    if (press) message_.clear();
    const bool tab_key = has_key("Tab");
    if (tab_key || ((press & (kL | kR)) && at_tab_root() && page_ != Page::Picker)) {
        const auto t = tabs();
        int current = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i].page == page_) current = static_cast<int>(i);
        }
        go_tab(current + ((tab_key ? in.shift : (press & kL) != 0) ? -1 : 1));
        return out;
    }
    if (rail_) {
        const auto t = tabs();
        int current = 0;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i].page == page_) current = static_cast<int>(i);
        }
        if (press & kDown) go_tab(current + 1);
        if (press & kUp) go_tab(current - 1);
        if (press & (kA | kRight)) rail_ = false;
        if (press & kB) close_menu(out);
        return out;
    }
    if (page_ == Page::States) {
        update_states(press, out);
        return out;
    }
    if (page_ == Page::Picker) {
        update_picker(press, in, out);
        return out;
    }
    const std::vector<Row> all = rows();
    const int count = static_cast<int>(all.size());
    if (count > 0) {
        if (press & kDown) move_cursor(1);
        if (press & kUp) {
            if (page_ == Page::Cheats && path_.empty() && cursor_ == 0) {
                searching_ = true;
                return out;
            }
            move_cursor(-1);
        }
        if (has_key("Page Down")) move_cursor(kVisibleRows);
        if (has_key("Page Up")) move_cursor(-kVisibleRows);
    }
    cursor_ = std::clamp(cursor_, 0, std::max(count - 1, 0));
    if (press & kB) {
        back(out);
    } else if (count > 0) {
        const Row& row = all[static_cast<std::size_t>(cursor_)];
        if (press & kA) {
            activate(row, out);
        } else if (press & kRight) {
            step(row, 1, out);
        } else if (press & kLeft) {
            const bool adjustable = row.kind == RowKind::Choice || row.kind == RowKind::Slider || row.kind == RowKind::Toggle;
            if (adjustable) {
                step(row, -1, out);
            } else if (at_tab_root()) {
                rail_ = true;
            } else {
                back(out);
            }
        } else if ((press & kSelect) && starts_with(row.id, "cheat.")) {
            const auto index = static_cast<std::size_t>(id_number(row.id));
            if (cheats_.cheats()[index].custom) {
                info(tr("cheats.deleted") + ": " + cheats_.cheats()[index].name);
                cheats_.remove(index);
                out.push_back(MenuAction::CheatsChanged);
            }
        }
    }
    if (page_ != Page::States) {
        const int rows_now = static_cast<int>(rows().size());
        cursor_ = std::clamp(cursor_, 0, std::max(rows_now - 1, 0));
        if (cursor_ < scroll_) scroll_ = cursor_;
        if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1;
    }
    return out;
}

bool Menu::update_search(const MenuInput& in) {
    // While typing, keys are text: the GBA buttons bound to letters must not act.
    last_held_ = 0xFFFF;
    for (const std::string& k : in.keys) {
        if (k == "Escape") {
            search_.clear();
            searching_ = false;
        } else if (k == "Return" || k == "Keypad Enter" || k == "Down" || k == "Up") {
            searching_ = false;
        } else if (k == "Backspace") {
            if (!search_.empty()) search_.pop_back();
        } else if (k == "Space") {
            search_ += ' ';
        } else if (k.size() == 1 && std::isprint(static_cast<unsigned char>(k[0]))) {
            search_ += static_cast<char>(std::tolower(static_cast<unsigned char>(k[0])));
        }
        cursor_ = 0;
        scroll_ = 0;
    }
    return true;
}

void Menu::back(std::vector<MenuAction>& out) {
    confirm_.clear();
    switch (page_) {
    case Page::Picker:
        page_ = Page::Cheats;
        cursor_ = cursor_stack_.empty() ? 0 : cursor_stack_.back();
        if (!cursor_stack_.empty()) cursor_stack_.pop_back();
        return;
    case Page::Cheats:
        if (!search_.empty() && path_.empty()) {
            search_.clear();
            cursor_ = 0;
            return;
        }
        if (!path_.empty()) {
            if (path_ == kMine) {
                path_.clear();
            } else {
                const auto slash = path_.rfind('/', path_.size() - 2);
                path_ = slash == std::string::npos ? std::string() : path_.substr(0, slash + 1);
            }
            cursor_ = cursor_stack_.empty() ? 0 : cursor_stack_.back();
            if (!cursor_stack_.empty()) cursor_stack_.pop_back();
            return;
        }
        break;
    case Page::States:
        if (popup_) {
            popup_ = false;
            return;
        }
        break;
    default: break;
    }
    close_menu(out);
}

void Menu::activate(const Row& row, std::vector<MenuAction>& out) {
    const std::string& id = row.id;
    if (!row.enabled) return;
    if (id == "game.resume") {
        close_menu(out);
    } else if (id == "game.reset" || id == "game.exit") {
        if (confirm_ != id) {
            confirm_ = id;
            info(tr(id == "game.reset" ? "game.reset.confirm" : "game.exit.confirm"));
            return;
        }
        confirm_.clear();
        open_ = false;
        out.push_back(id == "game.reset" ? MenuAction::Reset : MenuAction::Exit);
        out.push_back(MenuAction::Close);
    } else if (row.kind == RowKind::Choice || row.kind == RowKind::Toggle || row.kind == RowKind::Slider) {
        step(row, 1, out);  // A cycles, wrapping around
    } else if (id == "settings.folder") {
        out.push_back(MenuAction::ChooseSaveFolder);
    } else if (id == "settings.export") {
        out.push_back(MenuAction::ExportSave);
    } else if (id == "settings.import") {
        out.push_back(MenuAction::ImportSave);
    } else if (id == "settings.default_folder") {
        if (!mapping_.save_folder.empty()) {
            mapping_.save_folder.clear();
            info(tr("settings.default_folder.done"));
            out.push_back(MenuAction::MappingChanged);
        }
    } else if (id == "controls.reset") {
        // Only the bindings; the [player] settings stay.
        const input::Mapping defaults = input::default_mapping();
        mapping_.keys = defaults.keys;
        mapping_.pad = defaults.pad;
        mapping_.hotkeys = defaults.hotkeys;
        mapping_.pad_hotkeys = defaults.pad_hotkeys;
        info(tr("controls.reset.done"));
        out.push_back(MenuAction::MappingChanged);
    } else if (starts_with(id, "bind.")) {
        capture_target_ = target_at(id_number(id));
        pending_modifier_.clear();
        page_ = Page::Capture;
    } else if (starts_with(id, "cat:") || id == "cheats.mine") {
        cursor_stack_.push_back(cursor_);
        path_ = id == "cheats.mine" ? std::string(kMine) : id.substr(4);
        search_.clear();
        cursor_ = 0;
    } else if (id == "cheats.add") {
        page_ = Page::CodeEntry;
        entry_lines_.clear();
        entry_current_.clear();
        entry_format_ = 0;
    } else if (starts_with(id, "cheat.")) {
        const auto index = static_cast<std::size_t>(id_number(id));
        const cheats::Cheat& c = cheats_.cheats()[index];
        if (c.param && !c.enabled) {
            open_picker(static_cast<int>(index));
            return;
        }
        if (auto error = cheats_.set_enabled(index, !c.enabled)) {
            say(tr("error") + ": " + *error, true);
        } else {
            info(tr(cheats_.cheats()[index].enabled ? "cheats.on" : "cheats.off") + ": " + leaf(c.name));
            out.push_back(MenuAction::CheatsChanged);
        }
    }
}

void Menu::step(const Row& row, int dir, std::vector<MenuAction>& out) {
    const std::string& id = row.id;
    confirm_.clear();
    const bool wrap = dir > 0;  // from A: cycle around
    if (id == "game.speed") {
        const int next = wrap && speed_ == kSpeeds.back() && dir > 0 ? kSpeeds.front() : next_speed(speed_, dir);
        if (next != speed_) {
            speed_ = next;
            out.push_back(MenuAction::SpeedChanged);
        }
    } else if (id == "game.ff") {
        const int current = mapping_.fast_forward;
        const int next = wrap && current == kFastForwardSpeeds.back() ? kFastForwardSpeeds.front() : next_fast_forward(current, dir);
        if (next != current) {
            mapping_.fast_forward = next;
            info(tr("game.ff") + ": " + format_fast_forward(next));
            out.push_back(MenuAction::MappingChanged);
        }
    } else if (id == "audio.volume") {
        const int next = std::clamp(mapping_.volume + dir * 10, 0, 100);
        if (next != mapping_.volume) {
            mapping_.volume = next;
            out.push_back(MenuAction::MappingChanged);
        }
    } else if (id == "audio.mute") {
        mapping_.mute = !mapping_.mute;
        out.push_back(MenuAction::MappingChanged);
    } else if (id == "audio.music") {
        mapping_.normal_speed_music = !mapping_.normal_speed_music;
        out.push_back(MenuAction::MappingChanged);
    } else if (starts_with(id, "video.") && video_) {
        video::menu_step(*video_, id_number(id), dir);
        out.push_back(MenuAction::VideoChanged);
    } else if (starts_with(id, "ai.") && settings_) {
        bool& value = id == "ai.control" ? settings_->ai_control : id == "ai.readonly" ? settings_->readonly_agent : settings_->journal;
        value = !value;
        out.push_back(MenuAction::SettingsChanged);
    } else if (id == "settings.language") {
        mapping_.language = mapping_.language == "en" ? "vi" : "en";
        out.push_back(MenuAction::MappingChanged);
    } else if (id == "settings.theme") {
        mapping_.light_theme = !mapping_.light_theme;
        out.push_back(MenuAction::MappingChanged);
    } else if (id == "settings.scale") {
        const auto at = std::find(kUiScales.begin(), kUiScales.end(), mapping_.ui_scale);
        int i = at == kUiScales.end() ? 1 : static_cast<int>(at - kUiScales.begin());
        i = ((i + dir) % static_cast<int>(kUiScales.size()) + static_cast<int>(kUiScales.size())) % static_cast<int>(kUiScales.size());
        mapping_.ui_scale = kUiScales[static_cast<std::size_t>(i)];
        out.push_back(MenuAction::MappingChanged);
    } else if (id == "settings.motion") {
        mapping_.reduce_motion = !mapping_.reduce_motion;
        out.push_back(MenuAction::MappingChanged);
    } else if (id == "settings.stats") {
        mapping_.stats = !mapping_.stats;
        out.push_back(MenuAction::StatsChanged);
        out.push_back(MenuAction::MappingChanged);
    } else if (starts_with(id, "cheat.") && dir > 0) {
        const auto index = static_cast<std::size_t>(id_number(id));
        if (cheats_.cheats()[index].param) open_picker(static_cast<int>(index));
    } else if ((starts_with(id, "cat:") || id == "cheats.mine" || id == "settings.folder") && dir > 0) {
        activate(row, out);
    }
}

void Menu::update_states(std::uint16_t press, std::vector<MenuAction>& out) {
    if (popup_) {
        const auto actions = slot_popup();
        const int n = static_cast<int>(actions.size());
        if (press & kDown) popup_cursor_ = (popup_cursor_ + 1) % n;
        if (press & kUp) popup_cursor_ = (popup_cursor_ + n - 1) % n;
        if (press & kB) popup_ = false;
        if (press & kA) slot_action(actions[static_cast<std::size_t>(std::clamp(popup_cursor_, 0, n - 1))].id, out);
        return;
    }
    cursor_ = std::clamp(cursor_, 0, kStateSlots - 1);
    const int col = cursor_ % kSlotColumns;
    if (press & kRight) cursor_ = col == kSlotColumns - 1 ? cursor_ - col : cursor_ + 1;
    if (press & kLeft) {
        if (col == 0) {
            rail_ = true;
            return;
        }
        --cursor_;
    }
    if (press & kDown) cursor_ = (cursor_ + kSlotColumns) % kStateSlots;
    if (press & kUp) cursor_ = (cursor_ + kStateSlots - kSlotColumns) % kStateSlots;
    slot_ = cursor_ + 1;
    if (press & kB) {
        close_menu(out);
        return;
    }
    if ((press & kSelect) && slots_[static_cast<std::size_t>(cursor_)].used) {
        slot_action("slot.delete", out);
        return;
    }
    if (press & kA) {
        popup_ = true;
        popup_cursor_ = 0;
    }
}

std::vector<Row> Menu::slot_popup() const {
    std::vector<Row> r;
    const bool used = slots_[static_cast<std::size_t>(std::clamp(cursor_, 0, kStateSlots - 1))].used;
    if (used) r.push_back(make_row([&](Row& x) { x.id = "slot.load"; x.label = tr("states.load"); x.icon = Icon::Load; }));
    r.push_back(make_row([&](Row& x) { x.id = "slot.save"; x.label = tr(used ? "states.overwrite" : "states.save"); x.icon = Icon::Save; }));
    if (used) r.push_back(make_row([&](Row& x) { x.id = "slot.delete"; x.label = tr("states.delete"); x.danger = true; x.icon = Icon::Delete; }));
    r.push_back(make_row([&](Row& x) { x.id = "slot.cancel"; x.label = tr("states.cancel"); x.icon = Icon::Close; }));
    return r;
}

void Menu::slot_action(const std::string& id, std::vector<MenuAction>& out) {
    popup_ = false;
    slot_ = std::clamp(cursor_, 0, kStateSlots - 1) + 1;
    if (id == "slot.load" || id == "slot.save") {
        open_ = false;
        out.push_back(id == "slot.load" ? MenuAction::LoadState : MenuAction::SaveState);
        out.push_back(MenuAction::Close);
    } else if (id == "slot.delete") {
        out.push_back(MenuAction::DeleteState);
    }
}

std::vector<Menu::Node> Menu::nodes() const {
    std::vector<Node> out;
    if (path_ == kMine) {
        for (std::size_t i = 0; i < cheats_.cheats().size(); ++i) {
            if (cheats_.cheats()[i].custom) out.push_back({cheats_.cheats()[i].name, false, {}, static_cast<int>(i)});
        }
        return out;
    }
    if (!search_.empty() && path_.empty()) {
        const std::string want = lower(search_);
        for (std::size_t i = 0; i < cheats_.cheats().size(); ++i) {
            const cheats::Cheat& c = cheats_.cheats()[i];
            if (lower(c.name).find(want) != std::string::npos) out.push_back({c.name, false, {}, static_cast<int>(i)});
        }
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
    return out;
}

void Menu::open_picker(int cheat) {
    const cheats::Cheat& c = cheats_.cheats()[static_cast<std::size_t>(cheat)];
    cursor_stack_.push_back(cursor_);
    page_ = Page::Picker;
    picker_cheat_ = cheat;
    picker_value_ = c.value;
    picker_index_ = 0;
    for (std::size_t i = 0; i < c.param->choices.size(); ++i) {
        if (c.param->choices[i].value == c.value) picker_index_ = static_cast<int>(i);
    }
    cursor_ = c.param->choices.empty() ? 0 : picker_index_;
}

void Menu::apply_picker(std::vector<MenuAction>& out) {
    const auto index = static_cast<std::size_t>(picker_cheat_);
    const cheats::Cheat& c = cheats_.cheats()[index];
    if (auto error = cheats_.set_value(index, picker_value_)) {
        say(tr("error") + ": " + *error, true);
        return;
    }
    info(tr("cheats.on") + ": " + leaf(c.name) + " = " + c.param->describe(picker_value_));
    out.push_back(MenuAction::CheatsChanged);
    std::vector<MenuAction> ignored;
    back(ignored);
}

void Menu::update_picker(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out) {
    const auto index = static_cast<std::size_t>(picker_cheat_);
    const cheats::Cheat& c = cheats_.cheats()[index];
    const cheats::Param& p = *c.param;
    if (press & kB) {
        back(out);
        return;
    }
    if (press & kSelect) {
        cheats_.set_enabled(index, false);
        info(tr("cheats.off") + ": " + leaf(c.name));
        out.push_back(MenuAction::CheatsChanged);
        back(out);
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
        cursor_ = picker_index_;
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
        cursor_ = 0;
    }
    if (press & kA) apply_picker(out);
}

void Menu::update_entry(std::uint16_t press, const MenuInput& in, std::vector<MenuAction>& out) {
    if (press & kRight) entry_format_ = (entry_format_ + 1) % kEntryFormatCount;
    if (press & kLeft) entry_format_ = (entry_format_ + kEntryFormatCount - 1) % kEntryFormatCount;
    for (const std::string& k : in.keys) {
        if (k == "Escape") {
            page_ = Page::Cheats;
            info(tr("cheats.entry.cancelled"));
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
                    info(fmt("cheats.added", {std::to_string(number), cheats::format_label(cheats_.cheats()[index].format)}));
                    out.push_back(MenuAction::CheatsChanged);
                    page_ = Page::Cheats;
                } catch (const cheats::CodeError& e) {
                    say(tr("error") + ": " + e.what(), true);
                }
            }
        } else if (is_hex_key(k) || (k.size() == 7 && k.rfind("Keypad ", 0) == 0 && std::isdigit(static_cast<unsigned char>(k[6])))) {
            if (entry_current_.size() < 16) entry_current_ += k.back();
        }
    }
}

void Menu::bind(input::Device device, const std::string& binding, std::vector<MenuAction>& out) {
    const auto swapped = input::rebind(mapping_, capture_target_, device, binding);
    const std::string bound = input::describe(mapping_, capture_target_, device);
    info(input::target_name(capture_target_) + " = " + bound +
         (swapped ? " (" + fmt("controls.swapped", {*swapped}) + ")" : ""));
    out.push_back(MenuAction::MappingChanged);
    page_ = Page::Controls;
    last_held_ = 0xFFFF;  // the button just bound must not also act in the menu
}

void Menu::update_capture(const MenuInput& in, std::vector<MenuAction>& out) {
    for (const std::string& k : in.keys) {
        if (k == "Escape") {
            info(tr("controls.cancelled"));
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

std::vector<MenuAction> Menu::click(anyui::Hit hit) {
    std::vector<MenuAction> out;
    if (!open_) return out;
    using anyui::HitKind;
    message_.clear();
    if (page_ == Page::Capture) {
        if (hit.kind == HitKind::Outside || hit.kind == HitKind::Close) {
            info(tr("controls.cancelled"));
            page_ = Page::Controls;
        }
        return out;
    }
    searching_ = hit.kind == HitKind::Search;
    switch (hit.kind) {
    case HitKind::Tab:
        go_tab(hit.index);
        rail_ = false;
        break;
    case HitKind::Close: close_menu(out); break;
    case HitKind::Outside:
        if (popup_) popup_ = false;
        else close_menu(out);
        break;
    case HitKind::Back: back(out); break;
    case HitKind::Slot:
        rail_ = false;
        cursor_ = std::clamp(hit.index, 0, kStateSlots - 1);
        slot_ = cursor_ + 1;
        popup_ = true;
        popup_cursor_ = 0;
        break;
    case HitKind::Popup: {
        const auto actions = slot_popup();
        if (hit.index >= 0 && hit.index < static_cast<int>(actions.size())) slot_action(actions[static_cast<std::size_t>(hit.index)].id, out);
        break;
    }
    case HitKind::Row:
    case HitKind::Decrease:
    case HitKind::Increase: {
        rail_ = false;
        const auto all = rows();
        if (hit.index < 0 || hit.index >= static_cast<int>(all.size())) break;
        const Row& row = all[static_cast<std::size_t>(hit.index)];
        if (!selectable(row)) break;
        if (cursor_ != hit.index) confirm_.clear();
        cursor_ = hit.index;
        if (page_ == Page::Picker) {
            if (!cheats_.cheats()[static_cast<std::size_t>(picker_cheat_)].param->choices.empty()) {
                picker_index_ = hit.index;
                picker_value_ = cheats_.cheats()[static_cast<std::size_t>(picker_cheat_)].param->choices[static_cast<std::size_t>(hit.index)].value;
                apply_picker(out);
            } else if (hit.kind != HitKind::Row) {
                const cheats::Param& p = *cheats_.cheats()[static_cast<std::size_t>(picker_cheat_)].param;
                const std::uint32_t span = p.max - p.min + 1;
                const std::int64_t v = static_cast<std::int64_t>(picker_value_ - p.min) + (hit.kind == HitKind::Increase ? 1 : -1);
                picker_value_ = p.min + static_cast<std::uint32_t>(((v % span) + span) % span);
            } else {
                apply_picker(out);
            }
            break;
        }
        if (page_ == Page::CodeEntry) break;
        if (hit.kind == HitKind::Decrease) step(row, -1, out);
        else if (hit.kind == HitKind::Increase) step(row, 1, out);
        else if (row.kind != RowKind::Choice && row.kind != RowKind::Slider) activate(row, out);
        break;
    }
    default: break;
    }
    return out;
}

std::string Menu::title() const {
    switch (page_) {
    case Page::Game: return tr("tab.game");
    case Page::States: return tr("tab.states");
    case Page::Video: return tr("tab.video");
    case Page::Audio: return tr("tab.audio");
    case Page::Controls: return tr("tab.controls");
    case Page::Capture: return tr("controls.rebind") + " " + input::target_name(capture_target_);
    case Page::Cheats: return tr("tab.cheats");
    case Page::Picker: {
        const cheats::Cheat& c = cheats_.cheats()[static_cast<std::size_t>(picker_cheat_)];
        return leaf(c.name) + ": " + c.param->label;
    }
    case Page::CodeEntry: return tr("cheats.entry.title");
    case Page::Ai: return tr("tab.ai");
    case Page::Settings: return tr("tab.settings");
    }
    return {};
}

std::string Menu::crumb() const {
    if (page_ == Page::Cheats) {
        if (path_ == kMine) return tr("cheats.mine");
        if (!path_.empty()) {
            std::string c = path_.substr(0, path_.size() - 1);
            for (std::size_t at = c.find('/'); at != std::string::npos; at = c.find('/', at + 3)) c.replace(at, 1, " / ");
            return c;
        }
        if (!search_.empty()) return fmt("cheats.results", {std::to_string(nodes().size())});
    }
    if (page_ == Page::Picker) return tr("cheats.picker.crumb");
    return {};
}

std::string Menu::hint() const {
    if (rail_) return tr("hint.rail");
    switch (page_) {
    case Page::States: return tr(popup_ ? "hint.popup" : "hint.states");
    case Page::Cheats: return tr(searching_ ? "hint.search" : at_tab_root() ? "hint.cheats" : "hint.cheats.sub");
    case Page::Picker: return tr("hint.picker");
    case Page::CodeEntry: return tr("hint.entry");
    case Page::Capture: return tr("hint.capture");
    default: return tr("hint.page");
    }
}

std::vector<Row> Menu::rows() const {
    std::vector<Row> out;
    const auto on_off = [&](bool on) { return tr(on ? "on" : "off"); };
    const auto toggle = [&](const char* id, bool on, Icon icon = Icon::None, std::string detail = {}) {
        out.push_back(make_row([&](Row& x) { x.id = id; x.kind = RowKind::Toggle; x.label = tr(id); x.value = on_off(on); x.on = on; x.icon = icon; x.detail = std::move(detail); }));
    };
    const auto choice = [&](const char* id, std::string value, Icon icon = Icon::None) {
        out.push_back(make_row([&](Row& x) { x.id = id; x.kind = RowKind::Choice; x.label = tr(id); x.value = std::move(value); x.icon = icon; }));
    };
    switch (page_) {
    case Page::Game:
        out.push_back(make_row([&](Row& x) { x.id = "game.resume"; x.label = tr("game.resume"); x.icon = Icon::Resume; }));
        choice("game.speed", std::to_string(speed_) + "%", Icon::Speed);
        choice("game.ff", format_fast_forward(mapping_.fast_forward), Icon::Speed);
        out.push_back(make_row([&](Row& x) { x.id = "game.reset"; x.label = tr("game.reset"); x.value = confirm_ == "game.reset" ? tr("confirm") : ""; x.icon = Icon::Reset; }));
        out.push_back(make_row([&](Row& x) { x.id = "game.exit"; x.label = tr("game.exit"); x.value = confirm_ == "game.exit" ? tr("confirm") : ""; x.danger = true; x.icon = Icon::Exit; }));
        break;
    case Page::States: break;  // a grid: see view() and lines()
    case Page::Video:
        if (video_) {
            int i = 0;
            for (const auto& item : video::menu_items(*video_)) {
                const bool flag = item.toggle;
                Row r = make_row([&](Row& x) { x.id = "video." + std::to_string(i++); x.kind = flag ? RowKind::Toggle : RowKind::Choice; x.label = tr(item.label); x.value = flag ? on_off(item.value == "on") : item.value; x.on = item.value == "on"; });
                out.push_back(std::move(r));
            }
        }
        break;
    case Page::Audio:
        out.push_back(make_row([&](Row& x) { x.id = "audio.volume"; x.kind = RowKind::Slider; x.label = tr("audio.volume"); x.value = std::to_string(mapping_.volume) + "%"; x.level = static_cast<float>(mapping_.volume) / 100.f; x.icon = Icon::Audio; }));
        toggle("audio.mute", mapping_.mute, Icon::Mute);
        toggle("audio.music", mapping_.normal_speed_music, Icon::Speed, tr("audio.music.detail"));
        break;
    case Page::Controls:
    case Page::Capture:
        out.push_back(make_row([&](Row& x) { x.id = "hdr.buttons"; x.kind = RowKind::Header; x.label = tr("controls.buttons"); }));
        for (int i = 0; i < target_count(); ++i) {
            if (i == input::kGbaButtons) out.push_back(make_row([&](Row& x) { x.id = "hdr.hotkeys"; x.kind = RowKind::Header; x.label = tr("controls.hotkeys"); }));
            const input::Target t = target_at(i);
            out.push_back(make_row([&](Row& x) { x.id = "bind." + std::to_string(i); x.kind = RowKind::Binding; x.label = input::target_name(t); x.value = input::describe(mapping_, t, input::Device::Keyboard); x.value2 = input::describe(mapping_, t, input::Device::Gamepad); }));
        }
        out.push_back(make_row([&](Row& x) { x.id = "controls.reset"; x.label = tr("controls.reset"); x.icon = Icon::Reset; }));
        break;
    case Page::Cheats: {
        for (const Node& n : nodes()) {
            if (n.category) {
                out.push_back(make_row([&](Row& x) { x.id = "cat:" + n.path; x.kind = RowKind::Link; x.label = n.label; x.icon = Icon::Folder; }));
            } else {
                const cheats::Cheat& c = cheats_.cheats()[static_cast<std::size_t>(n.cheat)];
                Row r = make_row([&](Row& x) { x.id = "cheat." + std::to_string(n.cheat); x.kind = RowKind::Check; x.label = search_.empty() || !path_.empty() ? leaf(n.label) : leaf(c.name); x.value = c.param ? c.param->describe(c.value) : ""; x.on = c.enabled; x.more = c.param.has_value(); });
                if (!search_.empty() && path_.empty() && c.name.find('/') != std::string::npos) {
                    r.detail = c.name.substr(0, c.name.rfind('/'));
                }
                out.push_back(std::move(r));
            }
        }
        if (path_.empty() && search_.empty()) {
            out.push_back(make_row([&](Row& x) { x.id = "cheats.mine"; x.kind = RowKind::Link; x.label = tr("cheats.mine"); x.icon = Icon::Folder; }));
        }
        if (path_ == kMine) out.push_back(make_row([&](Row& x) { x.id = "cheats.add"; x.label = tr("cheats.add"); x.icon = Icon::Plus; }));
        break;
    }
    case Page::Picker: {
        const cheats::Cheat& c = cheats_.cheats()[static_cast<std::size_t>(picker_cheat_)];
        const cheats::Param& p = *c.param;
        if (p.choices.empty()) {
            out.push_back(make_row([&](Row& x) { x.id = "range"; x.kind = RowKind::Choice; x.label = p.label; x.value = std::to_string(picker_value_); x.detail = std::to_string(p.min) + "-" + std::to_string(p.max); }));
        } else {
            for (std::size_t i = 0; i < p.choices.size(); ++i) {
                out.push_back(make_row([&](Row& x) { x.id = "choice." + std::to_string(i); x.kind = RowKind::Info; x.label = p.choices[i].name; x.value = std::to_string(p.choices[i].value); }));
                out.back().kind = RowKind::Action;
            }
        }
        break;
    }
    case Page::CodeEntry:
        out.push_back(make_row([&](Row& x) { x.id = "entry.format"; x.kind = RowKind::Choice; x.label = tr("cheats.entry.format"); x.value = cheats::format_label(kEntryFormats[entry_format_]); }));
        for (const auto& l : entry_lines_) out.push_back(make_row([&](Row& x) { x.id = "entry.line"; x.kind = RowKind::Info; x.label = "  " + l; }));
        out.push_back(make_row([&](Row& x) { x.id = "entry.current"; x.kind = RowKind::Field; x.label = spaced(entry_current_); }));
        break;
    case Page::Ai:
        if (settings_) {
            toggle("ai.control", settings_->ai_control, Icon::Ai);
            toggle("ai.readonly", settings_->readonly_agent, Icon::Info);
            toggle("ai.journal", settings_->journal, Icon::Save);
        }
        break;
    case Page::Settings:
        choice("settings.language", tr(mapping_.language == "vi" ? "language.vi" : "language.en"), Icon::Language);
        choice("settings.theme", tr(mapping_.light_theme ? "theme.light" : "theme.dark"), Icon::Theme);
        choice("settings.scale", std::to_string(mapping_.ui_scale) + "%", Icon::Video);
        toggle("settings.motion", mapping_.reduce_motion, Icon::Resume);
        toggle("settings.stats", mapping_.stats, Icon::Info);
        out.push_back(make_row([&](Row& x) { x.id = "settings.folder"; x.kind = RowKind::Link; x.label = tr("settings.folder"); x.value = mapping_.save_folder.empty() ? tr("folder.default") : mapping_.save_folder; x.icon = Icon::Folder; }));
        out.push_back(make_row([&](Row& x) { x.id = "settings.default_folder"; x.label = tr("settings.default_folder"); x.icon = Icon::Reset; }));
        out.push_back(make_row([&](Row& x) { x.id = "settings.export"; x.label = tr("settings.export"); x.icon = Icon::Save; }));
        out.push_back(make_row([&](Row& x) { x.id = "settings.import"; x.label = tr("settings.import"); x.icon = Icon::Load; }));
        break;
    }
    return out;
}

anyui::Frame Menu::view() const {
    anyui::Frame f;
    f.brand = "AnyGBA";
    f.game = game_title_;
    const auto t = tabs();
    const Page root = page_ == Page::Capture ? Page::Controls : page_ == Page::Picker || page_ == Page::CodeEntry ? Page::Cheats : page_;
    for (std::size_t i = 0; i < t.size(); ++i) {
        f.tabs.push_back({t[i].id, tr(t[i].id), t[i].icon});
        if (t[i].page == root) f.tab = static_cast<int>(i);
    }
    f.rail_focus = rail_;
    f.hint = hint();
    f.message = message_;
    f.message_error = message_error_;
    anyui::Page& p = f.page;
    p.title = title();
    p.crumb = crumb();
    p.can_back = !at_tab_root() && page_ != Page::Capture;
    p.rows = rows();
    p.cursor = page_ == Page::Picker ? cursor_ : page_ == Page::CodeEntry ? static_cast<int>(p.rows.size()) - 1 : cursor_;
    if (page_ == Page::States) {
        for (int i = 0; i < kStateSlots; ++i) {
            const SlotInfo& s = slots_[static_cast<std::size_t>(i)];
            anyui::Slot slot{i + 1, s.used, s.used ? s.when : tr("states.empty"), {}};
            if (!s.thumb.empty()) slot.thumb = {s.thumb.data(), s.thumb_width, s.thumb_height};
            p.slots.push_back(std::move(slot));
        }
        p.columns = kSlotColumns;
        p.thumb_aspect = 1.5f;
        p.cursor = std::clamp(cursor_, 0, kStateSlots - 1);
        if (popup_) {
            p.popup_title = fmt("states.slot", {std::to_string(p.cursor + 1)});
            p.popup = slot_popup();
            p.popup_cursor = popup_cursor_;
        }
    }
    if (page_ == Page::Cheats && path_.empty()) {
        p.search = true;
        p.search_active = searching_;
        p.search_text = search_;
        p.search_placeholder = tr("cheats.search");
        p.empty_text = tr("cheats.none");
    }
    if (page_ == Page::Controls || page_ == Page::Capture) {
        p.diagram = anyui::Diagram::Gba;
        if (cursor_ >= 0 && cursor_ < static_cast<int>(p.rows.size()) && starts_with(p.rows[static_cast<std::size_t>(cursor_)].id, "bind.")) {
            const int i = id_number(p.rows[static_cast<std::size_t>(cursor_)].id);
            if (i < input::kGbaButtons) p.highlight = std::string(input::kGbaButtonNames[static_cast<std::size_t>(i)]);
        }
    }
    if (page_ == Page::Capture) {
        p.modal = fmt("controls.capture", {input::target_name(capture_target_)});
        p.modal_detail = tr("controls.capture.detail");
    }
    return f;
}

std::vector<std::string> Menu::lines() const {
    std::vector<std::string> out;
    const std::string c = crumb();
    out.push_back("== " + title() + (c.empty() || page_ == Page::Picker ? "" : " / " + c) + " ==");
    if (rail_) out.push_back("[tabs]");
    if (page_ == Page::States) {
        if (popup_) {
            out.push_back("-- " + fmt("states.slot", {std::to_string(cursor_ + 1)}) + " --");
            const auto actions = slot_popup();
            for (std::size_t i = 0; i < actions.size(); ++i) {
                out.push_back(std::string(static_cast<int>(i) == popup_cursor_ ? "> " : "  ") + actions[i].label);
            }
        } else {
            for (int i = 0; i < kStateSlots; ++i) {
                const SlotInfo& s = slots_[static_cast<std::size_t>(i)];
                out.push_back(std::string(i == cursor_ && !rail_ ? "> " : "  ") +
                              fmt("states.slot", {std::to_string(i + 1)}) + "  " +
                              (s.used ? s.when : tr("states.empty")));
            }
        }
    } else {
        if (page_ == Page::Cheats && path_.empty() && (searching_ || !search_.empty())) {
            out.push_back(std::string(searching_ ? "> " : "  ") + tr("cheats.search") + ": " + search_ + (searching_ ? "_" : ""));
        }
        const std::vector<Row> all = rows();
        const int selected = page_ == Page::CodeEntry || rail_ || searching_ ? -1 : cursor_;
        const int first = page_ == Page::Picker ? std::max(0, cursor_ - kVisibleRows / 2) : scroll_;
        for (int i = first; i < static_cast<int>(all.size()) && i < first + kVisibleRows; ++i) {
            const Row& r = all[static_cast<std::size_t>(i)];
            std::string text = r.label, value = r.value;
            switch (r.kind) {
            case RowKind::Choice:
            case RowKind::Slider: value = "< " + value + " >"; break;
            case RowKind::Check: text = std::string(r.on ? "[x] " : "[ ] ") + text; break;
            case RowKind::Binding: value = r.value + (r.value2.empty() ? "" : " | " + r.value2); break;
            case RowKind::Header: text = "-- " + text + " --"; break;
            case RowKind::Field: text = "> " + text + "_"; break;
            case RowKind::Link: value = value.empty() ? ">" : value; break;
            default: break;
            }
            out.push_back(std::string(i == selected ? "> " : "  ") + text + (value.empty() ? "" : "  " + value));
        }
    }
    if (page_ == Page::Capture) out.push_back("? " + fmt("controls.capture", {input::target_name(capture_target_)}));
    if (!message_.empty()) out.push_back("! " + message_);
    return out;
}

}  // namespace anygba::player
