#include "bios/bios.h"
#include "apu/apu.h"
#include "ppu/ppu.h"
#include "rtc.h"

#include <algorithm>
#include <cstring>

// Power-on state as the BIOS leaves it when it enters the cartridge, plus the two BIOS
// reset functions (GBATEK "BIOS Reset Functions").
namespace anygba {

namespace {

constexpr std::uint32_t kSpSys = 0x03007F00;
constexpr std::uint32_t kSpIrq = 0x03007FA0;
constexpr std::uint32_t kSpSvc = 0x03007FE0;
constexpr int kBankIrq = 2;
constexpr int kBankSvc = 3;

void io_zero(System& sys, std::uint32_t first, std::uint32_t end) {
    for (std::uint32_t off = first; off < end; off += 2) {
        sys.io_write16(off, 0);
    }
}

void identity_affine(System& sys) {
    for (const std::uint32_t base : {0x020u, 0x030u}) {  // BG2PA.., BG3PA..
        sys.io_write16(base, 0x100);
        sys.io_write16(base + 2, 0);
        sys.io_write16(base + 4, 0);
        sys.io_write16(base + 6, 0x100);
    }
}

}  // namespace

void System::reset() {
    void* const runtime = this;
    std::memset(&cpu_, 0, sizeof cpu_);
    cpu_.runtime = runtime;
    cpu_.rom_patched = rom_patches_.empty() ? 0 : 1;  // cheats outlive a reset
    map_pages();
    update_wait_states();  // WAITCNT is 0 at power-on; games that never write it still pay

    std::fill(mem_.ewram.begin(), mem_.ewram.end(), 0);
    std::fill(mem_.iwram.begin(), mem_.iwram.end(), 0);
    std::fill(mem_.io.begin(), mem_.io.end(), 0);
    std::fill(mem_.palette.begin(), mem_.palette.end(), 0);
    std::fill(mem_.vram.begin(), mem_.vram.end(), 0);
    std::fill(mem_.oam.begin(), mem_.oam.end(), 0);

    dma_ = {};
    timers_ = {};
    line_ = 0;
    hblank_ = false;
    frames_ = 0;
    halted_ = false;
    in_irq_ = false;
    budget_start_ = 0;
    budget_ = 0;
    events_.fill(UINT64_MAX);
    schedule(Event::HBlank, 1006);
    schedule(Event::LineEnd, 1232);
    reschedule_budget();

    apu_->reset();
    bios::boot_state(*this);
    ppu_->reset();
    last_bios_opcode_ = bios::kOpcodeAfterBoot;
    if (rtc_) {
        rtc_->reset();
        show_gpio();
    }
}

namespace bios {

void spend(System& sys, std::int64_t cycles) {
    sys.cpu().cycles -= static_cast<std::int32_t>(std::min<std::int64_t>(cycles, 1 << 24));
}

void boot_state(System& sys) {
    anygba_cpu& c = sys.cpu();
    c.bank_r13_14[0][0] = kSpSys;
    c.bank_r13_14[kBankIrq][0] = kSpIrq;
    c.bank_r13_14[kBankSvc][0] = kSpSvc;
    c.ctrl = ANYGBA_MODE_SYS;
    c.t = 0;
    c.r[13] = kSpSys;
    c.r[15] = 0x08000000;

    sys.io_write16(0x000, 0x0080);  // DISPCNT: forced blank
    identity_affine(sys);
    sys.io_write16(0x088, 0x0200);  // SOUNDBIAS
    sys.io_write16(0x134, 0x8000);  // RCNT
    sys.io_write16(0x300, 0x0001);  // POSTFLG: booted
    sys.set_keys(0);
}

void soft_reset(System& sys) {
    anygba_cpu& c = sys.cpu();
    std::vector<std::uint8_t>& iwram = sys.memory().iwram;
    const bool to_ram = iwram[0x7FFA] != 0;
    std::fill(iwram.end() - 0x200, iwram.end(), 0);

    sys.set_cpsr(ANYGBA_MODE_SYS);
    for (int r = 0; r < 13; ++r) {
        c.r[r] = 0;
    }
    c.r[13] = kSpSys;
    c.r[14] = 0;
    c.bank_r13_14[kBankIrq][0] = kSpIrq;
    c.bank_r13_14[kBankIrq][1] = 0;
    c.bank_spsr[kBankIrq] = 0;
    c.bank_r13_14[kBankSvc][0] = kSpSvc;
    c.bank_r13_14[kBankSvc][1] = 0;
    c.bank_spsr[kBankSvc] = 0;
    c.r[15] = to_ram ? 0x02000000 : 0x08000000;
}

void register_ram_reset(System& sys, std::uint32_t flags) {
    Memory& m = sys.memory();
    if (flags & 0x01) std::fill(m.ewram.begin(), m.ewram.end(), 0);
    if (flags & 0x02) std::fill(m.iwram.begin(), m.iwram.end() - 0x200, 0);
    if (flags & 0x04) std::fill(m.palette.begin(), m.palette.end(), 0);
    if (flags & 0x08) std::fill(m.vram.begin(), m.vram.end(), 0);
    if (flags & 0x10) std::fill(m.oam.begin(), m.oam.end(), 0);
    if (flags & 0x20) {  // serial: general-purpose mode
        io_zero(sys, 0x120, 0x12C);
        sys.io_write16(0x134, 0x8000);
        io_zero(sys, 0x140, 0x142);
        io_zero(sys, 0x150, 0x15A);
    }
    if (flags & 0x40) {  // sound, including wave RAM
        io_zero(sys, 0x060, 0x084);
        io_zero(sys, 0x090, 0x0A0);
        sys.io_write16(0x088, 0x0200);
    }
    if (flags & 0x80) {  // everything else
        io_zero(sys, 0x004, 0x058);
        io_zero(sys, 0x0B0, 0x0E0);
        io_zero(sys, 0x100, 0x110);
        sys.io_write16(0x132, 0);
        sys.io_write16(0x200, 0);
        sys.io_write16(0x202, 0xFFFF);  // IF: write 1 to clear
        sys.io_write16(0x204, 0);
        sys.io_write16(0x208, 0);
        identity_affine(sys);
    }
    sys.io_write16(0x000, 0x0080);
    spend(sys, 400 + ((flags & 1) ? 0x40000 / 8 : 0) + ((flags & 8) ? 0x18000 / 8 : 0));
}

}  // namespace bios
}  // namespace anygba
