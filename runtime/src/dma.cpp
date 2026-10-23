#include "anygba/system.hpp"

#include <cstring>

// DMA channels 0-3 (GBATEK "DMA Transfers"). Transfers run instantly; their bus time is
// charged to the cycle budget.
namespace anygba {

namespace {

constexpr std::uint32_t kBase[4] = {0x0B0, 0x0BC, 0x0C8, 0x0D4};
constexpr std::uint32_t kSrcMask[4] = {0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF};
constexpr std::uint32_t kDstMask[4] = {0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF};
constexpr std::uint32_t kMaxCount[4] = {0x4000, 0x4000, 0x4000, 0x10000};

std::uint32_t raw32(const std::vector<std::uint8_t>& io, std::uint32_t off) {
    std::uint32_t v;
    std::memcpy(&v, io.data() + off, 4);
    return v;
}

std::uint16_t raw16(const std::vector<std::uint8_t>& io, std::uint32_t off) {
    std::uint16_t v;
    std::memcpy(&v, io.data() + off, 2);
    return v;
}

}  // namespace

void System::dma_write_control(int ch, std::uint16_t value) {
    DmaChannel& d = dma_[ch];
    const bool was_enabled = (d.control & 0x8000) != 0;
    d.control = ch == 3 ? value : static_cast<std::uint16_t>(value & 0xF7FF);
    if (!(d.control & 0x8000)) {
        d.active = false;
        return;
    }
    if (!was_enabled) {
        // Enabling latches the address and count registers.
        d.src = raw32(mem_.io, kBase[ch]) & kSrcMask[ch];
        d.dst = raw32(mem_.io, kBase[ch] + 4) & kDstMask[ch];
        const std::uint32_t count = raw16(mem_.io, kBase[ch] + 8) & (kMaxCount[ch] - 1);
        d.count = count == 0 ? kMaxCount[ch] : count;
    }
    d.active = true;
    if (((d.control >> 12) & 3) == 0) {
        run_dma(ch);
    }
}

void System::dma_trigger(int timing) {
    for (int ch = 0; ch < 4; ++ch) {
        if (dma_[ch].active && static_cast<int>((dma_[ch].control >> 12) & 3) == timing) {
            run_dma(ch);
        }
    }
}

void System::request_fifo_dma(int fifo) {
    const std::uint32_t fifo_addr = fifo == 0 ? 0x040000A0 : 0x040000A4;
    for (int ch = 1; ch <= 2; ++ch) {
        const DmaChannel& d = dma_[ch];
        if (d.active && ((d.control >> 12) & 3) == 3 && d.dst == fifo_addr) {
            run_dma(ch);
            return;
        }
    }
}

void System::run_dma(int ch) {
    DmaChannel& d = dma_[ch];
    const bool word = (d.control & 0x0400) != 0;
    const bool sound_fifo = ((d.control >> 12) & 3) == 3 && (ch == 1 || ch == 2);
    const std::uint32_t unit = (word || sound_fifo) ? 4 : 2;
    const std::uint32_t count = sound_fifo ? 4 : d.count;
    const auto step = [&](std::uint32_t control_bits) -> std::int32_t {
        switch (control_bits) {
        case 0: case 3: return static_cast<std::int32_t>(unit);
        case 1: return -static_cast<std::int32_t>(unit);
        default: return 0;
        }
    };
    const std::int32_t dst_step = sound_fifo ? 0 : step((d.control >> 5) & 3);
    const std::int32_t src_step = step((d.control >> 7) & 3);

    std::uint32_t src = d.src;
    std::uint32_t dst = d.dst;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (unit == 4) {
            write32(dst, read32(src));
        } else {
            write16(dst, read16(src));
        }
        src += static_cast<std::uint32_t>(src_step);
        dst += static_cast<std::uint32_t>(dst_step);
    }
    d.src = src;
    d.dst = dst;
    cpu_.cycles -= static_cast<std::int32_t>(2 + count * 2);

    if (d.control & 0x4000) {
        raise_irq(static_cast<std::uint16_t>(1u << (8 + ch)));
    }
    const bool repeat = (d.control & 0x0200) && ((d.control >> 12) & 3) != 0;
    if (repeat) {
        const std::uint32_t reload = raw16(mem_.io, kBase[ch] + 8) & (kMaxCount[ch] - 1);
        d.count = reload == 0 ? kMaxCount[ch] : reload;
        if (((d.control >> 5) & 3) == 3) {
            d.dst = raw32(mem_.io, kBase[ch] + 4) & kDstMask[ch];
        }
    } else {
        d.control &= 0x7FFF;
        d.active = false;
        std::uint16_t stored = raw16(mem_.io, kBase[ch] + 10);
        stored &= 0x7FFF;
        std::memcpy(mem_.io.data() + kBase[ch] + 10, &stored, 2);
    }
}

}  // namespace anygba
