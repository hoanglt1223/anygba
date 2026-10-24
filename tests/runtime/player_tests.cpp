#include "cheats/cheat_list.h"
#include "input_map.h"
#include "player/menu.h"
#include "player/mp2k.h"
#include "player/osd.h"
#include "player/script.h"
#include "player/speed.h"
#include "player/stats.h"
#include "support/temp_dir.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using anygba::input::Hotkey;
using anygba::player::Script;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

constexpr const char* kDatabase = R"(
[game]
title = "test"

[lists]
items = ["1 Potion", "2 Ether", "0x10 Elixir"]

[[cheat]]
name = "Memory/Write marker"
format = "raw"
code = ["0203F000:CAFEBABE"]

[[cheat]]
name = "Memory/Deep/Byte marker"
format = "cb"
code = "3203F010 0042"

[[cheat]]
name = "Items/Pick an item"
format = "cb"
code = ["8203F020 {v:4}"]
param = { label = "Item", list = "items" }

[[cheat]]
name = "Items/Quantity"
format = "cb"
code = ["8203F022 {v:4}"]
param = { label = "Count", min = 1, max = 99 }
default = 5
)";

}  // namespace

TEST_CASE("speed steps and the resampler keep sound continuous at every speed") {
    using anygba::player::next_speed;
    CHECK(next_speed(100, 1) == 200);
    CHECK(next_speed(400, 1) == 400);
    CHECK(next_speed(100, -1) == 100);
    CHECK(next_speed(300, -1) == 200);

    for (const double step : {1.0, 2.0, 3.0, 4.0}) {
        anygba::player::Resampler r;
        std::vector<std::int16_t> out;
        std::size_t in_frames = 0;
        // 60 blocks of uneven size (546/547 frames, as one video frame of sound) of a 440 Hz sine.
        for (int block = 0; block < 60; ++block) {
            const std::size_t n = block % 2 ? 547 : 546;
            std::vector<std::int16_t> in;
            for (std::size_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(in_frames + i) / 32768.0;
                const auto s = static_cast<std::int16_t>(std::lround(10000 * std::sin(2 * std::numbers::pi * 440 * t)));
                in.push_back(s);
                in.push_back(s);
            }
            in_frames += n;
            r.process(in, step, out);
        }
        CAPTURE(step);
        // Output length tracks input / step, with no drift across blocks.
        CHECK(std::abs(static_cast<double>(out.size() / 2) - static_cast<double>(in_frames) / step) <= 1.0);
        // No clicks: the largest jump between neighbours stays that of the sped-up sine.
        int largest = 0;
        for (std::size_t i = 2; i < out.size(); i += 2) {
            largest = std::max(largest, std::abs(out[i] - out[i - 2]));
        }
        const double expected = 10000 * 2 * std::numbers::pi * 440 * step / 32768.0;
        CHECK(largest <= expected * 1.05 + 2);
    }
}

TEST_CASE("input mapping: hotkeys, chords, gamepad hotkeys and player settings") {
    using anygba::input::parse_mapping;
    const auto m = parse_mapping(
        "[keyboard]\nSaveState1 = Ctrl+F5\nFastForward = Left Shift\nMenu =\n"
        "[gamepad]\nMenu = Start+Back\nL = LeftTrigger\n"
        "[player]\nSpeed = 300\nStickDeadzone = 9000\n",
        "t.ini");
    CHECK(m.hotkeys[static_cast<int>(Hotkey::SaveState1)] == std::vector<std::string>{"Ctrl+F5"});
    CHECK(m.hotkeys[static_cast<int>(Hotkey::LoadState1)] == std::vector<std::string>{"F1"});  // default
    CHECK(m.hotkeys[static_cast<int>(Hotkey::FastForward)] == std::vector<std::string>{"Left Shift"});
    CHECK(m.hotkeys[static_cast<int>(Hotkey::Menu)].empty());
    CHECK(m.pad_hotkeys[static_cast<int>(Hotkey::Menu)] == std::vector<std::string>{"Back+Start"});  // sorted
    CHECK(m.speed == 300);
    CHECK(m.stick_deadzone == 9000);

    CHECK_THROWS_WITH(parse_mapping("[player]\nSpeed = 250\n", "t.ini"), doctest::Contains("t.ini:2"));
    CHECK_THROWS_WITH(parse_mapping("[keyboard]\nSaveState1 = Hyper+F1\n", "t.ini"), doctest::Contains("Hyper"));
    CHECK_THROWS_WITH(parse_mapping("[gamepad]\nMenu = Back+Cross\n", "t.ini"), doctest::Contains("Cross"));

    const auto round_trip = parse_mapping(anygba::input::format_mapping(m), "round-trip");
    CHECK(round_trip.keys == m.keys);
    CHECK(round_trip.pad == m.pad);
    CHECK(round_trip.hotkeys == m.hotkeys);
    CHECK(round_trip.pad_hotkeys == m.pad_hotkeys);
    CHECK(round_trip.speed == m.speed);
}

