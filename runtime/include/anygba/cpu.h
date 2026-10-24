#ifndef ANYGBA_CPU_H
#define ANYGBA_CPU_H

/* CPU context and the helper ABI shared by generated C11 code, the fallback interpreter,
 * and the runtime. Generated code includes only this header. */

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anygba_cpu anygba_cpu;
typedef void (*anygba_fn)(anygba_cpu* c);

enum {
    ANYGBA_MODE_USR = 0x10,
    ANYGBA_MODE_FIQ = 0x11,
    ANYGBA_MODE_IRQ = 0x12,
    ANYGBA_MODE_SVC = 0x13,
    ANYGBA_MODE_ABT = 0x17,
    ANYGBA_MODE_UND = 0x1B,
    ANYGBA_MODE_SYS = 0x1F,
};

#define ANYGBA_RET_STACK 512

struct anygba_cpu {
    /* r[15] holds the address of the next instruction to run whenever control is outside
     * generated code (function exits, runtime calls). Inside generated code PC reads are
     * constants. */
    uint32_t r[16];
    uint32_t n, z, c, v; /* flags, each 0 or 1 */
    uint32_t t;          /* Thumb state */
    uint32_t ctrl;       /* CPSR bits 7..0 except T: I, F, mode */
    uint32_t spsr;       /* SPSR of the current mode (0 in USR/SYS) */
    int32_t cycles;      /* budget until the next scheduled event; anygba_tick when <= 0 */

    /* Banked copies of registers not currently live. Index: 0 USR/SYS, 1 FIQ, 2 IRQ,
     * 3 SVC, 4 ABT, 5 UND. */
    uint32_t bank_r8_12[2][5]; /* [0] every mode but FIQ, [1] FIQ */
    uint32_t bank_r13_14[6][2];
    uint32_t bank_spsr[6];

    /* Memory fast path by address bits 31..24: pointer to the backing store and the
     * mirror mask. NULL means call the bus. */
    uint8_t* read_page[256];
    uint32_t read_mask[256];
    uint8_t* write_page[256];
    uint32_t write_mask[256];

    /* Cycles per instruction fetch by region (address bits 27..24), [0] ARM, [1] Thumb.
     * Updated by the runtime when WAITCNT changes. */
    int32_t fetch_cost[2][16];

    /* Return addresses expected by active generated call sites, innermost last. */
    uint32_t ret_depth;
    uint32_t ret_stack[ANYGBA_RET_STACK];

    void* runtime; /* owned by the runtime */
};

/* ---- Runtime services (implemented by libanygba) ---- */

/* Slow-path memory access. Addresses are passed unaligned; the bus aligns them, except on
 * SRAM's 8-bit bus, which uses the exact byte address. */
uint32_t anygba_bus_read32(anygba_cpu* c, uint32_t addr);
uint32_t anygba_bus_read16(anygba_cpu* c, uint32_t addr);
uint32_t anygba_bus_read8(anygba_cpu* c, uint32_t addr);
void anygba_bus_write32(anygba_cpu* c, uint32_t addr, uint32_t value);
void anygba_bus_write16(anygba_cpu* c, uint32_t addr, uint32_t value);
void anygba_bus_write8(anygba_cpu* c, uint32_t addr, uint32_t value);

/* Advances the scheduler (video, timers, DMA, IRQ delivery). r[15] must name the next
 * instruction so an interrupt returns to the right place. */
void anygba_tick(anygba_cpu* c);
/* A branch to itself: nothing can change until the next event, so skip to it. */
void anygba_idle_loop(anygba_cpu* c);
/* SWI `number` (the BIOS function, comment field already decoded). r[15] = next. */
void anygba_swi(anygba_cpu* c, uint32_t number);
/* CPSR/SPSR writes. `mask` has bit0 c, bit1 x, bit2 s, bit3 f. Switches register banks. */
void anygba_msr(anygba_cpu* c, uint32_t value, uint32_t mask, uint32_t to_spsr);
/* CPSR = SPSR (data processing with S to PC, LDM with ^ and PC). */
void anygba_restore_cpsr(anygba_cpu* c);
/* LDM/STM with ^ and no PC: transfer the USR bank registers. */
void anygba_ldm_user(anygba_cpu* c, uint32_t addr, uint32_t list);
void anygba_stm_user(anygba_cpu* c, uint32_t addr, uint32_t list, uint32_t pc_value);
/* Called by generated code after a call returned somewhere other than `expected`.
 * Keeps running code (recompiled or interpreted) until r[15] == expected and returns 0,
 * or returns 1 when an outer call site expects r[15], in which case the caller must return. */
