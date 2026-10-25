# Function overrides

An override replaces one guest function of a recompiled game with C++ written against a small guest
API. The rest of the game stays recompiled. Overrides serve two purposes: readable rewrites of the
functions that matter for enhancements (text printers, sprite builders, HUDs), and engine-wide work
that one module does for several games (the Gen 3 Pokemon engine, for example).

`[[replace]]` ([GAME-CONFIG.md](GAME-CONFIG.md)) is the older, lower-level form: a C function that takes
the raw `anygba_cpu*`, always on, addressed by number. Overrides add names, symbols, C++, the guest API,
`--overrides off` and startup checks.

## Modules

A module is a folder with an `overrides.toml` and the `.cpp` (or `.c`) files next to it:

```
overrides/pokemon-gen3/
  overrides.toml
  string_copy.cpp
```

The repository keeps engine and game modules under `overrides/<engine-or-game>/` (installed to
`share/anygba/overrides/`). Your own modules can live anywhere. Build a game with one or more of them:

```
anygba build game.gba --overrides overrides/pokemon-gen3 [--overrides D:/my-mods/emerald-hud]
anygba-recomp game.gba --overrides DIR --symbols compat/symbols/BPEE.toml -o gen   # the recompiler alone
```

`anygba build` compiles every source of the module into the game. Changing the set of overridden
functions needs a new build; changing an override's code only recompiles that file. Modules may not
contain ROM data or code taken from the game.

## Configuration

`overrides.toml` holds only `[[override]]` entries. A game config (`--config game.toml`) may hold them too,
next to its own `game.sources`.

```toml
[[override]]
symbol = "StringCopy"              # a name from a [functions] table, or:
# addr = 0x08008BA0                #   the entry address (no Thumb bit) ...
# mode = "thumb"                   #   ... and its instruction set
impl = "pokemon_gen3_StringCopy"   # the name given to ANYGBA_OVERRIDE
enabled = true                     # optional (default true); false: compiled in, off unless --overrides all
engine = "pokemon-gen3"            # optional tag, shown in the list of overrides
verified = true                    # optional (default false): a difftest passed (see below)
```

Symbols resolve through `[functions]` tables, so an engine module names functions and each game supplies
the addresses. The recompiler reads the table from the game config, from `--symbols FILE`, and (with
`anygba build`) from the symbols file shipped for the ROM's game code (`compat/symbols/<code>.toml`,
installed to `share/anygba/symbols/`). Tables that name the same function differently are an error.

```toml
[functions]
StringCopy = { addr = 0x08008BA0, mode = "thumb" }
```

The recompiler rejects: an entry with both or neither of `addr` and `symbol`, an unknown symbol, two
overrides of one address, an override and a `[[replace]]` of one address, an address where no function
starts or whose instruction set differs, and two sources with the same file name.

## Writing an override

```cpp
#include <anygba/override.hpp>

ANYGBA_OVERRIDE(pokemon_gen3_StringCopy, g) {
    std::uint32_t dest = g.arg(0), src = g.arg(1);
    ...
    g.ret_to(return_address);
}
```

The guest API (`runtime/include/anygba/override.hpp`, class `anygba::guest::Guest`):

| Group | Members |
|---|---|
| Registers | `reg(i)`, `arg(i)` (r0-r3, then the stack), `set_result(v)`, `flags()`, `set_flags(f)`, `set_nz(v)` |
| Memory | `read<T>(addr)`, `write<T>(addr, v)` for 8/16/32-bit integers, through the same bus as translated code |
| Cycles | `block(next_pc, insns, extra)`, `fetch_cycles()` |
| Calls | `call(target, return_addr)`, `run_original()` |
| Return | `ret()` (`bx lr`), `ret_to(addr)` (`bx addr`) |
| Host | `osd(text)`: an on-screen message (printed in headless runs) |
| Diagnostics | `entry()`, `from_recompiled_code()` |

Rules the runtime relies on:

- Every override ends with `ret()`, `ret_to()` or `run_original()`, or returns right after a `call()` that
  returned false. Otherwise the game stops with "finished without ret(), ret_to() or run_original()".
- `call()` sets lr like `bl`. Keep the incoming lr (`g.reg(14)`) if you return with it later, and pass the
  return address the original's call instruction would leave: callees can save lr in guest memory.
  The callee runs recompiled when it can, else in the interpreter.
- `call()` and `block()` may run interrupt handlers, and a stuck callee may abandon the host stack
  (longjmp) the way generated code does. Do not keep objects with destructors alive across them.
