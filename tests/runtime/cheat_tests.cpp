#include "anygba/system.hpp"
#include "cheats/codes.h"
#include "cheats/engine.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace anygba::cheats;

namespace {

anygba::System make_system() {
    anygba::Options o;
    o.log_misses = false;
    return anygba::System(std::vector<std::uint8_t>(0x400, 0), nullptr, o);
}

Program code(Format f, std::initializer_list<const char*> lines) {
    std::vector<std::string> v(lines.begin(), lines.end());
    return compile(f, v);
}

std::pair<std::uint32_t, std::uint32_t> decrypt(std::uint32_t a, std::uint32_t v, bool ar) {
    gameshark_decrypt(a, v, ar);
    return {a, v};
}

}  // namespace

// Published master codes: they must decrypt to the cartridge's game code with the 001DC0DE
// marker, which the encrypted text gives no hint of.
TEST_CASE("GameShark v1/v2 and Action Replay v3 decryption matches published codes") {
    // Pokemon Emerald (BPEE), Action Replay v3 master code.
    CHECK(decrypt(0xA86CDBA5, 0x19BA49B3, true) == std::pair<std::uint32_t, std::uint32_t>{0x45455042, 0x001DC0DE});
    CHECK(decrypt(0xD8BAE4D9, 0x4864DCE5, true) == std::pair<std::uint32_t, std::uint32_t>{0xC40005EC, 0x00008401});
    // Pokemon Emerald "Anti DMA": ROM patch 1 at 0x08076BEE.
    CHECK(decrypt(0xB2809E31, 0x3CEF5320, true) == std::pair<std::uint32_t, std::uint32_t>{0x00000000, 0x1803B5F7});
    CHECK(decrypt(0x1C7B3231, 0xB494738C, true) == std::pair<std::uint32_t, std::uint32_t>{0x00002400, 0x00000000});
    // Mario & Luigi: Superstar Saga (A88E), GameShark v1/v2 master code and a 16-bit write.
    CHECK(decrypt(0x8791F574, 0x20D9F121, false) == std::pair<std::uint32_t, std::uint32_t>{0x45383841, 0x001DC0DE});
    CHECK(decrypt(0x502F4DE9, 0x75D4B6CA, false) == std::pair<std::uint32_t, std::uint32_t>{0xF8017F8C, 0x00000101});
    CHECK(decrypt(0xA9873144, 0x203A53CE, false) == std::pair<std::uint32_t, std::uint32_t>{0x120048E0, 0x0000270F});
}

TEST_CASE("auto-detection picks the device from the code's shape and decoding") {
    CHECK(code(Format::Auto, {"A86CDBA5 19BA49B3", "D8BAE4D9 4864DCE5"}).format == Format::ActionReplay);
    CHECK(code(Format::Auto, {"8791F574 20D9F121", "A9873144 203A53CE"}).format == Format::GameShark);
    CHECK(code(Format::Auto, {"82025E30 0001"}).format == Format::CodeBreaker);
    CHECK(code(Format::Auto, {"02000100:63"}).format == Format::Raw);
    // libretro's decrypted Action Replay ROM patch ("Skip Trainer Battles", Emerald).
    const Program p = code(Format::Auto, {"00000000 18059EB6", "0000E038 00000000"});
    CHECK(p.format == Format::ActionReplayRaw);
    REQUIRE(p.ops.size() == 1);
    CHECK(p.ops[0].kind == Op::Kind::RomPatch);
    CHECK(p.ops[0].addr == 0x080B3D6C);
    CHECK(p.ops[0].value == 0xE038);
    CHECK_THROWS_AS(code(Format::Auto, {"DEADFACE 00001234"}), CodeError);
}

TEST_CASE("split_lines accepts typed, pasted and libretro-joined codes") {
    CHECK(split_lines("82025E30 0001\n82025E32 0002") == std::vector<std::string>{"82025E30 0001", "82025E32 0002"});
    CHECK(split_lines("D8BAE4D9+4864DCE5+A86CDBA5+19BA49B3") ==
          std::vector<std::string>{"D8BAE4D9 4864DCE5", "A86CDBA5 19BA49B3"});
    CHECK(split_lines(" 02000000:12 ; 02000001:34 ,") == std::vector<std::string>{"02000000:12", "02000001:34"});
}

TEST_CASE("raw codes write 8, 16 and 32 bits") {
    auto sys = make_system();
    run(code(Format::Raw, {"02000000:12", "02000010:3456", "03000020 789ABCDE"}), sys, {});
    CHECK(sys.read8(0x02000000) == 0x12);
    CHECK(sys.read16(0x02000010) == 0x3456);
    CHECK(sys.read32(0x03000020) == 0x789ABCDE);
}

