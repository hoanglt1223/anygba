#include "anygba/system.hpp"
#include "anygba/embedded_rom.h"
#include "anygba/override.hpp"
#include "input_map.h"
#include "save/save.h"
#include "support/temp_dir.h"

#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <chrono>
#include <thread>
#endif

namespace fs = std::filesystem;
using namespace anygba::save;
using anygba::test::TempDir;

namespace {

std::vector<std::uint8_t> rom_containing(std::string_view id) {
    std::vector<std::uint8_t> rom(0x400, 0);
    std::copy(id.begin(), id.end(), rom.begin() + 0x200);
    return rom;
}

anygba::Options quiet() {
    anygba::Options o;
    o.log_misses = false;
    return o;
}

std::vector<std::uint8_t> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void flash_cmd(Backup& b, std::uint8_t cmd) {
    b.write8(0x0E005555, 0xAA);
    b.write8(0x0E002AAA, 0x55);
    b.write8(0x0E005555, cmd);
}

// Writes `bits` as halfwords to EWRAM at `src`, then DMA 3 copies them to the EEPROM.
void eeprom_dma_out(anygba::System& sys, const std::vector<int>& bits) {
    for (std::size_t i = 0; i < bits.size(); ++i) {
        sys.write16(0x02000000 + static_cast<std::uint32_t>(i) * 2, static_cast<std::uint32_t>(bits[i]));
    }
    sys.write32(0x040000D4, 0x02000000);
    sys.write32(0x040000D8, 0x0D000000);
    sys.io_write16(0x0DC, static_cast<std::uint16_t>(bits.size()));
    sys.io_write16(0x0DE, 0x8000);  // enable, immediate, 16-bit, increment
}

std::vector<int> eeprom_dma_in(anygba::System& sys) {
    sys.write32(0x040000D4, 0x0D000000);
    sys.write32(0x040000D8, 0x02001000);
    sys.io_write16(0x0DC, 68);
    sys.io_write16(0x0DE, 0x8000);
    std::vector<int> bits;
    for (std::uint32_t i = 0; i < 68; ++i) {
        bits.push_back(static_cast<int>(sys.read16(0x02001000 + i * 2) & 1));
    }
    return bits;
}

void push_bits(std::vector<int>& out, std::uint64_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
        out.push_back(static_cast<int>((value >> i) & 1u));
    }
}

}  // namespace

TEST_CASE("save type detection from library ID strings, and names") {
    CHECK(detect(rom_containing("SRAM_V113")) == Type::Sram);
    CHECK(detect(rom_containing("SRAM_F_V100")) == Type::Sram);
    CHECK(detect(rom_containing("FLASH_V126")) == Type::Flash64);
    CHECK(detect(rom_containing("FLASH512_V131")) == Type::Flash64);
    CHECK(detect(rom_containing("FLASH1M_V103")) == Type::Flash128);
    CHECK(detect(rom_containing("EEPROM_V124")) == Type::Eeprom);
    CHECK(detect(rom_containing("nothing")) == Type::None);
    for (const char* n : {"none", "sram", "flash64", "flash128", "eeprom", "eeprom512", "eeprom8k"}) {
        REQUIRE(parse(n));
        CHECK(std::string(name(*parse(n))) == n);
    }
    CHECK_FALSE(parse("flash"));
}

TEST_CASE("the per-game save type overrides detection; an unknown one is an error") {
    anygba::Options o = quiet();
    o.save_type = "flash128";
    anygba::System sys(rom_containing("SRAM_V113"), nullptr, o);
    CHECK(sys.backup().type() == Type::Flash128);
    o.save_type = "flash";
    CHECK_THROWS_AS(anygba::System(rom_containing(""), nullptr, o), anygba::Error);
}

