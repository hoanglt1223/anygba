#include "anygba/system.hpp"

// Timers 0-3 (GBATEK "Timers"). A running timer is stored as (counter, start cycle) and
// read lazily; only its overflow is scheduled. Count-up timers tick on the previous
// timer's overflow instead of the clock.
namespace anygba {

namespace {

constexpr int kShift[4] = {0, 6, 8, 10};  // prescaler 1, 64, 256, 1024

bool running(const Timer& t) { return (t.control & 0x80) != 0; }

}  // namespace

std::uint16_t System::timer_counter(int n) const {
    const Timer& t = timers_[n];
    if (!running(t) || (n > 0 && (t.control & 4))) {
        return t.counter;
    }
    const std::uint64_t ticks = (now() - t.started) >> kShift[t.control & 3];
    return static_cast<std::uint16_t>(t.counter + ticks);
}

void System::schedule_timer(int n) {
    const auto e = static_cast<Event>(static_cast<int>(Event::Timer0) + n);
    const Timer& t = timers_[n];
    if (!running(t) || (n > 0 && (t.control & 4))) {
        schedule(e, UINT64_MAX);
        return;
    }
    const std::uint64_t remaining = (0x10000u - t.counter) << kShift[t.control & 3];
    schedule(e, t.started + remaining);
    reschedule_budget();
}

void System::timer_write_control(int n, std::uint16_t value) {
    Timer& t = timers_[n];
    t.counter = timer_counter(n);
    const bool was_running = running(t);
    t.control = static_cast<std::uint16_t>(value & 0xC7);
    t.started = now();
    if (!was_running && running(t)) {
        t.counter = t.reload;
    }
    schedule_timer(n);
}

void System::on_timer_overflow(int n) {
    Timer& t = timers_[n];
    const auto e = static_cast<Event>(static_cast<int>(Event::Timer0) + n);
    if (!(n > 0 && (t.control & 4))) {
        // Restart from reload at the exact overflow time so the period does not drift.
        t.started = events_[static_cast<int>(e)];
    }
    t.counter = t.reload;
    if (t.control & 0x40) {
        raise_irq(static_cast<std::uint16_t>(1u << (3 + n)));
    }
    if (n < 3) {
        Timer& next = timers_[n + 1];
        if (running(next) && (next.control & 4)) {
            if (++next.counter == 0) {
                on_timer_overflow(n + 1);
            }
        }
    }
    schedule_timer(n);
}

}  // namespace anygba
