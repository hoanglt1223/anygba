#include "cheats/cheat_list.h"
#include "control/controller.h"
#include "control/json.h"
#include "control/net.h"
#include "control/png.h"
#include "control/server.h"
#include "control/symbols.h"
#include "journal/journal.h"
#include "player/menu.h"
#include "support/png.h"
#include "support/temp_dir.h"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using anygba::control::Json;
using anygba::control::Symbols;

namespace {

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Guest memory for symbol tests: 64 KB at 0x02000000.
struct FakeMemory {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x10000);
    anygba::control::Peek peek() {
        return [this](std::uint32_t addr, std::span<std::uint8_t> out) {
            for (std::size_t i = 0; i < out.size(); ++i) {
                const std::uint32_t a = addr + static_cast<std::uint32_t>(i);
                if (a < 0x02000000 || a >= 0x02010000) throw std::runtime_error("unreadable");
                out[i] = bytes[a - 0x02000000];
            }
        };
    }
    void put(std::uint32_t addr, std::initializer_list<std::uint8_t> data) {
        std::size_t i = addr - 0x02000000;
        for (const auto b : data) bytes[i++] = b;
    }
};

}  // namespace

TEST_CASE("JSON: parse, build and serialize") {
    const Json j = Json::parse(R"({"a": [1, -2, 3.5, true, null], "s": "x\"\\\n\u00e9\ud83d\ude00", "big": 4294967295})");
    CHECK(j.find("a")->as_array()[1].as_int() == -2);
    CHECK(j.find("a")->as_array()[2].as_double() == 3.5);
    CHECK(j.find("a")->as_array()[4].is_null());
    CHECK(j.find("s")->as_string() == "x\"\\\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(j.find("big")->as_int() == 4294967295ll);
    CHECK(Json::parse(j.dump()) == j);
    Json o;
    o["n"] = 1u;
    o["list"].push_back("v");
    CHECK(o.dump() == R"({"n":1,"list":["v"]})");
    for (const char* bad : {"{", "[1,]", "\"\\ud800\"", "01x", "{\"a\" 1}", "[1] 2", "\"\x01\""}) {
        CHECK_THROWS_AS(Json::parse(bad), anygba::control::JsonError);
    }
    std::string deep(100, '[');
    CHECK_THROWS_AS(Json::parse(deep), anygba::control::JsonError);
    CHECK_THROWS_AS(Json("x").as_int(), anygba::control::JsonError);
    const std::uint8_t data[] = {'M', 'a', 'n', 'y'};
    CHECK(anygba::control::base64_encode(data, 4) == "TWFueQ==");
    CHECK(anygba::control::base64_encode(data, 3) == "TWFu");
    CHECK(anygba::control::hex_encode(data, 2) == "4d61");
    CHECK(anygba::control::hex_decode("4d 61") == std::vector<std::uint8_t>{0x4d, 0x61});
    CHECK_THROWS(anygba::control::hex_decode("4d6"));
}

