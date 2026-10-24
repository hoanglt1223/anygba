#include "cheats/cheat_list.h"
#include "input_map.h"
#include "player/menu.h"
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

    constexpr std::uint16_t A = 1, B = 2, Right = 16, Left = 32, Up = 64, Down = 128, R = 256;
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
    press(Down);
    press(Down);
    press(Down);
    press(Right);  // speed 200
    CHECK(speed == 200);
    press(Down);
    press(A);  // Cheats
    CHECK(menu.lines()[0] == "== Cheats ==");
    press(A);  // Memory
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
    press(B);
    press(B);  // main page, cursor back on Cheats
    press(Down);
    press(A);  // Controls
    press(A);  // rebind A
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
    (void)Left;
}

TEST_CASE("fast-forward speed: steps, settings file and the menu's Speed page") {
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
    constexpr std::uint16_t A = 1, B = 2, Right = 16, Left = 32, Down = 128;
    menu.open();
    menu.update({});
    for (int i = 0; i < 3; ++i) press(Down);
    press(Right);  // Left/Right on the main Speed row still change the speed
    CHECK(speed == 200);
    press(A);
    CHECK(menu.lines()[0] == "== Speed ==");
    CHECK(menu.lines()[2] == "  Fast-forward speed  < 300% >");
    press(Left);
    CHECK(speed == 100);
    press(Down);
    press(Right);
    press(Right);
    CHECK(mapping.fast_forward == 500);
    press(Left);
    CHECK(mapping.fast_forward == 400);
    press(B);
    CHECK(menu.lines()[0] == "== AnyGBA ==");
    CHECK(std::count(actions.begin(), actions.end(), MenuAction::MappingChanged) == 3);
    CHECK(std::count(actions.begin(), actions.end(), MenuAction::SpeedChanged) == 2);
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
