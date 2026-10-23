#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace anygba::test {

// One register write of a script ROM, then a wait of `frames` VBlanks.
struct RegWrite {
    std::uint32_t addr = 0;
    std::uint32_t value = 0;
    int size = 2;  // 2 or 4 bytes
    std::uint32_t frames = 0;
};

// A self-authored ROM: a small ARM loop that performs `script` (polling VCOUNT for the
// waits) and then idles. `blob` is placed at kBlobAddress for DMA sources and the like.
inline constexpr std::uint32_t kBlobAddress = 0x08001000;
std::vector<std::uint8_t> make_script_rom(const std::vector<RegWrite>& script, const std::vector<std::uint8_t>& blob = {});

struct TestRom {
    std::string name;
    std::vector<std::uint8_t> rom;
};

// Sound test ROMs: each PSG channel on its own, the DMA FIFO, and a panned mix. Written to
// disk by anygba-test-roms so the mGBA oracle plays exactly the same programs.
std::vector<TestRom> apu_test_roms();

}  // namespace anygba::test