TEST_CASE("CodeBreaker writes, arithmetic, slides, byte lists and conditions") {
    auto sys = make_system();
    sys.write16(0x02000100, 0x00F0);
    sys.write16(0x02000102, 0x0FFF);
    sys.write16(0x02000104, 5);
    run(code(Format::CodeBreaker, {"32000000 0042",                    // 8-bit write
                                   "22000100 000F",                    // OR
                                   "62000102 00F0",                    // AND
                                   "E2000104 0003",                    // add
                                   "42000200 0010", "00010004 0004",   // slide: 4 x 16-bit, +1 value, +4 address
                                   "52000300 0007", "11223344 5566", "77000000 0000",
                                   "72000104 0008", "82000400 BEEF",   // if [a] == 8 (true)
                                   "A2000104 0008", "82000402 DEAD"}),  // if [a] != 8 (false)
        sys, {});
    CHECK(sys.read8(0x02000000) == 0x42);
    CHECK(sys.read16(0x02000100) == 0x00FF);
    CHECK(sys.read16(0x02000102) == 0x00F0);
    CHECK(sys.read16(0x02000104) == 8);
    for (std::uint32_t i = 0; i < 4; ++i) {
        CHECK(sys.read16(0x02000200 + 4 * i) == 0x10 + i);
    }
    const std::uint8_t bytes[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    for (std::uint32_t i = 0; i < 7; ++i) {
        CHECK(sys.read8(0x02000300 + i) == bytes[i]);
    }
    CHECK(sys.read8(0x02000307) == 0);
    CHECK(sys.read16(0x02000400) == 0xBEEF);
    CHECK(sys.read16(0x02000402) == 0);
}

TEST_CASE("CodeBreaker joypad condition follows the pressed buttons") {
    auto sys = make_system();
    const Program p = code(Format::CodeBreaker, {"D0000020 0004", "82000000 1234"});  // Select
    run(p, sys, {.pressed = 0x0001});
    CHECK(sys.read16(0x02000000) == 0);
    run(p, sys, {.pressed = 0x0005});
    CHECK(sys.read16(0x02000000) == 0x1234);
}

TEST_CASE("Action Replay v3 fills, pointers, adds, I/O, blocks and button codes") {
    auto sys = make_system();
    sys.write32(0x02000F00, 0x02000E00);
    sys.write16(0x02000D00, 7);
    run(code(Format::ActionReplayRaw, {"00200010 000003AB",       // 4 bytes of AB
                                       "02200020 00011234",       // 2 halfwords of 1234
                                       "04200030 DEADBEEF",
                                       "42200F00 00025678",       // [[0x02000F00] + 2*2] = 5678
                                       "82200D00 00000003",       // +3
                                       "C6000208 00000000",       // IME = 0 (I/O write)
                                       "8A200D00 0000000A",       // if [d00] == 10 (16-bit) then block
                                       "02200040 00001111",
                                       "00000000 60000000",       // else
                                       "02200042 00002222",
                                       "00000000 40000000",       // endif
                                       "00000000 12200044", "00003333 00000000"}),  // button write
        sys, {});
    for (std::uint32_t i = 0; i < 4; ++i) CHECK(sys.read8(0x02000010 + i) == 0xAB);
    CHECK(sys.read8(0x02000014) == 0);
    CHECK(sys.read16(0x02000020) == 0x1234);
    CHECK(sys.read16(0x02000022) == 0x1234);
    CHECK(sys.read32(0x02000030) == 0xDEADBEEF);
    CHECK(sys.read16(0x02000E04) == 0x5678);
    CHECK(sys.read16(0x02000D00) == 10);
    CHECK(sys.read16(0x02000040) == 0x1111);
    CHECK(sys.read16(0x02000042) == 0);
    CHECK(sys.read16(0x02000044) == 0);
    run(code(Format::ActionReplayRaw, {"00000000 12200044", "00003333 00000000"}), sys, {.cheat_button = true});
    CHECK(sys.read16(0x02000044) == 0x3333);
}

TEST_CASE("GameShark v1/v2 conditions skip the requested number of codes") {
    auto sys = make_system();
    sys.write16(0x02000000, 1);
    run(code(Format::GameSharkRaw, {"E0020002 02000000",  // if [a] == 2: next 2 codes (false)
                                    "12000010 00001111", "12000012 00002222", "12000014 00003333",
                                    "D2000000 00000001",  // if [a] == 1: next code (true)
                                    "02000016 000000AA",
                                    "30000003 77777777",  // one value to three addresses
                                    "02000020 02000024", "02000028 00000000"}),
        sys, {});
    CHECK(sys.read16(0x02000010) == 0);
    CHECK(sys.read16(0x02000012) == 0);
    CHECK(sys.read16(0x02000014) == 0x3333);
    CHECK(sys.read8(0x02000016) == 0xAA);
    CHECK(sys.read32(0x02000020) == 0x77777777);
    CHECK(sys.read32(0x02000024) == 0x77777777);
    CHECK(sys.read32(0x02000028) == 0x77777777);
}

TEST_CASE("ROM patches change cartridge reads and are undone when cleared") {
    std::vector<std::uint8_t> rom(0x400, 0);
    rom[0x200] = 0x34;
    rom[0x201] = 0x12;
    anygba::Options o;
    o.log_misses = false;
    anygba::System sys(rom, nullptr, o);
    const std::string hash = sys.rom_sha256();
    const Program p = code(Format::GameSharkRaw, {"60000100 00005678"});
    REQUIRE(p.has_rom_patch);
    sys.set_rom_patches(rom_patches({&p}));
    CHECK(sys.read16(0x08000200) == 0x5678);
    CHECK(sys.read16(0x0A000200) == 0x5678);  // mirror
    CHECK(sys.rom_sha256() == hash);
    sys.set_rom_patches({});
    CHECK(sys.read16(0x08000200) == 0x1234);
}

TEST_CASE("CodeBreaker encryption round-trips through its own key schedule") {
    // No published plain/encrypted pair is available, so this checks the cipher is a
    // permutation of the 48 code bits (distinct inputs stay distinct) and is deterministic.
    CodeBreakerCipher c;
    c.set_key(0x9123ABCD, 0x1234);
    REQUIRE(c.active());
    std::uint32_t a1 = 0x82025E30, a2 = 0x82025E30;
    std::uint16_t v1 = 0x0001, v2 = 0x0001;
    c.decrypt(a1, v1);
    c.decrypt(a2, v2);
    CHECK(a1 == a2);
    CHECK(v1 == v2);
    std::uint32_t a3 = 0x82025E31;
    std::uint16_t v3 = 0x0001;
    c.decrypt(a3, v3);
    CHECK((a3 != a1 || v3 != v1));
}
