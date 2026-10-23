#include "bios/bios.h"

namespace anygba::bios {

namespace {

// The BIOS silently does nothing when the source range starts or ends inside the BIOS.
bool source_allowed(std::uint32_t src, std::uint32_t bytes) {
    return (src & 0x0E000000) != 0 && ((src + bytes) & 0x0E000000) != 0;
}

}  // namespace

void cpu_set(System& sys) {
    anygba_cpu& c = sys.cpu();
    const std::uint32_t control = c.r[2];
    const std::uint32_t count = control & 0x1FFFFF;
    const bool fill = (control & (1u << 24)) != 0;
    const bool word = (control & (1u << 26)) != 0;
    const std::uint32_t unit = word ? 4 : 2;
    std::uint32_t src = c.r[0] & ~(unit - 1);
    std::uint32_t dst = c.r[1] & ~(unit - 1);
    if (!source_allowed(src, fill ? unit : count * unit)) {
        spend(sys, 30);
        return;
    }
    const std::uint32_t fill_value = word ? sys.read32(src) : sys.read16(src);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (word) {
            sys.write32(dst, fill ? fill_value : sys.read32(src));
        } else {
            sys.write16(dst, fill ? fill_value : sys.read16(src));
        }
        if (!fill) src += unit;
        dst += unit;
    }
    spend(sys, 30 + static_cast<std::int64_t>(count) * (fill ? 3 : 5));
}

void cpu_fast_set(System& sys) {
    anygba_cpu& c = sys.cpu();
    const std::uint32_t control = c.r[2];
    const std::uint32_t count = ((control & 0x1FFFFF) + 7) & ~7u;
    const bool fill = (control & (1u << 24)) != 0;
    std::uint32_t src = c.r[0] & ~3u;
    std::uint32_t dst = c.r[1] & ~3u;
    if (!source_allowed(src, fill ? 4 : count * 4)) {
        spend(sys, 30);
        return;
    }
    const std::uint32_t fill_value = sys.read32(src);
    for (std::uint32_t i = 0; i < count; ++i) {
        sys.write32(dst, fill ? fill_value : sys.read32(src));
        if (!fill) src += 4;
        dst += 4;
    }
    spend(sys, 30 + static_cast<std::int64_t>(count) * (fill ? 1 : 2));
}

}  // namespace anygba::bios
