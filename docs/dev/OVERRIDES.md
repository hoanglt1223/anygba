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
on the entries with `enabled = false`. The menu does not list overrides yet.

## Tests

- `tests/recomp/override_config_tests.cpp`: config, modules, symbol resolution, generated code.
- `tests/runtime/override_tests.cpp`: activation, calls from recompiled and interpreted code, guest calls,
  `run_original()`, the missing-return error.
- `override-e2e` (ctest): tonc's irq_demo with `tests/overrides/call-paths`, one override per call path
  (bl from recompiled code, bx from recompiled code, bl from interpreted code); counts and the frame hash
  with `--overrides on` and `off`.
