#pragma once

#include "cheats/codes.h"

#include <cstdint>
#include <map>
#include <vector>

namespace anygba {
class System;
}

namespace anygba::cheats {

struct FrameKeys {
    std::uint16_t pressed = 0;  // GBA buttons, KEYINPUT order, 1 = pressed
    bool cheat_button = false;  // the GameShark / Action Replay device button
};

// Runs cheat programs the way a cheat device's hook does, once per frame: RAM and I/O writes,
// conditions and button codes. ROM patches are not run per frame; rom_patches() collects them
// for System::set_rom_patches.
void run(const Program& program, System& sys, const FrameKeys& keys);

// Halfword address -> value for every ROM patch in `programs` (later programs win).
std::map<std::uint32_t, std::uint16_t> rom_patches(const std::vector<const Program*>& programs);

}  // namespace anygba::cheats
