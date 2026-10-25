#pragma once

// Guest API for function overrides: C++ that replaces one guest function of a recompiled game
// (docs/dev/OVERRIDES.md). An override sees the guest through Guest only: registers, guest
// memory, calls into other guest functions, cycle accounting and a few host services.
//
//   #include <anygba/override.hpp>
//
//   ANYGBA_OVERRIDE(my_engine_StrLen, g) {
//       std::uint32_t p = g.arg(0);
//       while (g.read<std::uint8_t>(p) != 0xFF) ++p;
//       g.set_result(p - g.arg(0));
//       g.ret();
//   }
//
// with [[override]] symbol = "StrLen", impl = "my_engine_StrLen" in the module's overrides.toml.

#include "anygba/cpu.h"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace anygba::guest {

// Condition flags as the ARM CPSR holds them.
struct Flags {
    bool n = false;
    bool z = false;
    bool c = false;
    bool v = false;
};

class Guest {
public:
    // Built by the runtime for each call; overrides receive it by reference.
    Guest(anygba_cpu* cpu, std::uint32_t entry) : c_(cpu), entry_(entry) {}
    Guest(const Guest&) = delete;
    Guest& operator=(const Guest&) = delete;

    // Address of the overridden function (without the Thumb bit).
    std::uint32_t entry() const { return entry_; }

    // ---- registers ----
    // r0-r15; r13 is sp, r14 lr. r15 is not meaningful while an override runs.
    std::uint32_t& reg(unsigned i) { return c_->r[i & 15u]; }
    std::uint32_t reg(unsigned i) const { return c_->r[i & 15u]; }
    // Argument i of the AAPCS call: r0-r3, then the words at sp, sp + 4, ...
    std::uint32_t arg(unsigned i) { return i < 4 ? c_->r[i] : read<std::uint32_t>(c_->r[13] + 4u * (i - 4u)); }
    void set_result(std::uint32_t value) { c_->r[0] = value; }
    Flags flags() const { return {c_->n != 0, c_->z != 0, c_->c != 0, c_->v != 0}; }
    void set_flags(Flags f) {
        c_->n = f.n;
        c_->z = f.z;
        c_->c = f.c;
        c_->v = f.v;
    }
    // N and Z from a result, as a flag-setting move or add leaves them (C and V untouched).
    void set_nz(std::uint32_t result) { ag_nz(c_, result); }

    // ---- guest memory (the same bus as translated code: mirrors, IO, open bus) ----
    // T is std::uint8_t, std::uint16_t, std::uint32_t or a signed variant. Addresses are aligned
    // down to the access size, as the hardware does for plain stores (and loads, minus rotation).
    template <typename T>
    T read(std::uint32_t addr) {
        static_assert(std::is_integral_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4));
        if constexpr (sizeof(T) == 1) {
            return static_cast<T>(ag_rd8(c_, addr));
        } else if constexpr (sizeof(T) == 2) {
            return static_cast<T>(ag_rd16(c_, addr & ~1u));
        } else {
            return static_cast<T>(ag_rd32(c_, addr & ~3u));
        }
    }
    template <typename T>
    void write(std::uint32_t addr, T value) {
        static_assert(std::is_integral_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4));
        if constexpr (sizeof(T) == 1) {
            ag_wr8(c_, addr, static_cast<std::uint8_t>(value));
        } else if constexpr (sizeof(T) == 2) {
            ag_wr16(c_, addr & ~1u, static_cast<std::uint16_t>(value));
        } else {
            ag_wr32(c_, addr & ~3u, static_cast<std::uint32_t>(value));
        }
    }

    // ---- cycle accounting ----
    // Translated code charges each basic block's cycles at its end and then lets due events
    // (video, timers, interrupts) run. An override that must match the original frame for frame
    // does the same: one block() per block of the original, with that block's instruction count,
    // extra cycles (memory waits, multiplies) and the address execution would continue at.
    // Interrupt handlers may run inside block().
    void block(std::uint32_t next_pc, int insns, int extra = 0) {
        c_->cycles -= insns * fetch_cycles() + extra;
        if (c_->cycles <= 0) {
            c_->r[15] = next_pc;
            anygba_tick(c_);
        }
    }
    // Cycles one instruction fetch from the overridden function's memory costs now.
    int fetch_cycles() const { return c_->fetch_cost[c_->t][(entry_ >> 24) & 15u]; }

    // ---- calls ----
    // Calls the guest function at `target` (bit 0 set: Thumb) the way `bl`/`blx` would, with
    // lr = `return_addr` (bit 0 set: return to Thumb code). Pass the return address the original's
    // call instruction would leave, since callees may store lr in guest memory. The callee runs
    // recompiled when it can and in the interpreter otherwise. Returns false when it did not come
    // back (it returned to an outer caller instead); the override must then return at once,
    // without ret().
    [[nodiscard]] bool call(std::uint32_t target, std::uint32_t return_addr);
    // Runs the translation of the overridden function itself, which returns to the caller: the
    // override is done afterwards and must not call ret().
    void run_original();

    // ---- return ----
    // `bx lr`.
    void ret() { ret_to(c_->r[14]); }
    // `bx addr`: continue at addr (bit 0 selects Thumb).
    void ret_to(std::uint32_t addr) {
        c_->t = addr & 1u;
        c_->r[15] = addr & (c_->t ? ~1u : ~3u);
        returned_ = true;
    }
    bool returned() const { return returned_; }

    // ---- host services ----
    // A short on-screen message (also printed in headless runs). Ignored when no player runs.
    void osd(std::string_view text);

    // ---- diagnostics ----
    // True when a call site in recompiled code (a direct call or an indirect one) waits for this
    // call to return; false when the interpreter, an interrupt or the dispatcher started it.
    bool from_recompiled_code() const {
        return c_->ret_depth > 0 && c_->ret_stack[c_->ret_depth - 1] == (c_->r[14] & ~1u);
    }

private:
    anygba_cpu* c_;
    std::uint32_t entry_;
    bool returned_ = false;
};

using Impl = void (*)(Guest&);

// Adds `impl` under `name` to the process-wide registry; ANYGBA_OVERRIDE calls it during static
// initialisation. A name registered twice is reported when the game starts.
bool register_impl(const char* name, Impl impl) noexcept;

}  // namespace anygba::guest

// Defines and registers an override: ANYGBA_OVERRIDE(impl_name, g) { ... uses Guest& g ... }.
// impl_name is what [[override]] impl = "..." names.
#define ANYGBA_OVERRIDE(impl_name, param)                                                        \
    static void anygba_override_##impl_name(::anygba::guest::Guest& param);                       \
    [[maybe_unused]] static const bool anygba_override_registered_##impl_name =                   \
        ::anygba::guest::register_impl(#impl_name, &anygba_override_##impl_name);                 \
    static void anygba_override_##impl_name(::anygba::guest::Guest& param)