TEST_CASE("SRAM: an unaligned 32-bit store keeps its byte lane, from translated code and from an override") {
    anygba::System translated(rom_containing("SRAM_V113"), nullptr, quiet());
    anygba::System overridden(rom_containing("SRAM_V113"), nullptr, quiet());
    ag_wr32(&translated.cpu(), 0x0E000011, 0xAABBCCDD);  // what a translated str does
    anygba::guest::Guest g(&overridden.cpu(), 0x08000000);
    g.write<std::uint32_t>(0x0E000011, 0xAABBCCDD);
    for (std::uint32_t a = 0x0E000010; a < 0x0E000014; ++a) {
        CHECK(overridden.read8(a) == translated.read8(a));
    }
    CHECK(translated.read8(0x0E000011) == 0xCC);
    CHECK(translated.read8(0x0E000010) == 0xFF);
}

TEST_CASE("SRAM: 32 KB mirrored, no save reads 0xFF") {
    anygba::System sys(rom_containing("SRAM_V113"), nullptr, quiet());
    CHECK(sys.read8(0x0E000010) == 0xFF);
    sys.write8(0x0E000010, 0x5A);
    CHECK(sys.read8(0x0E008010) == 0x5A);
    CHECK(sys.read8(0x0F000010) == 0x5A);
    CHECK(sys.backup().data().size() == 0x8000);

    anygba::System none(rom_containing(""), nullptr, quiet());
    none.write8(0x0E000000, 1);
    CHECK(none.read8(0x0E000000) == 0xFF);
}

TEST_CASE("Flash: IDs, program, sector and chip erase, bank switch") {
    auto f128 = make_flash(true);
    flash_cmd(*f128, 0x90);
    CHECK(f128->read8(0x0E000000) == 0x62);
    CHECK(f128->read8(0x0E000001) == 0x13);
    flash_cmd(*f128, 0xF0);
    CHECK(f128->read8(0x0E000000) == 0xFF);

    flash_cmd(*f128, 0xA0);
    f128->write8(0x0E001234, 0x12);
    CHECK(f128->read8(0x0E001234) == 0x12);
    f128->write8(0x0E001235, 0x34);  // not armed: ignored
    CHECK(f128->read8(0x0E001235) == 0xFF);

    flash_cmd(*f128, 0xB0);
    f128->write8(0x0E000000, 1);
    CHECK(f128->read8(0x0E001234) == 0xFF);
    flash_cmd(*f128, 0xA0);
    f128->write8(0x0E001234, 0x56);
    flash_cmd(*f128, 0xB0);
    f128->write8(0x0E000000, 0);
    CHECK(f128->read8(0x0E001234) == 0x12);
    CHECK(f128->data()[0x11234] == 0x56);

    flash_cmd(*f128, 0x80);
    f128->write8(0x0E005555, 0xAA);
    f128->write8(0x0E002AAA, 0x55);
    f128->write8(0x0E001000, 0x30);  // erase sector 1 of bank 0
    CHECK(f128->read8(0x0E001234) == 0xFF);
    CHECK(f128->data()[0x11234] == 0x56);
    flash_cmd(*f128, 0x80);
    flash_cmd(*f128, 0x10);
    CHECK(f128->data()[0x11234] == 0xFF);

    auto f64 = make_flash(false);
    flash_cmd(*f64, 0x90);
    CHECK(f64->read8(0x0E000000) == 0x32);
    CHECK(f64->read8(0x0E000001) == 0x1B);
    CHECK(f64->data().size() == 0x10000);
}

