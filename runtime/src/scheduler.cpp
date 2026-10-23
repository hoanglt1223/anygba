#include "anygba/system.hpp"

#include <algorithm>
#include <csetjmp>
#include <limits>

// Cycle timeline. cpu.cycles counts down from budget_ while code runs; when it reaches
// zero the code calls anygba_tick, which runs every event that is due and sets a new
// budget up to the next one. Video timing per GBATEK: 1232 cycles per line (HBlank from
// cycle 1006), 228 lines per frame, VBlank on lines 160-227.
namespace anygba {

namespace {
constexpr std::uint32_t kHblankStart = 1006;
constexpr std::uint32_t kLineCycles = 1232;
constexpr std::uint32_t kLines = 228;
constexpr std::int32_t kMaxBudget = 1 << 20;
}  // namespace

std::uint64_t System::now() const {
    return budget_start_ + static_cast<std::uint64_t>(static_cast<std::int64_t>(budget_) - cpu_.cycles);
}

void System::schedule(Event e, std::uint64_t at) { events_[static_cast<int>(e)] = at; }

void System::reschedule_budget() {
    const std::uint64_t t = now();
    const std::uint64_t next = *std::min_element(events_.begin(), events_.end());
    const std::uint64_t gap = next > t ? next - t : 0;
    budget_start_ = t;
    budget_ = static_cast<std::int32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>(gap, 1), kMaxBudget));
    cpu_.cycles = budget_;
}

void System::run_events() {
    for (;;) {
        const std::uint64_t t = now();
        const auto it = std::min_element(events_.begin(), events_.end());
        if (*it > t) {
            break;
        }
        const auto e = static_cast<Event>(it - events_.begin());
        const std::uint64_t at = *it;
        switch (e) {
        case Event::HBlank:
            schedule(Event::HBlank, at + kLineCycles);
            on_hblank();
            break;
        case Event::LineEnd:
            schedule(Event::LineEnd, at + kLineCycles);
            on_line_end();
            break;
        case Event::Timer0:
        case Event::Timer1:
        case Event::Timer2:
        case Event::Timer3:
            on_timer_overflow(static_cast<int>(e) - static_cast<int>(Event::Timer0));
            break;
        case Event::Count:
            break;
        }
    }
    reschedule_budget();
}

void System::on_hblank() {
    hblank_ = true;
    if (io_read16(0x004) & 0x10) {
        raise_irq(1u << 1);
    }
    if (line_ < 160) {
        dma_trigger(2);
    }
}

void System::on_line_end() {
    hblank_ = false;
    line_ = (line_ + 1) % kLines;
    const std::uint16_t dispstat = io_read16(0x004);
    if (line_ == 160) {
        if (dispstat & 0x08) {
            raise_irq(1u << 0);
        }
        dma_trigger(1);
        ++frames_;
        if (on_vblank) {
            on_vblank(*this);
        }
    }
    if ((dispstat & 0x20) && line_ == static_cast<std::uint32_t>(dispstat >> 8)) {
        raise_irq(1u << 2);
    }
}

void System::tick() {
    run_events();
    deliver_irq();
    if (stop_requested_ && top_jmp_) {
        std::longjmp(*static_cast<std::jmp_buf*>(top_jmp_), 2);
    }
}

void System::idle_loop() {
    if (on_idle_loop) {
        on_idle_loop(*this);
    }
    cpu_.cycles = 0;  // nothing changes until the next event: jump to it
    tick();
}

void System::halt_until_irq(std::uint16_t wake_mask) {
    // Skip whole budgets until an enabled interrupt is requested (IME is not required).
    while ((io_read16(0x200) & io_read16(0x202) & wake_mask) == 0) {
        cpu_.cycles = 0;
        run_events();
        if (stop_requested_ && top_jmp_) {
            std::longjmp(*static_cast<std::jmp_buf*>(top_jmp_), 2);
        }
    }
    request_tick();
}

void System::request_tick() {
    budget_start_ = now();
    budget_ = 0;
    cpu_.cycles = 0;
}

}  // namespace anygba

extern "C" {
void anygba_tick(anygba_cpu* c) { anygba::system_of(c).tick(); }
void anygba_idle_loop(anygba_cpu* c) { anygba::system_of(c).idle_loop(); }
}