TEST_CASE("hotkey chords need exactly their modifiers; gamepad chords hide their buttons from the game") {
    const auto m = anygba::input::default_mapping();
    Script s = Script::parse("1 key down F1\n2 key down Left Shift\n3 key up F1\n3 key up Left Shift\n"
                             "4 pad down Back\n4 pad down Start\n5 pad up Back\n6 stick 0 -20000\n",
                             "s");
    s.advance(1);
    auto held = anygba::input::hotkeys_held(m, s);
    CHECK(anygba::input::has(held, Hotkey::LoadState1));
    CHECK(!anygba::input::has(held, Hotkey::SaveState1));
    s.advance(2);
    held = anygba::input::hotkeys_held(m, s);
    CHECK(anygba::input::has(held, Hotkey::SaveState1));
    CHECK(!anygba::input::has(held, Hotkey::LoadState1));
    s.advance(3);
    s.advance(4);
    held = anygba::input::hotkeys_held(m, s);
    CHECK(anygba::input::has(held, Hotkey::Menu));
    CHECK((anygba::input::gba_keys(m, s) & 0x000C) == 0);  // Select and Start hidden while the chord is held
    s.advance(5);
    CHECK((anygba::input::gba_keys(m, s) & 0x0008) == 0x0008);  // Start alone reaches the game
    s.advance(6);
    CHECK((anygba::input::gba_keys(m, s) & 0x0040) == 0x0040);  // stick up past the deadzone = Up
}

TEST_CASE("scripts: key taps, typing and anygba-shot --keys ranges") {
    Script s = Script::parse("# comment\n10 key tap Shift+F1\n12 type ab1\n", "s");
    s.advance(10);
    CHECK(s.key_held("Left Shift"));
    CHECK(s.key_held("F1"));
    CHECK(s.take_key_presses() == std::vector<std::string>{"Left Shift", "F1"});
    s.advance(11);
    CHECK(!s.key_held("F1"));
    std::vector<std::string> typed;
    for (std::uint64_t f = 12; f < 20; ++f) {
        s.advance(f);
        for (auto& k : s.take_key_presses()) typed.push_back(k);
    }
    CHECK(typed == std::vector<std::string>{"A", "B", "1"});
    CHECK_THROWS_WITH(Script::parse("5 key tap NoSuchKey\n", "s"), doctest::Contains("s:1"));
    CHECK_THROWS(Script::parse("0 key tap A\n", "s"));

    Script k = Script::from_keys("a:3-4,start:5-5");
    k.advance(3);
    CHECK(k.gba_held() == 0x0001);
    k.advance(5);
    CHECK(k.gba_held() == 0x0008);
    k.advance(6);
    CHECK(k.gba_held() == 0);
}

TEST_CASE("rebinding swaps conflicting bindings") {
    using namespace anygba::input;
    Mapping m = default_mapping();
    // B = X by default; binding A to X gives B the old key of A (Z).
    const auto swapped = rebind(m, {false, 0}, Device::Keyboard, "X");
    REQUIRE(swapped);
    CHECK(*swapped == "B");
    CHECK(m.keys[0] == std::vector<std::string>{"X"});
    CHECK(m.keys[1] == std::vector<std::string>{"Z"});
    // A GBA button takes a key, not a chord; a hotkey keeps the chord.
    rebind(m, {false, 2}, Device::Keyboard, "Shift+Q");
    CHECK(m.keys[2] == std::vector<std::string>{"Q"});
    rebind(m, {true, static_cast<int>(Hotkey::Menu)}, Device::Gamepad, "Back+Guide");
    CHECK(m.pad_hotkeys[static_cast<int>(Hotkey::Menu)] == std::vector<std::string>{"Back+Guide"});
    rebind(m, {false, 1}, Device::Gamepad, "North");
    CHECK(m.pad[1] == std::vector<Button>{Button::North});
}