TEST_CASE("EEPROM through DMA 3: size from the first transfer, write then read back") {
    for (const int width : {6, 14}) {
        CAPTURE(width);
        anygba::System sys(rom_containing("EEPROM_V124"), nullptr, quiet());
        REQUIRE(sys.backup().is_eeprom());
        const std::uint64_t value = 0x0123456789ABCDEFull;
        std::vector<int> write = {1, 0};
        push_bits(write, 5, width);
        push_bits(write, value, 64);
        write.push_back(0);
        eeprom_dma_out(sys, write);
        CHECK(sys.backup().type() == (width == 6 ? Type::Eeprom512 : Type::Eeprom8k));
        CHECK(sys.backup().data()[5 * 8] == 0x01);
        CHECK(sys.backup().data()[5 * 8 + 7] == 0xEF);
        CHECK(sys.read16(0x0D000000) == 1);  // ready

        std::vector<int> request = {1, 1};
        push_bits(request, 5, width);
        request.push_back(0);
        eeprom_dma_out(sys, request);
        const std::vector<int> bits = eeprom_dma_in(sys);
        std::uint64_t got = 0;
        for (int i = 4; i < 68; ++i) {
            got = (got << 1) | static_cast<std::uint64_t>(bits[static_cast<std::size_t>(i)]);
        }
        CHECK(got == value);
    }
}

TEST_CASE("EEPROM of unknown size refuses a CPU access and names the config fix") {
    auto e = make_eeprom(0);
    CHECK_THROWS_WITH(e->eeprom_write(1, 0), doctest::Contains("eeprom8k"));
    CHECK_THROWS(e->load(std::vector<std::uint8_t>(100)));
    e->load(std::vector<std::uint8_t>(512, 0));
    CHECK(e->type() == Type::Eeprom512);
}

#if defined(_WIN32)
// OneDrive holds a file it syncs open without delete sharing for a moment; the write waits.
TEST_CASE("save file: a write retries while another program holds the file open") {
    TempDir dir("anygba-save-lock test");  // a space, as in "My Games"
    const fs::path path = dir.path / "game.sav";
    REQUIRE_FALSE(replace_file(path, std::vector<std::uint8_t>{1, 2, 3}, true));
    HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    std::thread release([held] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CloseHandle(held);
    });
    const auto error = replace_file(path, std::vector<std::uint8_t>{4, 5}, true);
    release.join();
    CHECK_FALSE(error);
    CHECK(read_all(path) == std::vector<std::uint8_t>{4, 5});
    CHECK(read_all(fs::path(path).concat(".bak")) == std::vector<std::uint8_t>{1, 2, 3});

    // Held for longer than the retries last: an error naming the file, the old file intact.
    held = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    const auto stuck = replace_file(path, std::vector<std::uint8_t>{6}, true);
    CloseHandle(held);
    REQUIRE(stuck);
    CHECK(stuck->find("game.sav") != std::string::npos);
    CHECK(read_all(path) == std::vector<std::uint8_t>{4, 5});
}
#endif

TEST_CASE("import: .sav sizes from other emulators, footers trimmed") {
    std::string error;
    const auto sized = [](std::size_t n) { return std::vector<std::uint8_t>(n, 0x5A); };
    CHECK(fit_import(Type::Sram, sized(0x8000), &error)->size() == 0x8000);
    CHECK(fit_import(Type::Flash128, sized(0x20000 + 16), &error)->size() == 0x20000);  // RTC bytes
    CHECK(fit_import(Type::Flash64, sized(0x10000 + kMaxFooter), &error)->size() == 0x10000);
    CHECK(fit_import(Type::Eeprom, sized(512), &error)->size() == 512);
    CHECK(fit_import(Type::Eeprom, sized(8192 + 4), &error)->size() == 8192);
    CHECK(fit_import(Type::Eeprom8k, sized(8192), &error)->size() == 8192);

    CHECK_FALSE(fit_import(Type::Flash128, sized(0x10000), &error));  // a 64 KB Flash save
    CHECK(error == "65536 bytes does not fit the flash128 save (131072 bytes)");
    CHECK_FALSE(fit_import(Type::Sram, sized(0x10000), &error));      // too much to be a footer
    CHECK_FALSE(fit_import(Type::Eeprom512, sized(8192), &error));
    CHECK_FALSE(fit_import(Type::None, sized(0x8000), &error));
    CHECK(error == "this game has no save chip");
}

