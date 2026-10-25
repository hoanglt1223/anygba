#include "codegen/c_emitter.h"
#include "config/game_config.h"
#include "pipeline.h"
#include "support/temp_dir.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace anygba::config;
using anygba::analysis::Mode;

namespace {

std::string error_of(const char* text) {
    try {
        parse_game_config(text, "game.toml");
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "";
}

std::string read(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

// ARM: 0x08000000 b 0x08000000 ; 0x08000040 bx lr ; 0x08000080 bx lr (Thumb, at 0x08000080: bx lr = 0x4770)
std::vector<std::uint8_t> small_rom() {
    std::vector<std::uint8_t> rom(0x100, 0);
    const std::uint8_t loop[] = {0xFE, 0xFF, 0xFF, 0xEA};
    const std::uint8_t bx_lr[] = {0x1E, 0xFF, 0x2F, 0xE1};
    const std::uint8_t thumb_bx_lr[] = {0x70, 0x47};
    std::copy(std::begin(loop), std::end(loop), rom.begin());
    std::copy(std::begin(bx_lr), std::end(bx_lr), rom.begin() + 0x40);
    std::copy(std::begin(thumb_bx_lr), std::end(thumb_bx_lr), rom.begin() + 0x80);
    return rom;
}

}  // namespace

TEST_CASE("game config: [[override]] by address or by [functions] symbol") {
    const GameConfig c = parse_game_config(R"(
[functions]
StringCopy = { addr = 0x08008BA0, mode = "thumb" }

[[override]]
symbol = "StringCopy"
impl = "gen3_StringCopy"
engine = "pokemon-gen3"

[[override]]
addr = 0x08000400
mode = "arm"
impl = "skip_logo"
enabled = false
verified = true
)",
                                           "game.toml");
    REQUIRE(c.functions.size() == 1);
    CHECK(c.functions.at("StringCopy").addr == 0x08008BA0);
    CHECK(c.functions.at("StringCopy").mode == Mode::Thumb);
    REQUIRE(c.overrides.size() == 2);
    CHECK(c.overrides[0].symbol == "StringCopy");
    CHECK_FALSE(c.overrides[0].addr);
    CHECK(c.overrides[0].impl == "gen3_StringCopy");
    CHECK(c.overrides[0].engine == "pokemon-gen3");
    CHECK(c.overrides[0].enabled);
    CHECK(c.overrides[0].where == "game.toml:5");
    CHECK(*c.overrides[1].addr == 0x08000400);
    CHECK(c.overrides[1].mode == Mode::Arm);
    CHECK_FALSE(c.overrides[1].enabled);
    CHECK_FALSE(c.overrides[0].verified);
    CHECK(c.overrides[1].verified);
}

TEST_CASE("game config: override errors name the file and line") {
    CHECK(error_of("[[override]]\nimpl = \"x\"\n").find("needs exactly one of 'addr'") != std::string::npos);
    CHECK(error_of("[[override]]\naddr = 0x08000000\nmode = \"arm\"\nsymbol = \"A\"\nimpl = \"x\"\n")
              .find("needs exactly one of 'addr'") != std::string::npos);
    CHECK(error_of("[[override]]\nsymbol = \"A\"\nmode = \"arm\"\nimpl = \"x\"\n")
              .starts_with("game.toml:3: override.mode goes with addr"));
    CHECK(error_of("[[override]]\nsymbol = \"A\"\nimpl = \"not-an-identifier\"\n")
              .starts_with("game.toml:3: override.impl must be the C++ identifier"));
    CHECK(error_of("[[override]]\nsymbol = \"A\"\n").find("override needs 'impl'") != std::string::npos);
    CHECK(error_of("[[override]]\nsymbol = \"A\"\nimpl = \"x\"\nenabled = 1\n")
              .starts_with("game.toml:4: override.enabled must be true or false"));
    CHECK(error_of("[[override]]\nsymbol = \"A\"\nimpl = \"x\"\nname = \"y\"\n")
              .starts_with("game.toml:4: unknown key 'name' in [[override]]"));
    CHECK(error_of("[functions]\nA = { addr = 0x08000000 }\n").find("functions.A needs 'mode'") != std::string::npos);
    CHECK(error_of("[functions]\nA = 0x08000000\n").starts_with("game.toml:2: functions.A must be an identifier"));
    CHECK(error_of("[game]\nsources = [\"x.h\"]\n").find(".c or .cpp file names") != std::string::npos);
}

TEST_CASE("override module: overrides.toml with [[override]] only, sources next to it") {
    const anygba::test::TempDir temp("anygba-override-module-test");
    const auto& dir = temp.path;
    write(dir / "overrides.toml", "[[override]]\nsymbol = \"StringCopy\"\nimpl = \"gen3_StringCopy\"\n");
    write(dir / "b.cpp", "");
    write(dir / "a.c", "");
    write(dir / "notes.md", "");
    const OverrideModule module = load_override_module(dir);
    REQUIRE(module.overrides.size() == 1);
    REQUIRE(module.sources.size() == 2);
    CHECK(module.sources[0].filename() == "a.c");
    CHECK(module.sources[1].filename() == "b.cpp");

    CHECK_THROWS_WITH(parse_override_module("[game]\ntitle = \"x\"\n", "overrides.toml"),
                      doctest::Contains("unknown key 'game' in the top level of an override module"));
    CHECK_THROWS_WITH(parse_override_module("", "overrides.toml"), doctest::Contains("at least one [[override]]"));
    CHECK_THROWS_WITH(load_override_module(dir / "missing"), doctest::Contains("has no overrides.toml"));
}

TEST_CASE("pipeline: override symbols resolve through symbols files; conflicts are errors") {
    const anygba::test::TempDir temp("anygba-override-pipeline-test");
    const auto& dir = temp.path;
    {
        const auto rom = small_rom();
        std::ofstream(dir / "game.gba", std::ios::binary).write(reinterpret_cast<const char*>(rom.data()),
                                                                  static_cast<std::streamsize>(rom.size()));
    }
    std::filesystem::create_directories(dir / "module");
    write(dir / "module" / "overrides.toml",
          "[[override]]\nsymbol = \"Helper\"\nimpl = \"test_Helper\"\nengine = \"test\"\n"
          "[[override]]\naddr = 0x08000080\nmode = \"thumb\"\nimpl = \"test_Thumb\"\n");
    write(dir / "module" / "helper.cpp", "");
    write(dir / "symbols.toml", "[symbols.x]\naddress = 0x02000000\ntype = \"u8\"\n"
                                "[functions]\nHelper = { addr = 0x08000040, mode = \"arm\" }\n");

    anygba::recomp::Options options;
    options.rom = dir / "game.gba";
    options.override_modules = {dir / "module"};
    options.symbol_files = {dir / "symbols.toml"};
    const anygba::recomp::Analysis analysis = anygba::recomp::analyze(options);
    REQUIRE(analysis.overrides.size() == 2);
    CHECK(analysis.overrides[0].addr == 0x08000040);
    CHECK_FALSE(analysis.overrides[0].thumb);
    CHECK(analysis.overrides[0].name == "Helper");
    CHECK(analysis.overrides[0].engine == "test");
    CHECK(analysis.overrides[1].addr == 0x08000080);
    CHECK(analysis.overrides[1].thumb);
    CHECK(analysis.overrides[1].name == "0x08000080");
    CHECK(analysis.discovery.functions.contains(0x08000040));
    CHECK(analysis.discovery.functions.contains(0x08000080));
    REQUIRE(analysis.sources.size() == 1);
    CHECK(analysis.sources[0].filename() == "helper.cpp");

    options.symbol_files.clear();
    CHECK_THROWS_WITH(anygba::recomp::analyze(options),
                      doctest::Contains("override.symbol 'Helper' is not in any [functions] table (pass --symbols FILE)"));

    write(dir / "other.toml", "[functions]\nHelper = { addr = 0x08000044, mode = \"arm\" }\n");
    options.symbol_files = {dir / "symbols.toml", dir / "other.toml"};
    CHECK_THROWS_WITH(anygba::recomp::analyze(options), doctest::Contains("functions.Helper differs"));

    write(dir / "game.toml", "[[override]]\naddr = 0x08000040\nmode = \"arm\"\nimpl = \"again\"\n");
    options.symbol_files = {dir / "symbols.toml"};
    options.config = dir / "game.toml";
    CHECK_THROWS_WITH(anygba::recomp::analyze(options), doctest::Contains("a second override for 0x08000040"));

    write(dir / "game.toml", "[[skip]]\nstart = 0x08000030\nend = 0x08000050\n");
    CHECK_THROWS_WITH(anygba::recomp::analyze(options),
                      doctest::Contains("the entry point 0x08000040 (from [entry], [[ram_region]], [[replace]] or "
                                        "[[override]]) lies in [[skip]] 0x08000030-0x08000050"));
}

TEST_CASE("pipeline: overrides limited to other game codes are skipped") {
    const anygba::test::TempDir temp("anygba-override-games-test");
    const auto& dir = temp.path;
    {
        auto rom = small_rom();
        const char code[] = "ABCE";
        std::copy(code, code + 4, rom.begin() + 0xAC);
        std::ofstream(dir / "game.gba", std::ios::binary).write(reinterpret_cast<const char*>(rom.data()),
                                                                  static_cast<std::streamsize>(rom.size()));
    }
    std::filesystem::create_directories(dir / "module");
    write(dir / "module" / "overrides.toml",
          "[[override]]\naddr = 0x08000040\nmode = \"arm\"\nimpl = \"for_us\"\ngames = [\"XYZE\", \"ABCE\"]\n"
          "[[override]]\nsymbol = \"NotInThisGame\"\nimpl = \"for_others\"\ngames = [\"XYZE\"]\n");
    write(dir / "module" / "a.cpp", "");
    anygba::recomp::Options options;
    options.rom = dir / "game.gba";
    options.override_modules = {dir / "module"};
    const anygba::recomp::Analysis analysis = anygba::recomp::analyze(options);
    REQUIRE(analysis.overrides.size() == 1);
    CHECK(analysis.overrides[0].impl == "for_us");

    CHECK(error_of("[[override]]\naddr = 0x08000040\nmode = \"arm\"\nimpl = \"x\"\ngames = [\"BPE\"]\n")
              .find("4-character game codes") != std::string::npos);
    CHECK(error_of("[[override]]\naddr = 0x08000040\nmode = \"arm\"\nimpl = \"x\"\ngames = []\n")
              .find("non-empty list") != std::string::npos);
}

TEST_CASE("generated project: an overridden function keeps its translation behind a runtime switch") {
    const anygba::test::TempDir temp("anygba-override-codegen-test");
    const auto& dir = temp.path;
    write(dir / "mod.cpp", "// override module\n");
    const auto rom = small_rom();
    const anygba::analysis::Image image(rom);
    anygba::analysis::DiscoveryOptions options;
    options.entries.push_back({0x08000000, Mode::Arm, anygba::analysis::EntrySource::RomEntry});
    options.entries.push_back({0x08000040, Mode::Arm, anygba::analysis::EntrySource::Config});
    const auto discovery = anygba::analysis::discover(image, options);

    anygba::codegen::ProjectInfo info;
    info.title = "t";
    info.rom_sha256 = "00";
    info.rom_size = 0x100;
    const auto out = dir / "gen";
    anygba::codegen::write_project(out, image, discovery, info);
    const std::string plain_dispatch = read(out / "dispatch.c");
    CHECK(plain_dispatch.find("override") == std::string::npos);
    CHECK(read(out / "CMakeLists.txt").find("*.cpp") == std::string::npos);

    info.overrides.push_back({0x08000040, false, "test_Helper", "Helper", "test", true});
    info.sources.push_back(dir / "mod.cpp");
    anygba::codegen::write_project(out, image, discovery, info);
    const std::string header = read(out / "game.h");
    CHECK(header.find("void f_08000040_orig(anygba_cpu* c);") != std::string::npos);
    CHECK(header.find("extern uint8_t ag_override_on[];") != std::string::npos);
    const std::string funcs = read(out / "funcs_000.c");
    CHECK(funcs.find("void f_08000040_orig(anygba_cpu* c)") != std::string::npos);
    CHECK(funcs.find("void f_08000040(anygba_cpu* c) {\n    if (ag_override_on[0]) {\n        anygba_override_run(c, 0u);"
                     "\n        return;\n    }\n    f_08000040_orig(c);\n}") != std::string::npos);
    const std::string dispatch = read(out / "dispatch.c");
    CHECK(dispatch.find("uint8_t ag_override_on[1];") != std::string::npos);
    // The translation counts its blocks for difftest coverage: one block here (bx lr).
    CHECK(funcs.find("uint32_t f_08000040_hits[1];\nconst uint32_t f_08000040_blocks[1] = {0x08000040u};") !=
          std::string::npos);
    CHECK(funcs.find("f_08000040_hits[0]++;") != std::string::npos);
    CHECK(funcs.find("++;") == funcs.rfind("++;"));  // functions without an override count nothing
    CHECK(header.find("extern uint32_t f_08000040_hits[];") != std::string::npos);
    CHECK(dispatch.find("{0x08000040u, 0, \"test_Helper\", \"Helper\", \"test\", 1, f_08000040_orig, 0, "
                        "f_08000040_hits, f_08000040_blocks, 1u},") != std::string::npos);
    CHECK(dispatch.find("    overrides,\n    1u,\n    ag_override_on,\n};") != std::string::npos);
    const std::string cmake = read(out / "CMakeLists.txt");
    CHECK(cmake.find("*.cpp") != std::string::npos);
    CHECK(cmake.find("cxx_std_20") != std::string::npos);
    CHECK(cmake.find("CXX_SCAN_FOR_MODULES OFF") != std::string::npos);
    CHECK(read(out / "mod.cpp") == "// override module\n");

    info.overrides[0].verified = true;
    anygba::codegen::write_project(out, image, discovery, info);
    CHECK(read(out / "dispatch.c").find("1, f_08000040_orig, 1, f_08000040_hits") != std::string::npos);

    info.overrides[0].thumb = true;
    CHECK_THROWS_WITH(anygba::codegen::write_project(out, image, discovery, info), doctest::Contains("is ARM code"));
    info.overrides[0] = {0x08000044, false, "x", "x", "", true};
    CHECK_THROWS_WITH(anygba::codegen::write_project(out, image, discovery, info),
                      doctest::Contains("override for 0x08000044: no function starts there"));
}
