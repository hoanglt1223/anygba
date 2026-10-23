#include "support/headless_run.h"

#include "anygba/sha256.h"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace anygba::test {

KeyHold parse_key_hold(std::string_view spec) {
    static constexpr std::pair<std::string_view, std::uint16_t> kKeys[] = {
        {"a", 1},      {"b", 2},     {"select", 4}, {"start", 8}, {"right", 16},
        {"left", 32},  {"up", 64},   {"down", 128}, {"r", 256},   {"l", 512},
    };
    const auto colon = spec.find(':');
    const auto dash = colon == std::string_view::npos ? colon : spec.find('-', colon);
    if (dash == std::string_view::npos) {
        throw std::invalid_argument("key hold must look like KEY:FIRST-LAST, got " + std::string(spec));
    }
    KeyHold hold;
    const std::string_view name = spec.substr(0, colon);
    for (const auto& [key, mask] : kKeys) {
        if (key == name) hold.mask = mask;
    }
    if (!hold.mask) {
        throw std::invalid_argument("unknown key " + std::string(name));
    }
    hold.first = std::strtoull(std::string(spec.substr(colon + 1, dash - colon - 1)).c_str(), nullptr, 10);
    hold.last = std::strtoull(std::string(spec.substr(dash + 1)).c_str(), nullptr, 10);
    return hold;
}

void run_headless(std::vector<std::uint8_t> rom, std::uint64_t frames, const std::vector<KeyHold>& keys,
                  const std::function<void(std::uint64_t, System&)>& on_frame) {
    Options options;
    options.log_misses = false;
    System sys(std::move(rom), nullptr, options);
    const auto keys_for = [&keys](std::uint64_t frame) {
        std::uint16_t mask = 0;
        for (const KeyHold& k : keys) {
            if (frame >= k.first && frame <= k.last) mask = static_cast<std::uint16_t>(mask | k.mask);
        }
        return mask;
    };
    sys.set_keys(keys_for(1));
    sys.on_vblank = [&](System& s) {
        on_frame(s.frames(), s);
        if (s.frames() >= frames) {
            s.stop();
        }
        s.set_keys(keys_for(s.frames() + 1));
    };
    sys.run();
}

std::string frame_hash(std::span<const std::uint16_t> frame) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(frame.size() * 2);
    for (std::uint16_t v : frame) {
        bytes.push_back(static_cast<std::uint8_t>(v));
        bytes.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    return sha256_hex(bytes);
}

}  // namespace anygba::test