TEST_CASE("save file: debounced writes, backup of the previous file, reload") {
    TempDir dir("anygba-save-test");
    const fs::path path = dir.path / "game.sav";
    {
        auto sram = make_sram();
        SaveFile file(path, *sram, 3);
        file.load();  // no file yet
        sram->write8(0x0E000000, 7);
        CHECK_FALSE(file.on_frame());
        CHECK_FALSE(file.on_frame());
        CHECK_FALSE(file.on_frame());
        CHECK_FALSE(fs::exists(path));
        CHECK_FALSE(file.on_frame());
        REQUIRE(fs::exists(path));
        CHECK(read_all(path).size() == 0x8000);
        CHECK(read_all(path)[0] == 7);

        sram->write8(0x0E000000, 8);
        CHECK_FALSE(file.flush());
        CHECK(read_all(path)[0] == 8);
        CHECK(read_all(fs::path(path).concat(".bak"))[0] == 7);
        CHECK_FALSE(fs::exists(fs::path(path).concat(".tmp")));
    }
    auto again = make_sram();
    SaveFile reloaded(path, *again);
    reloaded.load();
    CHECK(again->read8(0x0E000000) == 8);
    CHECK_FALSE(reloaded.flush());  // nothing changed: no write

    auto flash = make_flash(true);
    SaveFile wrong(path, *flash);
    CHECK_THROWS_WITH(wrong.load(), doctest::Contains("game.sav"));
}

TEST_CASE("input mapping: defaults, overrides, unbinding, errors") {
    using anygba::input::parse_mapping;
    using anyport::platform::Button;
    const auto m = parse_mapping(
        "# comment\n[keyboard]\nA = K, Space\nB =\nFastForward = Left Shift\n\n[gamepad]\nA = East\nL = LeftTrigger\n",
        "t.ini");
    CHECK(m.keys[0] == std::vector<std::string>{"K", "Space"});
    CHECK(m.keys[1].empty());
    CHECK(m.keys[3] == std::vector<std::string>{"Return"});  // untouched default
    CHECK(m.hotkeys[static_cast<int>(anygba::input::Hotkey::FastForward)] == std::vector<std::string>{"Left Shift"});
    CHECK(m.pad[0] == std::vector<Button>{Button::East});
    CHECK(m.pad[9] == std::vector<Button>{Button::LeftTrigger});

    CHECK_THROWS_WITH(parse_mapping("[keyboard]\nTurbo = X\n", "t.ini"), doctest::Contains("t.ini:2"));
    CHECK_THROWS_WITH(parse_mapping("[keyboard]\nA = NoSuchKey\n", "t.ini"), doctest::Contains("NoSuchKey"));
    CHECK_THROWS_WITH(parse_mapping("[gamepad]\nA = Cross\n", "t.ini"), doctest::Contains("Cross"));
    CHECK_THROWS_WITH(parse_mapping("[mouse]\n", "t.ini"), doctest::Contains("[mouse]"));
    CHECK_THROWS(parse_mapping("A = X\n", "t.ini"));

    const auto round_trip = parse_mapping(anygba::input::format_mapping(m), "round-trip");
    CHECK(round_trip.keys == m.keys);
    CHECK(round_trip.pad == m.pad);
    CHECK(round_trip.hotkeys == m.hotkeys);
}

TEST_CASE("embedded ROM trailer") {
    TempDir dir("anygba-embed-test");
    const fs::path exe = dir.path / "game.exe";
    const std::vector<std::uint8_t> rom = rom_containing("SRAM_V113");
    {
        std::ofstream out(exe, std::ios::binary);
        out << "MZ program bytes";
    }
    anygba::append_rom(exe, rom);
    const auto got = anygba::read_appended_rom(exe);
    REQUIRE(got);
    CHECK(*got == rom);

    const fs::path plain = dir.path / "plain.exe";
    std::ofstream(plain, std::ios::binary) << "MZ no trailer here, long enough";
    CHECK_FALSE(anygba::read_appended_rom(plain));
}