TEST_CASE("cheat database, value slots, and the per-game cheat file round trip") {
    anygba::test::TempDir dir("anygba-cheatlist-test");
    write_file(dir.path / "db.toml", kDatabase);
    anygba::cheats::CheatList list;
    REQUIRE(list.load_database(dir.path / "db.toml", 0x12345678));
    REQUIRE(list.cheats().size() == 4);
    CHECK(list.cheats()[2].param->choices.size() == 3);
    CHECK(list.cheats()[2].param->describe(16) == "Elixir");
    CHECK(list.cheats()[3].value == 5);
    CHECK(anygba::cheats::expand("8203F020 {v:4}", 0x1A) == "8203F020 001A");
    CHECK_THROWS(anygba::cheats::expand("8203F020 {v:2}", 0x100));

    CHECK(!list.set_enabled(0, true));
    CHECK(!list.set_value(2, 16));
    CHECK(list.set_value(2, 3));  // not a choice
    CHECK(!list.set_value(3, 42));
    list.add_custom("Mine", anygba::cheats::Format::Auto, {"3203F030 0077"});
    CHECK(list.cheats()[4].format == anygba::cheats::Format::CodeBreaker);
    CHECK(list.active().size() == 4);
    REQUIRE(!list.save_user(dir.path / "game.cheats.toml", "Test"));

    anygba::cheats::CheatList again;
    REQUIRE(again.load_database(dir.path / "db.toml", 0x12345678));
    again.load_user(dir.path / "game.cheats.toml");
    CHECK(again.warnings().empty());
    CHECK(again.cheats()[0].enabled);
    CHECK(!again.cheats()[1].enabled);
    CHECK(again.cheats()[2].enabled);
    CHECK(again.cheats()[2].value == 16);
    CHECK(again.cheats()[3].value == 42);
    REQUIRE(again.cheats().size() == 5);
    CHECK(again.cheats()[4].name == "Mine");
    CHECK(again.cheats()[4].enabled);

    // A database for other ROMs is skipped; broken entries name their line.
    write_file(dir.path / "other.toml", "[game]\ncrc32 = [\"DEADBEEF\"]\n");
    CHECK(!list.load_database(dir.path / "other.toml", 0x12345678));
    write_file(dir.path / "bad.toml", "[[cheat]]\nname = \"x\"\ncode = [\"DEADFACE 00001234\"]\n"
                                      "[[cheat]]\nname = \"y\"\ncode = [\"0203F000:12\"]\n");
    anygba::cheats::CheatList tolerant;
    CHECK(tolerant.load_database(dir.path / "bad.toml", 0));
    REQUIRE(tolerant.cheats().size() == 1);  // the broken entry is skipped, the good one kept
    CHECK(tolerant.cheats()[0].name == "y");
    REQUIRE(tolerant.warnings().size() == 1);
    CHECK(tolerant.warnings()[0].find("bad.toml:1") != std::string::npos);
    write_file(dir.path / "broken.toml", "[[cheat]\nname = \"x\"\n");
    CHECK_THROWS_WITH(list.load_database(dir.path / "broken.toml", 0), doctest::Contains("broken.toml:1"));
    write_file(dir.path / "slot.toml", "[[cheat]]\nname = \"x\"\ncode = [\"8203F020 {v:4}\"]\n");
    CHECK_THROWS_WITH(list.load_database(dir.path / "slot.toml", 0), doctest::Contains("no param"));
}

// Databases from tools/cheats/import-libretro.py, when ANYGBA_CHEAT_DB_DIR names their folder:
// every file must load, and the share of codes that decode is reported.
TEST_CASE("imported cheat databases load") {
    const char* dir = std::getenv("ANYGBA_CHEAT_DB_DIR");
    if (!dir) {
        MESSAGE("ANYGBA_CHEAT_DB_DIR not set; skipping");
        return;
    }
    std::size_t files = 0, loaded = 0, skipped = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".toml") continue;
        const std::uint32_t crc = static_cast<std::uint32_t>(std::stoul(entry.path().stem().string().substr(0, 8), nullptr, 16));
        anygba::cheats::CheatList list;
        CAPTURE(entry.path().string());
        REQUIRE(list.load_database(entry.path(), crc));
        ++files;
        loaded += list.cheats().size();
        skipped += list.warnings().size();
    }
    MESSAGE(files << " databases, " << loaded << " cheats decoded, " << skipped << " skipped");
    CHECK(files > 0);
}

