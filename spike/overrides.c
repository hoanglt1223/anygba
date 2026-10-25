/* Spike: hand-written replacement for Emerald's StringCopy (u8 *StringCopy(u8 *dest, const u8 *src)),
 * written from Ghidra's disassembly/pseudo-C of the ROM. Strings end with 0xFF.
 *
 * It reproduces everything the Thumb original leaves behind, so a differential test can compare
 * the whole CPU state: the `push {lr}` stack slot, r1 = the popped LR, r2 = 0xFF, r3 = r0 = the
 * terminator's address, the flags of the last `adds r0, r3, #0`, and the cycle budget.
 *
 * Spike instrumentation (not part of an override's contract):
 *   - counts calls reached from generated code (the call site pushed LR onto ret_stack) and from
 *     the interpreter or dispatcher (it did not); printed to stderr at exit;
 *   - ANYGBA_SPIKE_RECORD=<file>: appends each call's dest, src and source bytes for the
 *     recorded-state difftest. */
#include <anygba/cpu.h>

#include <stdio.h>
#include <stdlib.h>

static unsigned long long calls_generated;
static unsigned long long calls_other;
static FILE* record_file;
static int initialised;

static void report(void) {
    fprintf(stderr, "[override] StringCopy calls: generated=%llu interpreter/dispatch=%llu\n", calls_generated,
            calls_other);
    if (record_file) fclose(record_file);
}

static void init_once(void) {
    initialised = 1;
    const char* path = getenv("ANYGBA_SPIKE_RECORD");
    if (path && *path) {
        record_file = fopen(path, "ab");
        if (!record_file) fprintf(stderr, "[override] cannot open %s\n", path);
    }
    atexit(report);
}

static void record_call(anygba_cpu* c) {
    unsigned char bytes[1024];
    unsigned n = 0;
    while (n < sizeof bytes) {
        bytes[n] = (unsigned char)ag_rd8(c, c->r[1] + n);
        if (bytes[n++] == 0xFF) break;
    }
    const unsigned header[3] = {c->r[0], c->r[1], n};
    fwrite(header, sizeof header, 1, record_file);
    fwrite(bytes, 1, n, record_file);
}

void ov_StringCopy(anygba_cpu* c) {
    if (!initialised) init_once();
    const uint32_t lr = c->r[14] & ~1u;
    if (c->ret_depth > 0 && c->ret_stack[c->ret_depth - 1] == lr) {
        ++calls_generated;
    } else {
        ++calls_other;
    }
    if (record_file) record_call(c);

    /* The cycle budget is spent and checked at the same points as in the translation (each block
     * end), so interrupts land at the same moments of a long copy. */
    const int32_t fetch = c->fetch_cost[1][8];
    const uint32_t sp = c->r[13];
    uint32_t dest = c->r[0];
    uint32_t src = c->r[1];
    ag_wr32(c, sp - 4u, c->r[14]);
    c->cycles -= 3 * fetch + 1;
    AG_TICK(c, 0x08008ba4u);
    for (;;) {
        const uint32_t ch = ag_rd8(c, src);
        c->cycles -= 4 * fetch + 1;
        AG_TICK(c, 0x08008bb2u);
        if (ch == 0xFFu) break;
        ag_wr8(c, dest, ch);
        ++dest;
        ++src;
        c->cycles -= 3 * fetch;
        AG_TICK(c, 0x08008bacu);
    }
    ag_wr8(c, dest, 0xFFu);

    const uint32_t ret = ag_rd32(c, sp - 4u);
    c->r[0] = dest;
    c->r[1] = ret;
    c->r[2] = 0xFFu;
    c->r[3] = dest;
    c->n = dest >> 31;
    c->z = dest == 0;
    c->c = 0;
    c->v = 0;

    c->cycles -= 5 * fetch + 1;
    AG_TICK(c, 0x08008bbcu);
    c->t = ret & 1u;
    c->r[15] = ret & (c->t ? ~1u : ~3u);
}