TEST_CASE("PNG screenshots decode to the same picture") {
    std::vector<std::uint16_t> picture(240 * 160);
    for (std::size_t i = 0; i < picture.size(); ++i) picture[i] = static_cast<std::uint16_t>((i * 7919) & 0x7FFF);
    anygba::test::TempDir dir("anygba-png-test");
    const auto png = anygba::control::encode_png(picture, 240, 160);
    std::ofstream(dir.path / "a.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    CHECK(anygba::test::read_png_rgb555(dir.path / "a.png", 240, 160) == picture);
    const auto half = anygba::control::encode_png(picture, 240, 160, 0.5);
    std::ofstream(dir.path / "b.png", std::ios::binary).write(reinterpret_cast<const char*>(half.data()), static_cast<std::streamsize>(half.size()));
    const auto small = anygba::test::read_png_rgb555(dir.path / "b.png", 120, 80);
    CHECK(small[1] == picture[2]);
    CHECK(small[120] == picture[480]);
    CHECK_THROWS(anygba::control::encode_png(picture, 240, 160, 16));
}

TEST_CASE("buttons by name") {
    CHECK(anygba::control::parse_buttons(Json("A+Up")) == 0x41);
    CHECK(anygba::control::parse_buttons(Json::parse(R"(["start", "L"])")) == 0x208);
    CHECK(anygba::control::button_names(0x41) == "A+Up");
    CHECK_THROWS(anygba::control::parse_buttons(Json("Jump")));
}

TEST_CASE("symbols: scalars, pointers, XOR, text, struct arrays") {
    const Symbols s = Symbols::parse(R"(
[game]
title = "ignored"
[symbols.key]
address = 0x02000000
type = "u32"
[symbols.money]
pointer = 0x02000004
address = 0x10
type = "u32"
xor = "key"
journal = true
[symbols.coins]
pointer = 0x02000004
address = 0x14
type = "u16"
xor = "key"
[symbols.badge2]
address = 0x02000020
type = "u8"
bit = 1
[symbols.name]
address = 0x02000030
type = "text"
size = 8
charset = "pokemon"
[symbols.party]
address = 0x02000100
type = "struct"
size = 8
count = 3
journal = true
fields.hp = { offset = 0, type = "u16" }
fields.temp = { offset = 2, type = "s8" }
fields.moves = { offset = 4, type = "u8", count = 4 }
)", "test.toml");
    FakeMemory mem;
    mem.put(0x02000000, {0x0F, 0x0F, 0x00, 0x00});              // key 0x0F0F
    mem.put(0x02000004, {0x00, 0x02, 0x00, 0x02});              // pointer 0x02000200
    mem.put(0x02000210, {0x0F ^ 0x40, 0x0F ^ 0x42, 0x0F, 0x00});  // money 0x0F004240 ^ key
    mem.put(0x02000214, {0x0F ^ 0x10, 0x0F ^ 0x27});            // coins 0x2710
    mem.put(0x02000020, {0x02});
    mem.put(0x02000030, {0xC2, 0xD9, 0xE0, 0xE0, 0xFF});
    mem.put(0x02000108, {0x2C, 0x01, 0xFE, 0, 1, 2, 3, 4});     // party[1]
    const auto peek = mem.peek();
    CHECK(s.read("money", peek).as_int() == 1000000);
    CHECK(s.read("coins", peek).as_int() == 0x2710);
    CHECK(s.read("badge2", peek).as_bool());
    CHECK(s.read("name", peek).as_string() == "Hell");
    CHECK(s.read("party[1].hp", peek).as_int() == 300);
    CHECK(s.read("party[1].temp", peek).as_int() == -2);
    CHECK(s.read("party[1].moves[3]", peek).as_int() == 4);
    CHECK(s.read("party[1]", peek).dump() == R"({"hp":300,"temp":-2,"moves":[1,2,3,4]})");
    CHECK(s.read("party", peek).size() == 3);
    CHECK_THROWS(s.read("party[3]", peek));
    CHECK_THROWS(s.read("party.hp", peek));
    CHECK_THROWS(s.read("nobody", peek));
    CHECK_THROWS(s.read("party[1].mana", peek));
    const Json snap = s.snapshot(peek);
    CHECK(snap.size() == 2);
    CHECK(snap.find("party")->size() == 3);
    CHECK(s.describe(peek).size() == 6);

    mem.put(0x02000004, {0, 0, 0, 0});
    CHECK_THROWS_WITH(s.read("money", peek), doctest::Contains("null"));

    for (const char* bad : {"[symbols.a]\naddress = 1\ntype = \"u64\"\n", "[symbols.a]\naddress = 1\ntype = \"u8\"\nsize = 2\n",
                            "[symbols.a]\naddress = 1\ntype = \"u8\"\ncolor = 1\n", "[symbols.a]\ntype = \"u8\"\n",
                            "[symbols.a]\naddress = 1\ntype = \"u8\"\nxor = \"b\"\n",
                            "[symbols.a]\naddress = 1\ntype = \"struct\"\nsize = 2\nfields.x = { offset = 1, type = \"u16\" }\n",
                            "[symbols.\"a.b\"]\naddress = 1\ntype = \"u8\"\n"}) {
        CHECK_THROWS(Symbols::parse(bad, "bad.toml"));
    }
}

TEST_CASE("journal: records, summary, search, retention, export, clear") {
    anygba::test::TempDir dir("anygba-journal-test");
    const std::array<std::uint16_t, 240 * 160> picture{};
    {
        anygba::journal::Journal j(dir.path / "journal", "game-a");
        j.settings().snapshot_seconds = 1;
        j.settings().thumbnail_seconds = 2;
        j.start(100, Json(Json::Object{{"title", "Game A"}}));
        REQUIRE(j.active());
        j.record(101, "command", "agent", Json(Json::Object{{"method", "press"}, {"params", Json(Json::Object{{"buttons", "A"}})}}));
        j.record(102, "note", "agent", Json(Json::Object{{"text", "Find the key"}}));
        j.record(103, "goal", "agent", Json(Json::Object{{"text", "Leave town"}}));
        j.record(104, "command", "agent", Json(Json::Object{{"method", "write_memory"}, {"error", "nope"}}));
        for (std::uint64_t f = 105; f < 300; ++f) j.on_frame(f, [] { return Json(Json::Object{{"money", 5}}); }, picture);
        j.stop(300);
        CHECK_FALSE(j.active());
    }
    const auto sessions = anygba::journal::list_sessions(dir.path / "journal");
    REQUIRE(sessions.size() == 1);
    const Json sum = anygba::journal::summarize(sessions[0].dir);
    CHECK(sum.find("ended_cleanly")->as_bool());
    CHECK(sum.find("commands")->find("press")->as_int() == 1);
    CHECK(sum.find("notes")->as_array()[0].find("text")->as_string() == "Find the key");
    CHECK(sum.find("goals")->size() == 1);
    CHECK(sum.find("errors")->size() == 1);
    CHECK(sum.find("last_snapshot")->find("money")->as_int() == 5);
    CHECK(sum.find("kinds")->find("snapshot")->as_int() >= 3);
    CHECK(sum.find("kinds")->find("thumbnail")->as_int() >= 2);
    CHECK(fs::exists(sessions[0].dir / "thumbs" / "f105.png"));

    anygba::journal::Query q;
    q.text = "find THE";
    CHECK(anygba::journal::search(dir.path / "journal", q).size() >= 1);
    q = {};
    q.method = "press";
    const Json hits = anygba::journal::search(dir.path / "journal", q);
    REQUIRE(hits.size() == 1);
    CHECK(hits.as_array()[0].find("game")->as_string() == "game-a");
    q = {};
    q.kind = "snapshot";
    q.limit = 2;
    CHECK(anygba::journal::search(dir.path / "journal", q).size() == 2);

    const fs::path copy = anygba::journal::export_session(sessions[0].dir, dir.path / "export");
    CHECK(fs::exists(copy / "session.jsonl"));

    // A crash leaves a cut-off last line; reading skips it.
    std::ofstream(sessions[0].dir / "session.jsonl", std::ios::app) << "{\"kind\":\"note\",\"te";
    CHECK(anygba::journal::summarize(sessions[0].dir).find("records")->as_int() > 0);

    // Retention: sessions past the age limit go, except the kept (active) one; then the oldest
    // go until the total fits the size cap.
    const fs::path old = dir.path / "journal" / "game-b" / "old";
    const auto make_old = [&](std::size_t bytes, std::chrono::hours age) {
        fs::create_directories(old);
        {
            std::ofstream out(old / "session.jsonl", std::ios::binary);
            out << std::string(bytes, ' ') << "\n";
        }
        fs::last_write_time(old / "session.jsonl", fs::file_time_type::clock::now() - age);
    };
    make_old(10, std::chrono::hours(24 * 10));
    CHECK(anygba::journal::apply_retention(dir.path / "journal", 0, 5, old) == 0);
    CHECK(anygba::journal::apply_retention(dir.path / "journal", 0, 5) == 1);
    CHECK_FALSE(fs::exists(old));
    make_old(2 << 20, std::chrono::hours(1));
    CHECK(anygba::journal::apply_retention(dir.path / "journal", 1, 0) == 1);  // the big, older one
    CHECK_FALSE(fs::exists(old));
    CHECK(anygba::journal::clear(dir.path / "journal", "game-a") == 1);
    CHECK(anygba::journal::list_sessions(dir.path / "journal").empty());
}

TEST_CASE("journal: off and on again within a second starts a second session") {
    anygba::test::TempDir dir("anygba-journal-restart-test");
    anygba::journal::Journal j(dir.path, "g");
    j.start(1, Json::object());
    const std::string first = j.session_id();
    j.stop(2);
    j.start(3, Json::object());
    CHECK(j.session_id() != first);
    j.stop(4);
    CHECK(anygba::journal::list_sessions(dir.path).size() == 2);
}

TEST_CASE("control server: loopback only, token handshake, one request at a time") {
    anygba::test::TempDir dir("anygba-server-test");
    namespace net = anygba::control::net;
    anygba::control::Server server;
    CHECK_THROWS(server.start("0.0.0.0", 0, dir.path / "t.json", Json::object()));
    CHECK_THROWS(server.start("192.168.1.10", 0, dir.path / "t.json", Json::object()));
    server.start("127.0.0.1", 0, dir.path / "t.json", Json(Json::Object{{"title", "T"}}));
    const Json file = Json::parse(read_text(dir.path / "t.json"));
    CHECK(file.find("port")->as_int() == server.port());
    CHECK(file.find("token")->as_string() == server.token());
    CHECK(file.find("title")->as_string() == "T");

    const auto pump = [&](net::Socket& s, std::string& buf, std::vector<anygba::control::Request>& got) {
        std::string line;
        for (int i = 0; i < 200; ++i) {
            for (auto& r : server.poll(5)) got.push_back(std::move(r));
            if (net::read_line(s, buf, line, 5)) return Json::parse(line);
            if (!got.empty()) return Json();
        }
        FAIL("no answer");
        return Json();
    };

    {
        net::Socket bad = net::connect_loopback(server.port());
        net::send_all(bad, R"({"jsonrpc":"2.0","id":1,"method":"hello","params":{"token":"x"}})" "\n");
        std::string buf;
        std::vector<anygba::control::Request> got;
        const Json reply = pump(bad, buf, got);
        CHECK(reply.find("error")->find("code")->as_int() == anygba::control::kUnauthorized);
        CHECK(got.empty());
    }
    net::Socket c = net::connect_loopback(server.port());
    net::send_all(c, R"({"jsonrpc":"2.0","id":1,"method":"hello","params":{"token":")" + server.token() +
                         R"(","client":"t"}})" "\n" R"({"jsonrpc":"2.0","id":2,"method":"get_state"})" "\n");
    std::string buf;
    std::vector<anygba::control::Request> got;
    pump(c, buf, got);
    REQUIRE(got.size() == 1);  // get_state waits until hello is answered
    CHECK(got[0].method == "hello");
    CHECK(server.clients() == 1);
    CHECK(server.take_events() == std::vector<std::string>{"connected t"});
    server.reply(got[0], Json(Json::Object{{"ok", true}}));
    got.clear();
    std::string line;
    REQUIRE(net::read_line(c, buf, line, 2000));
    CHECK(Json::parse(line).find("result")->find("ok")->as_bool());
    pump(c, buf, got);
    REQUIRE(got.size() == 1);
    CHECK(got[0].method == "get_state");
    server.reply_error(got[0], anygba::control::kFailed, "no");
    REQUIRE(net::read_line(c, buf, line, 2000));
    CHECK(Json::parse(line).find("error")->find("message")->as_string() == "no");

    server.stop();
    CHECK_FALSE(fs::exists(dir.path / "t.json"));
    CHECK_THROWS_AS(net::read_line(c, buf, line, 2000), net::NetError);
}

