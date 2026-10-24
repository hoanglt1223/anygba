#include "video/hd/keys.h"

#include <array>
#include <charconv>
#include <cstdio>

namespace anygba::video::hd {

namespace {

constexpr std::array<std::string_view, kKeyTypes> kPrefixes = {"t", "ti", "o", "oi", "b"};

// One leading byte per kind keeps keys of different kinds apart even for equal pixels.
constexpr std::array<std::uint8_t, kKeyTypes> kTag = {'T', 'I', 'O', 'P', 'B'};

void header(Fnv& f, KeyType type, const Art& art) {
    f.byte(kTag[static_cast<int>(type)]);
    f.u16(static_cast<std::uint16_t>(art.width));
    f.u16(static_cast<std::uint16_t>(art.height));
}

}  // namespace

std::string_view prefix(KeyType type) { return kPrefixes[static_cast<int>(type)]; }

std::string key_name(KeyType type, std::uint64_t hash) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hash));
    return std::string(prefix(type)) + "-" + buf;
}

std::optional<ParsedKey> parse_key(std::string_view name) {
    const auto dash = name.find('-');
    if (dash == std::string_view::npos || name.size() - dash - 1 != 16) return std::nullopt;
    const std::string_view p = name.substr(0, dash);
    for (int i = 0; i < kKeyTypes; ++i) {
        if (kPrefixes[static_cast<std::size_t>(i)] != p) continue;
        std::uint64_t hash = 0;
        const char* first = name.data() + dash + 1;
        const char* last = name.data() + name.size();
        const auto [end, ec] = std::from_chars(first, last, hash, 16);
        if (ec != std::errc{} || end != last) return std::nullopt;
        return ParsedKey{static_cast<KeyType>(i), hash};
    }
    return std::nullopt;
}

std::uint64_t color_key(KeyType type, const Art& art) {
    Fnv f;
    header(f, type, art);
    for (const std::uint16_t c : art.colors) f.u16((c & kClear) ? kClear : static_cast<std::uint16_t>(c & 0x7FFF));
    return f.value();
}

std::uint64_t index_key(KeyType type, const Art& art) {
    if (art.indices.empty()) return 0;
    Fnv f;
    header(f, type, art);
    f.byte(art.bpp8 ? 8 : 4);
    for (const std::uint8_t i : art.indices) f.byte(i);
    return f.value();
}

}  // namespace anygba::video::hd