- Overrides are process-wide, like the generated code, and must not keep pointers into guest memory.

### Timing

Translated code charges each basic block's cycles at the block's end and then lets due events run
(video, timers, DMA, interrupts). An override that must look identical to the original, frame for frame,
does the same with `block()`: one call per block of the original, with its instruction count, extra
cycles and the address execution continues at. Charging the whole function once at the end changes
where interrupts land in long loops: on Emerald's StringCopy that differed from the original in 1,205
of 10,020 random states. `overrides/pokemon-gen3/string_copy.cpp` is the reference: its comments list the
original's blocks.

Overrides that change behaviour on purpose (drawing text through the host, skipping work) do not need
exact timing, but the frame-hash checkpoints of the game then change too.

## How calls reach an override

For each overridden function the recompiler keeps the translation as `f_XXXXXXXX_orig` and emits
`f_XXXXXXXX` as a switch: it runs the override while the runtime has it on, else the translation.
Every call path enters `f_XXXXXXXX`: direct calls from generated code, indirect calls and returns
through the dispatcher, and calls from interpreted code (the interpreter stops at compiled entries).
Games without overrides get the same generated code as before. A ROM patch cheat over an overridden
function is honoured only where the dispatcher starts the function (it interprets the patched code);
direct calls from generated code still run the override.

At startup the game checks that every override has a registered implementation and stops with the
override's name, address and the missing implementation otherwise (an implementation registered twice
is an error too). It then prints the list:

```
override: StringCopy at 0x08008ba0 thumb -> pokemon_gen3_StringCopy [pokemon-gen3]: on
```

`--overrides off` runs the original code everywhere, for A/B comparison; `--overrides all` also switches
on the entries with `enabled = false`. The menu does not list overrides yet. An active override without
`verified = true` also prints `warning: override ... is not verified`.

## Differential tests

An override is trusted when it behaves like the translation it replaces. `anygba difftest` runs both from
identical start states inside the generated game and compares what they leave behind:

```
anygba difftest <game program or its folder> --func StringCopy --overrides overrides/pokemon-gen3 \
    [--load-state FILE] [--recorded FILE]... [--states 10000] [--seed 1] [--json report.json] [--mark-verified]
```

It is a front end for the game's own `--difftest FUNC` mode (`<game> --help`), which holds both the
translation (`f_XXXXXXXX_orig`) and the override. Exit code 0: every state passed the gate; 1: one did not.

**What is compared.** The gate is the ABI-level comparison: r0, r4-r11, sp, where the function returns
to (address and instruction set), every memory write (address, size and value; in order per address, and
in global order for IO registers), the memory contents afterwards, and the guest functions it called with
r0-r3. Everything else is reported separately and does not fail the test: r1-r3, r12, lr, the flags, the
mode bits, cycles used, scheduler ticks, the order of writes across addresses, the lr of each call, and
the dead stack (`dead_stack` bytes below the entry sp, where `push {lr}` leaves the return address).

**How a state runs.** The machine state (the `--load-state` save state, else the reset state) is
restored, the state's memory is planted and its registers loaded, and the function is called from host
code. Ticks are isolated: when the cycle budget runs out the runtime adds one scanline (1,232 cycles) and
counts a tick instead of running video, timers, DMA or interrupts, so both sides see the same machine
however long they take. A run past `tick_limit` ticks stops with a timeout; when both sides stop the same
way the state counts as passed and the report lists how many did, per shape. Code that waits for an
interrupt (BIOS Halt, Stop, IntrWait) stops with an error: such functions need a recorded state that does
not reach the wait. Calls are seen at the entries of generated functions (all of them are breakpoints
during a difftest) and of the interpreter; writes through the bus (every region is watched). BIOS calls
and DMA that write memory directly show up in the memory comparison, not as writes.

**Random states** come from the argument spec, `difftest.toml` next to the module's `overrides.toml`
(or `--spec FILE`): one table per function, named like the override (`[StringCopy]`, or `["0x08000186"]`
for one given by address), with weighted shapes:

```toml
[StringCopy]
dead_stack = 64                  # default 256
tick_limit = 200                 # default 10000
ignore_memory = [[0x03007ff0, 0x03008000]]   # optional [begin, end) ranges compared nowhere; listed in the report

[[StringCopy.shape]]
name = "ram-src"
weight = 2                       # default 1
r0 = "ram"                       # a pointer into ewram/iwram ("ewram", "iwram", "io", "palette", "vram", "oam", "rom", or a list)
r1 = { string = "ram", length = [0, 300] }    # planted random bytes ending in terminator (default 0xFF)

[[StringCopy.shape]]
name = "overlap"
r1 = { string = "ram", length = [0, 300] }
r0 = { near = "r1", offset = [-8, 0] }        # relative to another register (not itself near one)
cycles = { range = [0, 400] }    # the cycle budget at entry (default 1000000: no tick)
```