TEST_CASE("menu: the AI page switches AI control, read-only and the journal") {
    anygba::cheats::CheatList list;
    anygba::input::Mapping mapping = anygba::input::default_mapping();
    int speed = 100, slot = 1;
    anygba::player::Menu menu(list, mapping, speed, slot);
    anygba::player::MenuSettings settings;
    menu.set_settings(&settings);
    using anygba::player::MenuAction;
    std::vector<MenuAction> actions;
    const auto press = [&](std::uint16_t buttons) {
        menu.update({});
        anygba::player::MenuInput in;
        in.gba_held = buttons;
        const auto a = menu.update(in);
        actions.insert(actions.end(), a.begin(), a.end());
    };
    menu.open();
    menu.update({});
    press(512);  // L: Settings
    press(512);  // L: AI
    CHECK(menu.lines()[0] == "== AI ==");
    CHECK(menu.lines()[1] == "> AI control  Off");
    press(1);
    CHECK(settings.ai_control);
    press(128);
    press(128);
    press(16);  // Right also switches
    CHECK(settings.journal);
    CHECK(std::count(actions.begin(), actions.end(), MenuAction::SettingsChanged) == 2);
    press(2);
    CHECK(!menu.is_open());
}

TEST_CASE("shipped symbol files parse") {
    const fs::path dir = fs::path(ANYGBA_GOLDEN_DIR).parent_path().parent_path() / "compat" / "symbols";
    int files = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".toml") continue;
        CAPTURE(e.path().string());
        CHECK_FALSE(Symbols::load(e.path()).empty());
        ++files;
    }
    CHECK(files >= 1);
}