int anygba_resume(anygba_cpu* c, uint32_t expected);
/* Abandons every active generated frame and restarts dispatch at r[15]. */
void anygba_unwind(anygba_cpu* c);
/* Undefined instruction reached: reports and stops. */
void anygba_undefined(anygba_cpu* c, uint32_t addr, uint32_t raw);

/* ---- Inline helpers for generated code ---- */

/* `bx lr`: for hand-written replacement functions (per-game config [[replace]]). */
static inline void ag_return(anygba_cpu* c) {
    c->t = c->r[14] & 1u;
    c->r[15] = c->r[14] & ~1u;
}

static inline uint32_t ag_cpsr(const anygba_cpu* c) {
    return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | (c->t << 5) | c->ctrl;
}

static inline uint32_t ag_rotr(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

static inline uint32_t ag_rd32(anygba_cpu* c, uint32_t a) {
    const uint8_t* p = c->read_page[a >> 24];
    if (p) {
        uint32_t v;
        memcpy(&v, p + (a & c->read_mask[a >> 24] & ~3u), 4);
        return v;
    }
    return anygba_bus_read32(c, a);
}

static inline uint32_t ag_rd16(anygba_cpu* c, uint32_t a) {
    const uint8_t* p = c->read_page[a >> 24];
    if (p) {
        uint16_t v;
        memcpy(&v, p + (a & c->read_mask[a >> 24] & ~1u), 2);
        return v;
    }
    return anygba_bus_read16(c, a);
}

static inline uint32_t ag_rd8(anygba_cpu* c, uint32_t a) {
    const uint8_t* p = c->read_page[a >> 24];
    if (p) {
        return p[a & c->read_mask[a >> 24]];
    }
    return anygba_bus_read8(c, a);
}

static inline void ag_wr32(anygba_cpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_page[a >> 24];
    if (p) {
        memcpy(p + (a & c->write_mask[a >> 24] & ~3u), &v, 4);
        return;
    }
    anygba_bus_write32(c, a, v);
}

static inline void ag_wr16(anygba_cpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_page[a >> 24];
    if (p) {
        const uint16_t h = (uint16_t)v;
        memcpy(p + (a & c->write_mask[a >> 24] & ~1u), &h, 2);
        return;
    }
    anygba_bus_write16(c, a, v & 0xFFFFu);
}

static inline void ag_wr8(anygba_cpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_page[a >> 24];
    if (p) {
        p[a & c->write_mask[a >> 24]] = (uint8_t)v;
        return;
    }
    anygba_bus_write8(c, a, v & 0xFFu);
}

/* ARM7TDMI load quirks: misaligned LDR rotates the aligned word; misaligned LDRH rotates
 * the aligned halfword; misaligned LDRSH loads a sign-extended byte. */
static inline uint32_t ag_ldr(anygba_cpu* c, uint32_t a) { return ag_rotr(ag_rd32(c, a), (a & 3u) * 8u); }
static inline uint32_t ag_ldrh(anygba_cpu* c, uint32_t a) { return ag_rotr(ag_rd16(c, a), (a & 1u) * 8u); }
static inline uint32_t ag_ldrsb(anygba_cpu* c, uint32_t a) { return (uint32_t)(int32_t)(int8_t)ag_rd8(c, a); }
static inline uint32_t ag_ldrsh(anygba_cpu* c, uint32_t a) {
    if (a & 1u) {
        return (uint32_t)(int32_t)(int8_t)ag_rd8(c, a);
    }
    return (uint32_t)(int32_t)(int16_t)ag_rd16(c, a);
}

static inline void ag_nz(anygba_cpu* c, uint32_t r) {
    c->n = r >> 31;
    c->z = r == 0;
}

/* a + b + carry with all four flags; SUB/CMP pass (a, ~b, 1), SBC passes (a, ~b, C). */
static inline uint32_t ag_adc_flags(anygba_cpu* c, uint32_t a, uint32_t b, uint32_t carry) {
    const uint64_t wide = (uint64_t)a + b + carry;
    const uint32_t r = (uint32_t)wide;
    c->c = (uint32_t)(wide >> 32);
    c->v = (~(a ^ b) & (a ^ r)) >> 31;
    ag_nz(c, r);
    return r;
}

/* Register-specified shifts (amount = Rs & 0xFF), with and without carry-out. */
static inline uint32_t ag_lsl(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v << n; }
static inline uint32_t ag_lsr(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v >> n; }
static inline uint32_t ag_asr(uint32_t v, uint32_t n) { return (uint32_t)((int32_t)v >> (n >= 32 ? 31 : n)); }
static inline uint32_t ag_ror(uint32_t v, uint32_t n) { return ag_rotr(v, n); }

static inline uint32_t ag_lsl_c(uint32_t v, uint32_t n, uint32_t* carry) {
    if (n == 0) return v;
    if (n < 32) { *carry = (v >> (32 - n)) & 1u; return v << n; }
    *carry = n == 32 ? (v & 1u) : 0;
    return 0;
}
static inline uint32_t ag_lsr_c(uint32_t v, uint32_t n, uint32_t* carry) {
    if (n == 0) return v;
    if (n < 32) { *carry = (v >> (n - 1)) & 1u; return v >> n; }
    *carry = n == 32 ? (v >> 31) : 0;
    return 0;
}
static inline uint32_t ag_asr_c(uint32_t v, uint32_t n, uint32_t* carry) {
    if (n == 0) return v;
    if (n < 32) { *carry = ((uint32_t)((int32_t)v >> (n - 1))) & 1u; return (uint32_t)((int32_t)v >> n); }
    *carry = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ag_ror_c(uint32_t v, uint32_t n, uint32_t* carry) {
    if (n == 0) return v;
    n &= 31;
    if (n == 0) { *carry = v >> 31; return v; }
    *carry = (v >> (n - 1)) & 1u;
    return ag_rotr(v, n);
}

#define AG_SPEND(cpu, region, insns, extra) \
    ((cpu)->cycles -= (insns) * (cpu)->fetch_cost[(cpu)->t][(region)] + (extra))

#define AG_TICK(cpu, next_pc)              \
    do {                                   \
        if ((cpu)->cycles <= 0) {          \
            (cpu)->r[15] = (next_pc);      \
            anygba_tick(cpu);              \
        }                                  \
    } while (0)

/* Direct call: `fn` returns with r[15] = where execution continues. `ret` stays on the
   return stack while resuming, so code the interpreter runs for the callee (a veneer's
   target, say) stops at the return address instead of running on past it. */
#define AG_CALL(cpu, fn, ret)                                                 \
    do {                                                                      \
        int unwind_ = 0;                                                      \
        if ((cpu)->ret_depth >= ANYGBA_RET_STACK) anygba_unwind(cpu);         \
        (cpu)->ret_stack[(cpu)->ret_depth++] = (ret);                         \
        fn(cpu);                                                              \
        if ((cpu)->r[15] != (ret)) unwind_ = anygba_resume((cpu), (ret));     \
        (cpu)->ret_depth--;                                                   \
        if (unwind_) return;                                                  \
    } while (0)

/* Indirect call: r[15] already holds the target. */
#define AG_ICALL(cpu, ret)                                                    \
    do {                                                                      \
        int unwind_;                                                          \
        if ((cpu)->ret_depth >= ANYGBA_RET_STACK) anygba_unwind(cpu);         \
        (cpu)->ret_stack[(cpu)->ret_depth++] = (ret);                         \
        unwind_ = anygba_resume((cpu), (ret));                                \
        (cpu)->ret_depth--;                                                   \
        if (unwind_) return;                                                  \
    } while (0)

/* Generated program description, emitted by anygba-recomp into dispatch.c. */
typedef struct anygba_function_entry {
    uint32_t addr;
    uint32_t thumb;
    anygba_fn fn;
} anygba_function_entry;

typedef struct anygba_game {
    const char* title;
    const char* rom_sha256; /* hex of the ROM the code was generated from */
    uint32_t rom_size;
    const anygba_function_entry* functions; /* sorted by addr */
    uint32_t function_count;
    const char* save_type; /* per-game config override, or "" to detect from the ROM */
} anygba_game;

#ifdef __cplusplus
}
#endif

#endif
