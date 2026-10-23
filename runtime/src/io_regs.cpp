#include "anygba/system.hpp"
#include "ppu/ppu.h"

#include <cstring>

// IO register reads and writes with their side effects. Registers without behaviour here
// keep the last value written.
namespace anygba {

namespace {

constexpr std::uint32_t kDispstat = 0x004;
constexpr std::uint32_t kVcount = 0x006;
constexpr std::uint32_t kKeyinput = 0x130;
constexpr std::uint32_t kKeycnt = 0x132;
constexpr std::uint32_t kIe = 0x200;
constexpr std::uint32_t kIf = 0x202;
constexpr std::uint32_t kWaitcnt = 0x204;
constexpr std::uint32_t kIme = 0x208;
constexpr std::uint32_t kPostflgHaltcnt = 0x300;

std::uint16_t raw(const std::vector<std::uint8_t>& io, std::uint32_t off) {
    std::uint16_t v;
    std::memcpy(&v, io.data() + off, 2);
    return v;
}

void set_raw(std::vector<std::uint8_t>& io, std::uint32_t off, std::uint16_t v) { std::memcpy(io.data() + off, &v, 2); }

}  // namespace

std::uint16_t System::io_read16(std::uint32_t off) const {
    off &= 0x3FE;
    switch (off) {
    case kDispstat: {
        const std::uint16_t stored = raw(mem_.io, kDispstat);
        std::uint16_t v = stored & 0xFF38;
        if (line_ >= 160 && line_ < 227) v |= 1;
        if (hblank_) v |= 2;
        if (line_ == static_cast<std::uint32_t>(stored >> 8)) v |= 4;
        return v;
    }
    case kVcount:
        return static_cast<std::uint16_t>(line_);
    case 0x0B0: case 0x0B2: case 0x0B4: case 0x0B6: case 0x0B8:  // DMA0 SAD/DAD/CNT_L
    case 0x0BC: case 0x0BE: case 0x0C0: case 0x0C2: case 0x0C4:
    case 0x0C8: case 0x0CA: case 0x0CC: case 0x0CE: case 0x0D0:
    case 0x0D4: case 0x0D6: case 0x0D8: case 0x0DA: case 0x0DC:
        return 0;  // write-only
    case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
        return dma_[(off - 0x0BA) / 12].control;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        return timer_counter(static_cast<int>((off - 0x100) / 4));
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        return timers_[(off - 0x102) / 4].control;
    default:
        return raw(mem_.io, off);
    }
}

void System::io_write16(std::uint32_t off, std::uint16_t value) {
    off &= 0x3FE;
    switch (off) {
    case kDispstat:
        set_raw(mem_.io, off, static_cast<std::uint16_t>((value & 0xFF38) | (raw(mem_.io, off) & 7)));
        return;
    case kVcount:
    case kKeyinput:
        return;  // read-only
    case kKeycnt:
        set_raw(mem_.io, off, value & 0xC3FF);
        check_keypad_irq();
        return;
    case kIf:
        set_raw(mem_.io, off, static_cast<std::uint16_t>(raw(mem_.io, off) & ~value));
        return;
    case kWaitcnt:
        set_raw(mem_.io, off, value & 0x5FFF);
        update_wait_states();
        return;
    case kIme:
        set_raw(mem_.io, off, value & 1);
        if ((value & 1) && (io_read16(kIe) & io_read16(kIf))) {
            request_tick();  // deliver at the next tick check
        }
        return;
    case kIe:
        set_raw(mem_.io, off, value & 0x3FFF);
        if (io_read16(kIme) & io_read16(kIe) & io_read16(kIf)) {
            request_tick();
        }
        return;
    case kPostflgHaltcnt:
        set_raw(mem_.io, off, static_cast<std::uint16_t>(value & 0x00FF));
        if (value & 0xFF00) {
            halt_until_irq((value & 0x8000) ? 0x3080 : 0x3FFF);  // HALTCNT: stop wakes on keypad/cart/serial only
        }
        return;
    case 0x028: case 0x02A: case 0x02C: case 0x02E:  // BG2X/BG2Y
    case 0x038: case 0x03A: case 0x03C: case 0x03E:  // BG3X/BG3Y
        set_raw(mem_.io, off, value);
        ppu_->reference_written(off);
        return;
    case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
        set_raw(mem_.io, off, value);
        dma_write_control(static_cast<int>((off - 0x0BA) / 12), value);
        return;
    case 0x100: case 0x104: case 0x108: case 0x10C:
        set_raw(mem_.io, off, value);
        timers_[(off - 0x100) / 4].reload = value;
        return;
    case 0x102: case 0x106: case 0x10A: case 0x10E:
        set_raw(mem_.io, off, value);
        timer_write_control(static_cast<int>((off - 0x102) / 4), value);
        return;
    default:
        set_raw(mem_.io, off, value);
        return;
    }
}

void System::set_keys(std::uint16_t pressed_mask) {
    set_raw(mem_.io, kKeyinput, static_cast<std::uint16_t>(~pressed_mask & 0x03FF));
    check_keypad_irq();
}

void System::check_keypad_irq() {
    const std::uint16_t keycnt = raw(mem_.io, kKeycnt);
    if (!(keycnt & 0x4000)) {
        return;
    }
    const std::uint16_t selected = keycnt & 0x03FF;
    const std::uint16_t pressed = static_cast<std::uint16_t>(~raw(mem_.io, kKeyinput) & selected);
    const bool all = (keycnt & 0x8000) != 0;
    if (selected != 0 && (all ? pressed == selected : pressed != 0)) {
        raise_irq(1u << 12);
    }
}

}  // namespace anygba