TEST_CASE("menu: cheat tree, value picker, typed code and rebinding") {
    anygba::test::TempDir dir("anygba-menu-test");
    write_file(dir.path / "db.toml", kDatabase);
    anygba::cheats::CheatList list;
    REQUIRE(list.load_database(dir.path / "db.toml", 0));
    anygba::input::Mapping mapping = anygba::input::default_mapping();
    int speed = 100;
    int slot = 1;
    anygba::player::Menu menu(list, mapping, speed, slot);
    using anygba::player::MenuAction;
    using anygba::player::MenuInput;

    constexpr std::uint16_t A = 1, B = 2, Right = 16, Up = 64, Down = 128, R = 256, L = 512;
    std::vector<MenuAction> actions;
    const auto press = [&](std::uint16_t buttons) {
        MenuInput up;
        menu.update(up);
        MenuInput in;
        in.gba_held = buttons;
        auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    const auto keys = [&](std::vector<std::string> names) {
        MenuInput in;
        in.keys = std::move(names);
        auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    menu.open();
    menu.update({});  // the first frame only notes what is held
    CHECK(menu.lines()[0] == "== Game ==");
    press(Down);
    press(Right);  // speed 200
    CHECK(speed == 200);
    press(L);  // Settings
    press(L);  // Cheats
    REQUIRE(menu.lines()[0] == "== Cheats ==");
    CHECK(menu.lines()[1] == "> Memory  >");
    press(A);  // Memory
    CHECK(menu.lines()[0] == "== Cheats / Memory ==");
    press(A);  // Write marker on
    CHECK(list.cheats()[0].enabled);
    press(B);
    press(Down);
    press(A);  // Items
    press(A);  // Pick an item: picker
    CHECK(menu.lines()[0] == "== Pick an item: Item ==");
    press(Down);
    press(Down);
    press(A);
    CHECK(list.cheats()[2].enabled);
    CHECK(list.cheats()[2].value == 16);
    press(Down);
    press(Right);  // Quantity picker, from 5
    press(R);
    press(Up);
    press(A);
    CHECK(list.cheats()[3].value == 16);
    press(B);
    press(Down);
    press(A);  // My codes
    press(A);  // Add code...
    CHECK(menu.lines()[0] == "== Add a cheat code ==");
    keys({"3", "2", "0", "3", "F", "0", "3", "0", "0", "0", "7", "7", "Return"});
    keys({"Return"});
    REQUIRE(list.cheats().size() == 5);
    CHECK(list.cheats()[4].custom);
    CHECK(list.cheats()[4].code == std::vector<std::string>{"3203F030 0077"});
    CHECK(menu.lines().back() == "! Added Code 1 (CodeBreaker)");
    press(B);  // back to the tree's root
    press(L);  // Controls
    REQUIRE(menu.lines()[0] == "== Controls ==");
    CHECK(menu.lines()[1] == "  -- GBA buttons --");
    CHECK(menu.lines()[2] == "> A  Z | South");
    press(A);  // rebind A
    CHECK(menu.view().page.modal == "Press a key or button for A");
    keys({"C"});
    CHECK(mapping.keys[0] == std::vector<std::string>{"C"});
    press(Down);
    press(A);  // rebind B
    MenuInput pad;
    pad.pads = {anyport::platform::Button::North};
    pad.pad_held = 1u << static_cast<unsigned>(anyport::platform::Button::North);
    const auto pad_actions = menu.update(pad);
    actions.insert(actions.end(), pad_actions.begin(), pad_actions.end());
    CHECK(mapping.pad[1] == std::vector<anyport::platform::Button>{anyport::platform::Button::North});
    keys({"Escape"});
    CHECK(!menu.is_open());
    const auto count = [&](MenuAction a) { return std::count(actions.begin(), actions.end(), a); };
    CHECK(count(MenuAction::SpeedChanged) == 1);
    CHECK(count(MenuAction::CheatsChanged) == 4);
    CHECK(count(MenuAction::MappingChanged) == 2);
    CHECK(count(MenuAction::Close) == 1);
}

TEST_CASE("menu: tabs, rail, save state slots, audio, settings, search and clicks") {
    anygba::test::TempDir dir("anygba-menu-tabs");
    write_file(dir.path / "db.toml", kDatabase);
    anygba::cheats::CheatList list;
    REQUIRE(list.load_database(dir.path / "db.toml", 0));
    anygba::input::Mapping mapping = anygba::input::default_mapping();
    int speed = 100;
    int slot = 1;
    anygba::player::Menu menu(list, mapping, speed, slot);
    using anygba::player::MenuAction;
    using anygba::player::MenuInput;
    constexpr std::uint16_t A = 1, B = 2, Select = 4, Right = 16, Left = 32, Up = 64, Down = 128, R = 256, L = 512;
    std::vector<MenuAction> actions;
    const auto press = [&](std::uint16_t buttons) {
        menu.update({});
        MenuInput in;
        in.gba_held = buttons;
        auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    const auto keys = [&](std::vector<std::string> names) {
        MenuInput in;
        in.keys = std::move(names);
        auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    std::array<anygba::player::SlotInfo, anygba::player::kStateSlots> slots{};
    slots[1].used = true;
    slots[1].when = "2026-10-24 21:05";
    menu.set_slots(slots);

    menu.open();
    menu.update({});
    // The Game page: Reset and Exit want a second A.
    press(Down);
    press(Down);
    press(Down);
    press(A);
    CHECK(menu.lines()[4] == "> Reset  Press again to confirm");
    CHECK(actions.empty());
    press(A);
    CHECK(actions == std::vector<MenuAction>{MenuAction::Reset, MenuAction::Close});
    CHECK(!menu.is_open());
    actions.clear();

    // Save states: a 3x3 grid; A opens the slot's actions.
    menu.open();
    menu.update({});
    press(R);
    REQUIRE(menu.lines()[0] == "== Save states ==");
    CHECK(menu.lines()[1] == "> Slot 1  Empty");
    CHECK(menu.lines()[2] == "  Slot 2  2026-10-24 21:05");
    press(Right);
    CHECK(slot == 2);
    press(Down);
    press(Up);
    press(A);
    CHECK(menu.lines()[1] == "-- Slot 2 --");
    CHECK(menu.lines()[2] == "> Load");
    press(Down);
    press(Down);
    press(A);  // Delete
    CHECK(actions == std::vector<MenuAction>{MenuAction::DeleteState});
    press(Select);  // Select deletes too
    CHECK(actions.size() == 2);
    press(A);
    press(A);  // Load
    CHECK(actions.back() == MenuAction::Close);
    CHECK(actions[actions.size() - 2] == MenuAction::LoadState);
    CHECK(slot == 2);
    CHECK(!menu.is_open());
    actions.clear();

    // Left from a plain row goes to the rail; up and down there switch tabs.
    menu.open();
    menu.update({});
    press(Left);
    CHECK(menu.lines()[1] == "[tabs]");
    press(Down);
    press(Down);
    CHECK(menu.lines()[0] == "== Audio ==");
    press(A);
    CHECK(menu.lines()[1] == "> Volume  < 100% >");
    press(Left);
    CHECK(mapping.volume == 90);
    press(Down);
    press(A);
    CHECK(mapping.mute);

    // Settings: language, theme, menu size.
    press(L);
    press(L);
    press(L);
    REQUIRE(menu.lines()[0] == "== Settings ==");
    press(Right);
    CHECK(mapping.language == "vi");
    CHECK(menu.lines()[0] == "== C\xC3\xA0i \xC4\x91\xE1\xBA\xB7t ==");
    press(Left);
    CHECK(mapping.language == "en");
    press(Down);
    press(A);
    CHECK(mapping.light_theme);
    press(Down);
    press(Right);
    CHECK(mapping.ui_scale == 125);
    press(Down);
    press(A);
    CHECK(mapping.reduce_motion);
    press(Down);
    press(A);
    CHECK(mapping.stats);
    CHECK(std::count(actions.begin(), actions.end(), MenuAction::StatsChanged) == 1);
    const auto saved = anygba::input::parse_mapping(anygba::input::format_mapping(mapping), "rt");
    CHECK(saved.volume == 90);
    CHECK(saved.mute);
    CHECK(saved.light_theme);
    CHECK(saved.ui_scale == 125);
    CHECK(saved.reduce_motion);
    CHECK(saved.language == "en");
    CHECK_THROWS_WITH(anygba::input::parse_mapping("[player]\nVolume = 55\n", "t.ini"), doctest::Contains("t.ini:2"));
    CHECK_THROWS_WITH(anygba::input::parse_mapping("[player]\nLanguage = fr\n", "t.ini"), doctest::Contains("t.ini:2"));

    // Clicks: a tab, then a row.
    actions = menu.click({anyui::HitKind::Tab, 0});
    CHECK(menu.lines()[0] == "== Game ==");
    actions = menu.click({anyui::HitKind::Increase, 1});
    CHECK(speed == 200);
    actions = menu.click({anyui::HitKind::Row, 0});
    CHECK(actions == std::vector<MenuAction>{MenuAction::Close});
    CHECK(!menu.is_open());

    // Cheat search: Up from the first row starts typing.
    menu.open_cheats();
    menu.update({});
    press(Up);
    keys({"P", "I", "C"});
    CHECK(menu.lines()[1] == "> Search cheats: pic_");
    CHECK(menu.lines()[2] == "  [ ] Pick an item  Potion");
    keys({"Return"});
    CHECK(menu.lines()[0] == "== Cheats / 1 found ==");
    press(B);  // clears the search
    CHECK(menu.lines()[1] == "> Memory  >");
    press(B);
    CHECK(!menu.is_open());
}

TEST_CASE("menu strings: every key has English and Vietnamese text") {
    const anyui::Strings& s = anygba::player::menu_strings();
    CHECK(s.languages() == std::vector<std::string>{"en", "vi"});
    CHECK(s.missing("en").empty());
    CHECK(s.missing("vi").empty());
    anyui::Strings t("key\ten\tvi\nhello\tHello {0}\tXin ch\xC3\xA0o {0}\nonly\tOnly\n");
    t.set_language("vi");
    CHECK(t.fmt("hello", {"Henry"}) == "Xin ch\xC3\xA0o Henry");
    CHECK(t.tr("only") == "Only");
    CHECK(t.tr("Unknown label") == "Unknown label");
    CHECK_THROWS(anyui::Strings("nope\n"));
    CHECK_THROWS(anyui::Strings("key\ten\na\tb\tc\n"));
}

TEST_CASE("fast-forward speed: steps, settings file and the menu's Game and Audio pages") {
    using namespace anygba::player;
    CHECK(next_fast_forward(300, 1) == 400);
    CHECK(next_fast_forward(800, 1) == kUnlimited);
    CHECK(next_fast_forward(kUnlimited, 1) == kUnlimited);
    CHECK(next_fast_forward(150, -1) == 150);
    CHECK(parse_fast_forward("500") == 500);
    CHECK(parse_fast_forward("unlimited") == kUnlimited);
    CHECK(!parse_fast_forward("250"));
    CHECK(!parse_fast_forward("0"));
    CHECK(!parse_fast_forward("+300"));

    auto m = anygba::input::parse_mapping("[player]\nFastForwardSpeed = unlimited\n", "t.ini");
    CHECK(m.fast_forward == kUnlimited);
    CHECK(anygba::input::default_mapping().fast_forward == 300);
    m.fast_forward = 800;
    CHECK(anygba::input::parse_mapping(anygba::input::format_mapping(m), "rt").fast_forward == 800);
    CHECK_THROWS_WITH(anygba::input::parse_mapping("[player]\nFastForwardSpeed = 250\n", "t.ini"),
                      doctest::Contains("t.ini:2"));

    anygba::cheats::CheatList list;
    anygba::input::Mapping mapping = anygba::input::default_mapping();
    int speed = 100;
    int slot = 1;
    Menu menu(list, mapping, speed, slot);
    std::vector<MenuAction> actions;
    const auto press = [&](std::uint16_t buttons) {
        menu.update({});
        MenuInput in;
        in.gba_held = buttons;
        auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    constexpr std::uint16_t A = 1, B = 2, Right = 16, Left = 32, Down = 128, R = 256;
    menu.open();
    menu.update({});
    press(Down);
    press(Right);
    CHECK(speed == 200);
    CHECK(menu.lines()[0] == "== Game ==");
    CHECK(menu.lines()[2] == "> Speed  < 200% >");
    CHECK(menu.lines()[3] == "  Fast-forward speed  < 300% >");
    press(Left);
    CHECK(speed == 100);
    press(Down);
    press(Right);
    press(Right);
    CHECK(mapping.fast_forward == 500);
    press(Left);
    CHECK(mapping.fast_forward == 400);
    press(R);
    press(R);
    REQUIRE(menu.lines()[0] == "== Audio ==");
    press(Down);
    press(Down);
    CHECK(menu.lines()[3] == "> Normal-speed music  Off");
    press(A);
    CHECK(mapping.normal_speed_music);
    CHECK(anygba::input::parse_mapping(anygba::input::format_mapping(mapping), "rt").normal_speed_music);
    press(B);
    CHECK(!menu.is_open());
    CHECK(std::count(actions.begin(), actions.end(), MenuAction::MappingChanged) == 4);
    CHECK(std::count(actions.begin(), actions.end(), MenuAction::SpeedChanged) == 2);
}

TEST_CASE("time-stretch keeps the pitch and shortens the sound by the ratio") {
    // 440 Hz tone bursts: 0.3 s on at the start of every 0.9 s, in video-frame blocks.
    for (const double ratio : {1.0, 1.5, 3.0, 8.0}) {
        anygba::player::TimeStretch stretch;
        std::vector<std::int16_t> out;
        const std::size_t total = 32768 * 12;
        for (std::size_t at = 0; at < total;) {
            const std::size_t n = std::min<std::size_t>(547, total - at);
            std::vector<std::int16_t> in;
            for (std::size_t i = at; i < at + n; ++i) {
                const double t = static_cast<double>(i) / 32768.0;
                const bool on = std::fmod(t, 0.9) < 0.3;
                const auto s = static_cast<std::int16_t>(on ? std::lround(8000 * std::sin(2 * std::numbers::pi * 440 * t)) : 0);
                in.push_back(s);
                in.push_back(s);
            }
            at += n;
            stretch.process(in, ratio, out);
        }
        CAPTURE(ratio);
        const double frames = static_cast<double>(out.size() / 2);
        CHECK(std::abs(frames - static_cast<double>(total) / ratio) <= 2048 + total / ratio * 0.01);
        // Burst starts, from where the level rises: one every 0.9 / ratio s.
        std::vector<double> starts;
        bool loud = false;
        for (std::size_t f = 0; f + 64 < out.size() / 2; f += 64) {
            double e = 0;
            for (std::size_t k = 0; k < 64; ++k) e += std::abs(out[(f + k) * 2]);
            const bool now = e / 64 > 2000;
            if (now && !loud) starts.push_back(static_cast<double>(f) / 32768.0);
            loud = now;
        }
        REQUIRE(starts.size() >= 10);
        const double period = (starts[starts.size() - 2] - starts[1]) / static_cast<double>(starts.size() - 3);
        CHECK(period == doctest::Approx(0.9 / ratio).epsilon(0.03));
        // Pitch: zero crossings inside the first full burst stay at 880 a second.
        const auto first = static_cast<std::size_t>(starts[1] * 32768) + 256;
        const double burst = 0.3 / ratio * 32768 - 512;
        if (burst > 500) {
            const auto span = static_cast<std::size_t>(burst);
            int crossings = 0;
            for (std::size_t f = first + 1; f < first + span; ++f) {
                crossings += (out[f * 2] >= 0) != (out[(f - 1) * 2] >= 0);
            }
            CHECK(crossings / (static_cast<double>(span) / 32768.0) == doctest::Approx(880).epsilon(0.03));
        }
    }
}

TEST_CASE("MP2K: tempo and envelopes slowed by the speed, then put back") {
    anygba::Memory mem;
    const auto put32 = [&](std::uint32_t addr, std::uint32_t v) {
        auto& ram = (addr >> 24) == 3 ? mem.iwram : mem.ewram;
        const std::uint32_t off = addr & static_cast<std::uint32_t>(ram.size() - 1);
        for (int i = 0; i < 4; ++i) ram[off + static_cast<std::uint32_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    const auto get16 = [&](std::uint32_t addr) {
        const std::uint32_t off = addr & 0x7FFF;
        return static_cast<int>(mem.iwram[off] | mem.iwram[off + 1] << 8);
    };
    const auto byte = [&](std::uint32_t addr) -> std::uint8_t& { return mem.iwram[addr & 0x7FFF]; };
    constexpr std::uint32_t info = 0x03006380, player1 = 0x03007420, player2 = 0x02010000;
    constexpr std::uint32_t ident = anygba::player::Mp2kTempo::kIdent;
    anygba::player::Mp2kTempo tempo;
    CHECK(!tempo.update(mem, 300));  // no engine

    put32(0x03007FF0, info);
    put32(info, ident);
    put32(info + 0x24, player1);
    put32(player1 + 0x1C, 184 | 256u << 16);  // tempoD 184, tempoU 0x100
    put32(player1 + 0x20, 184);               // tempoI
    put32(player1 + 0x34, ident);
    put32(player1 + 0x3C, player2);
    put32(player2 + 0x34, ident);            // second player, tempo 0
    const std::uint32_t chan = info + 0x50 + 2 * 0x40;  // third DirectSound channel
    byte(chan) = 0x83;                                  // playing
    byte(chan + 4) = 255;                               // attack
    byte(chan + 5) = 192;                               // decay
    byte(chan + 7) = 128;                               // release

    REQUIRE(tempo.update(mem, 300));
    CHECK(tempo.scaled());
    int sum = get16(player1 + 0x20);
    for (int frame = 1; frame < 3; ++frame) {
        tempo.update(mem, 300);
        sum += get16(player1 + 0x20);
    }
    CHECK(sum == 184);  // 61, 61, 62: exact over three frames
    CHECK(byte(chan + 4) == 85);
    CHECK(byte(chan + 5) == 233);  // 0.75^(1/3) * 256
    CHECK(byte(chan + 7) == 203);  // 0.5^(1/3) * 256

    put32(info, ident + 1);  // locked: left alone
    put32(player1 + 0x20, 7);
    tempo.update(mem, 100);
    CHECK(get16(player1 + 0x20) == 7);
    put32(info, ident);
    tempo.update(mem, 100);
    CHECK(!tempo.scaled());
    CHECK(get16(player1 + 0x20) == 184);
    CHECK(byte(chan + 4) == 255);
    CHECK(byte(chan + 5) == 192);
    CHECK(byte(chan + 7) == 128);
}

TEST_CASE("save folder: settings file and the Settings page") {
    const std::string folder = "C:\\Users\\me\\OneDrive\\Documents\\My Games\\AnyGBA\\saves";
    auto m = anygba::input::parse_mapping("[player]\nSaveFolder = " + folder + "\r\n", "t.ini");
    CHECK(m.save_folder == folder);
    CHECK(anygba::input::parse_mapping(anygba::input::format_mapping(m), "rt").save_folder == folder);
    CHECK(anygba::input::parse_mapping("[player]\nSaveFolder =\n", "t.ini").save_folder.empty());

    anygba::cheats::CheatList list;
    int speed = 100;
    int slot = 1;
    anygba::player::Menu menu(list, m, speed, slot);
    using anygba::player::MenuAction;
    std::vector<MenuAction> actions;
    const auto press = [&](std::uint16_t buttons) {
        menu.update({});
        anygba::player::MenuInput in;
        in.gba_held = buttons;
        auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    constexpr std::uint16_t A = 1, Down = 128, L = 512;
    menu.open();
    menu.update({});
    press(L);  // the last tab: Settings
    REQUIRE(menu.lines()[0] == "== Settings ==");
    for (int i = 0; i < 5; ++i) press(Down);
    CHECK(menu.lines()[6] == "> Save folder  " + folder);
    press(A);
    CHECK(actions == std::vector<MenuAction>{MenuAction::ChooseSaveFolder});
    press(Down);
    press(A);
    CHECK(m.save_folder.empty());
    CHECK(actions.back() == MenuAction::MappingChanged);
    CHECK(menu.lines()[6] == "  Save folder  default");
    press(Down);
    press(A);
    CHECK(actions.back() == MenuAction::ExportSave);
    press(Down);
    press(A);
    CHECK(actions.back() == MenuAction::ImportSave);
}

TEST_CASE("stats overlay: hotkey, setting and lines") {
    const auto defaults = anygba::input::default_mapping();
    CHECK(defaults.hotkeys[static_cast<int>(Hotkey::Stats)] == std::vector<std::string>{"Ctrl+F"});
    CHECK(!defaults.stats);
    auto m = anygba::input::parse_mapping("[player]\nStats = on\n", "t.ini");
    CHECK(m.stats);
    CHECK(anygba::input::parse_mapping(anygba::input::format_mapping(m), "rt").stats);
    CHECK_THROWS_WITH(anygba::input::parse_mapping("[player]\nStats = yes\n", "t.ini"), doctest::Contains("t.ini:2"));

    Script s = Script::parse("1 key down Left Ctrl\n1 key down F\n", "s");
    s.advance(1);
    CHECK(anygba::input::has(anygba::input::hotkeys_held(defaults, s), Hotkey::Stats));

    anygba::player::Stats st;
    st.host_fps = 60;
    st.gba_fps = 16777216.0 / 280896.0 * 3;
    st.target_speed = 300;
    st.fast_forward = true;
    st.frame_ms = 1.25;
    st.audio_fill = 50;
    st.underruns = 2;
    st.interpreted = 0.5;
    st.filter = "xbrz4x+lcd-grid";
    st.output = "1280x720";
    st.cheats = 3;
    st.save = "flash128";
    st.rtc = true;
    const auto lines = anygba::player::stats_lines(st);
    CHECK(lines == std::vector<std::string>{"FPS 60.0 host, 179.2 GBA", "Speed 300% of 300% (FF)", "Frame 1.25 ms",
                                            "Audio 50%, 2 underruns", "Interpreter 0.5%", "Video xbrz4x+lcd-grid 1280x720",
                                            "Cheats 3  Save flash128  RTC on"});
    for (const auto& l : lines) CHECK(l.size() < static_cast<std::size_t>(anygba::player::kColumns));
    std::array<std::uint16_t, 240 * 160> picture{};
    anygba::player::draw_stats(picture, lines);
    CHECK(std::count(picture.begin(), picture.end(), anygba::player::color::kGreen) > 0);
    CHECK(picture[240 * 159 + 239] == 0);  // only the top-left corner
}

TEST_CASE("OSD text folds accents and draws inside the picture") {
    CHECK(anygba::player::to_ascii("Pok\xC3\xA9mon \xE2\x98\x85") == "Pokemon ?");
    std::array<std::uint16_t, 240 * 160> picture{};
    anygba::player::draw_text(picture, 235, 155, "AB", 0x7FFF);  // clipped, must not write outside
    anygba::player::draw_text(picture, 0, 0, "A", 0x7FFF);
    CHECK(picture[2] == 0x7FFF);  // the top of the 'A' at x = 1..3
    CHECK(picture[0] == 0);
}