Value forms for `r0`-`r12`, `sp`, `lr` and `cycles`: an integer; `"any"` (the default for r0-r12); a
region or list of regions (a pointer); or a table with one of `value`, `range = [lo, hi]`, `pointer = ...`,
`string = ... (length, terminator, bytes = [lo, hi])`, `buffer = ... (size = [min, max], bytes)`,
`near = "reg" (offset)`, plus `align` (a power of two) and `thumb = true` (set bit 0). Defaults: sp in
0x03007A00-0x03007F00 (word aligned), lr a Thumb address in the ROM, random flags. Strings and buffers are
planted in writable memory only, at an address chosen for their longest length.

**Recorded states** are real calls. During a scripted headless run:

```
<game> --headless --no-save --load-state FILE --keys ... --frames N \
    --record-entry StringCopy --count 200 --record-out stringcopy.states
```

stores the machine state at the first call and, for every call, the registers and the memory that differs
from it; the run stops after `--count` states. Keep these files outside the repository: they hold game
memory. `--recorded FILE` adds them to a difftest; their first-call state is the base for them.

**Coverage.** The recompiler gives the translation of every overridden function block counters
(`f_XXXXXXXX_hits`, `_blocks`): blocks are the entry, branch targets and return points, and the
fall-through of a conditional branch. The report gives the share of blocks the original ran over the test
set and lists the ones it never reached, so gaps in the spec are visible. Games without overrides get no
counters.

**Failures.** The report names the first failing state, then shrinks it while it still fails: each
length and range parameter of a random state is binary-searched towards its lower bound and every `"any"`
register is tried at zero (one slot at a time, the rest of the state unchanged); for a recorded state each
planted memory chunk is dropped if the failure stays. It prints the shrunk input and its differences:

```
first failing state: #1 (random, shape vram, index 0)
  minimised input: r0=0x06001af0 r1=0x00000000 r2=0x00000000 ... sp=0x03007da4 lr=0x08005f8b flags=NzcV cycles=1000000
  differences (ABI):
    r0: original 0x06001af0, override 0x06001af4
    memory writes (by address): original 0 writes, override 1; first difference at #0: original (none), override str32 [0x06001af0] = 0x00000000
```

`--json FILE` writes the same as JSON for tools: `pass`, `states` (total, random, recorded, passed, failed,
`full_identical`, `same_stop`), per-shape counts, `abi_differences` and `full_differences` (counts per
kind), `coverage` (blocks, covered, percent, uncovered addresses, runs per block), `masks`, and
`first_failure` (input, differences, `minimised_input`, `minimised_differences`).

`--impl NAME` tests another implementation compiled into the program (a module may register candidates
besides the configured one). `--mark-verified` (with `--overrides DIR`) writes, after a pass,
`verified = true  # anygba difftest passed <date>: <states> states, block coverage <percent>` into the
override's entry in `DIR/overrides.toml`. Set `verified` only this way, and rebuild the game to pick it up.

Results so far: Emerald's StringCopy passes 10,000 random states (seeds 12345 and 777) and 72 states
recorded in the Bag menu, at the ABI level and in full state, with 4 of 4 blocks covered.

## Tests

- `tests/recomp/override_config_tests.cpp`: config, modules, symbol resolution, generated code.
- `tests/runtime/override_tests.cpp`: activation, calls from recompiled and interpreted code, guest calls,
  `run_original()`, the missing-return error.
- `override-e2e` (ctest): tonc's irq_demo with `tests/overrides/call-paths`, one override per call path
  (bl from recompiled code, bx from recompiled code, bl from interpreted code); counts and the frame hash
  with `--overrides on` and `off`.
- `tests/runtime/difftest_tests.cpp`: spec parsing, random state generation, recorded-state files, and a
  difftest of a correct and an off-by-one override on a small test program.
- `override-difftest` (ctest): irq_demo with `tests/overrides/difftest` (crt0's word fill): the override
  passes 10,000 random states with all blocks covered, an off-by-one variant (`--impl`) fails with a
  zero byte count as the minimised input, states recorded at boot pass, and `anygba difftest
  --mark-verified` records the pass in a copy of the module.
