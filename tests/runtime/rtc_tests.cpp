#include "anygba/system.hpp"

#include <doctest.h>

#include <array>
#include <cstdint>
#include <ctime>
#include <vector>

namespace {

constexpr std::uint32_t kData = 0x080000C4;
constexpr std::uint32_t kDir = 0x080000C6;
constexpr std::uint32_t kCtrl = 0x080000C8;

std::vector<std::uint8_t> rom_with_code(const char* code) {
    std::vector<std::uint8_t> rom(0x400, 0);
    for (int i = 0; i < 4; ++i) rom[0xAC + i] = static_cast<std::uint8_t>(code[i]);
    return rom;
}

anygba::Options fixed_clock(std::int64_t t) {
    anygba::Options o;
    o.log_misses = false;
    o.rtc_start = t;
    return o;
}

// Bit-bangs the S-3511 the way the Pokemon games do: CS low/SCK high, CS high, then
// the command byte MSB first, then reads or writes data bytes LSB first.
struct Port {
    anygba::System& sys;
    void begin() {
        sys.write16(kCtrl, 1);
        sys.write16(kDir, 7);
        sys.write16(kData, 1);
        sys.write16(kData, 5);
    }
    void end() { sys.write16(kData, 1); }
    void send(std::uint8_t byte, bool msb_first) {
        sys.write16(kDir, 7);
        for (int i = 0; i < 8; ++i) {
            const int bit = msb_first ? (byte >> (7 - i)) & 1 : (byte >> i) & 1;
            sys.write16(kData, static_cast<std::uint16_t>(4 | (bit << 1)));
            sys.write16(kData, static_cast<std::uint16_t>(5 | (bit << 1)));
        }
    }
    std::uint8_t receive() {
        sys.write16(kDir, 5);
        std::uint8_t v = 0;
        for (int i = 0; i < 8; ++i) {
            sys.write16(kData, 4);
            sys.write16(kData, 5);
            v |= static_cast<std::uint8_t>(((sys.read16(kData) >> 1) & 1) << i);
        }
        return v;
    }
    std::vector<std::uint8_t> read(std::uint8_t command, int n) {
        begin();
        send(command, true);
        std::vector<std::uint8_t> out;
        for (int i = 0; i < n; ++i) out.push_back(receive());
        end();
        return out;
    }
};

}  // namespace

TEST_CASE("RTC reports date and time in BCD from the configured clock") {
    // 2004-11-21 13:45:30 UTC, a Sunday
    anygba::System sys(rom_with_code("BPEE"), nullptr, fixed_clock(1101044730));
    REQUIRE(sys.has_rtc());
    Port port{sys};
    CHECK(port.read(0x63, 1) == std::vector<std::uint8_t>{0x40});  // status: 24-hour mode, power fine
    CHECK(port.read(0x65, 7) == std::vector<std::uint8_t>{0x04, 0x11, 0x21, 0x00, 0x13, 0x45, 0x30});
    CHECK(port.read(0x67, 3) == std::vector<std::uint8_t>{0x13, 0x45, 0x30});
}

TEST_CASE("RTC status writes are kept and reset clears them") {
    anygba::System sys(rom_with_code("BPEE"), nullptr, fixed_clock(1101044730));
    Port port{sys};
    port.begin();
    port.send(0x62, true);
    port.send(0x00, false);  // 12-hour mode
    port.end();
    CHECK(port.read(0x63, 1) == std::vector<std::uint8_t>{0x00});
    CHECK(port.read(0x67, 3) == std::vector<std::uint8_t>{0x81, 0x45, 0x30});  // 1 PM
    port.begin();
    port.send(0x60, true);
    port.end();
    CHECK(port.read(0x63, 1) == std::vector<std::uint8_t>{0x00});
}

TEST_CASE("RTC port reads back only when enabled, and only on clock cartridges") {
    auto rom = rom_with_code("BPEE");
    rom[0xC4] = 0xAB;
    anygba::System sys(rom, nullptr, fixed_clock(1101044730));
    CHECK(sys.read16(kData) == 0xAB);
    sys.write16(kCtrl, 1);
    CHECK(sys.read16(kCtrl) == 1);
    sys.write16(kCtrl, 0);
    CHECK(sys.read16(kData) == 0xAB);

    anygba::System plain(rom_with_code("AGBE"), nullptr, fixed_clock(1101044730));
    CHECK_FALSE(plain.has_rtc());
    anygba::Options forced = fixed_clock(1101044730);
    forced.rtc = "on";
    anygba::System on(rom_with_code("AGBE"), nullptr, forced);
    CHECK(on.has_rtc());
}

TEST_CASE("RTC state survives a save state") {
    anygba::System sys(rom_with_code("BPEE"), nullptr, fixed_clock(1101044730));
    const auto state = sys.save_state();
    anygba::System other(rom_with_code("BPEE"), nullptr, fixed_clock(1101044730));
    other.load_state(state);
    Port port{other};
    CHECK(port.read(0x67, 3) == std::vector<std::uint8_t>{0x13, 0x45, 0x30});
}

TEST_CASE("RTC without a start time reads the host's local clock") {
    anygba::Options o;
    o.log_misses = false;
    anygba::System sys(rom_with_code("BPEE"), nullptr, o);
    Port port{sys};
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const auto bcd = [](int v) { return static_cast<std::uint8_t>((v / 10) << 4 | (v % 10)); };
    const auto got = port.read(0x65, 7);
    CHECK(got[0] == bcd(tm.tm_year - 100));
    CHECK(got[1] == bcd(tm.tm_mon + 1));
    CHECK(got[4] == bcd(tm.tm_hour));
    CHECK(got[5] == bcd(tm.tm_min));  // fails only if the minute turns over mid-test
}
