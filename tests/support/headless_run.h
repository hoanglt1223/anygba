#pragma once

#include "anygba/system.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anygba::test {

// A button held from frame `first` to `last` inclusive (KEYINPUT bit mask).
struct KeyHold {
    std::uint16_t mask = 0;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

// Parses "start:10-12" (same syntax as tests/oracle/mgba-oracle.py --keys).
KeyHold parse_key_hold(std::string_view spec);

// Runs `rom` entirely in the interpreter for `frames` frames, holding `keys`, and calls
// `on_frame(frame, system)` at every VBlank (frame numbers start at 1, as in the oracle).
void run_headless(std::vector<std::uint8_t> rom, std::uint64_t frames, const std::vector<KeyHold>& keys,
                  const std::function<void(std::uint64_t, System&)>& on_frame);

// SHA-256 of the RGB555 frame as little-endian halfwords.
std::string frame_hash(std::span<const std::uint16_t> frame);

}  // namespace anygba::test
