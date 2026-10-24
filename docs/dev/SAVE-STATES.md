# Save states

A save state is a complete snapshot of the emulated machine that a later run, in the same process or
a new one, can resume from. In an interpreter that is easy: everything lives in data. In AnyGBA most
code is compiled C, and while it runs, part of the machine's progress lives in host call frames: the C
function for the guest function being executed, the functions that called it (`AG_CALL` turns `BL` into
a C call), and the instruction inside each of them. Those frames cannot be saved or rebuilt. This page
describes where snapshots are taken so that nothing in them is needed.

Code: `runtime/src/state/` (format and per-component serialization), `System::tick`,
`System::halt_until_irq` and `System::run_safe_point` (safe points), `runtime/src/main.cpp` (slots and
files). Tests: `tests/runtime/state_tests.cpp`, `tests/player-states.cmake`.

## Safe points

A snapshot is only taken at a *safe point*: a moment where every guest register is in `anygba_cpu`,
`r[15]` names the next instruction to execute, and no host frame holds anything the guest still needs.
The runtime has two:

1. **The end of a scheduler tick outside an interrupt handler.** Generated code calls `anygba_tick`
   through `AG_TICK(c, next_pc)`, which stores the address of the next instruction in `r[15]` first; the
   interpreter calls `tick()` after an instruction has updated `r[15]`. After `run_events()` and
   `deliver_irq()` the machine state is complete. Inside an interrupt handler (`kBiosIrqReturn` on the
   return stack) it is not: the BIOS epilogue in `deliver_irq` still has to run on the host stack, so
   requests wait for the next tick.
2. **Inside a BIOS wait (`Halt`, `Stop`, `IntrWait`, `VBlankIntrWait`) before its interrupt arrives.**
   Many games sleep in `VBlankIntrWait` and only run a few hundred cycles per frame, so they rarely end a
   tick outside their VBlank handler. While the HLE BIOS waits and no interrupt has arrived yet, running
   the same SWI again from the start waits in exactly the same way, so the snapshot stores `r[15]` = the
   SWI instruction (`System::swi_pc_`). Halts started by a `HALTCNT` write are not safe points: there
   `r[15]` is not exact.

`System::request_safe_point(action)` records an action; the next safe point runs it (`save_state()` or
`load_state()`) and then **always** unwinds to the dispatcher (`longjmp` to `run_guarded`, the same path
the runtime already uses for context switches), which continues at `r[15]` through `lookup()`: a compiled
function if one starts there, otherwise the interpreter until it reaches one. Because saving unwinds too,
the run that saved and any run that loads that state continue from identical state along an identical
path, which is what makes the frames after a load repeat the frames after the save exactly. The cost is a
short stretch of interpreted code after each save or load, until the next call or return reaches
compiled code. The address dispatch restarts at is not a function entry, so it is not written to the
miss log (`System::resuming_`).

An exception in the action stops `run()` with `anygba::Error`; the player catches its own errors
(`Load state 2 failed: ...`) and shows them instead.

## What is saved

| Part | Contents |
|---|---|
| CPU | `r[0..15]`, N/Z/C/V, T, control bits, SPSR, cycle budget, banked registers |
| Memory | BIOS area, EWRAM, IWRAM, I/O, palette, VRAM, OAM (not the ROM) |
| Scheduler | timeline position and budget, pending event times, scanline, HBlank flag, frame count |
| DMA, timers | every channel's and timer's registers and internal state |
| Video | the last picture and the BG2/BG3 internal reference points |
| Sound | every PSG channel, both FIFOs, the frame sequencer, the next sample time; produced but not yet drained samples are dropped |
| Backup chip | contents plus the Flash command state or EEPROM serial state and size |

Derived state is recomputed on load: the memory page tables (`map_pages`) and fetch costs
(`update_wait_states`). Host-side state is deliberately not saved: the return stack (empty after the
unwind), cheats and ROM patches (they stay as the player set them), options, the miss log.

Loading a state also replaces the save chip's contents, as in other emulators, so the next save-file
write stores the loaded contents.

## File format

Little-endian, version 1:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `AGBSTATE` |
| 8 | 4 | format version |
| 12 | 4 | flags (0) |
| 16 | 64 | SHA-256 of the unpatched ROM, lower-case hex |
| 80 | 8 | frame count |
| 88 | 4 | payload size |
| 92 | 4 | CRC-32 of the payload |
| 96 | | payload: the components above in table order, each field written explicitly (no struct padding) |

`load_state` checks the magic, version, ROM hash, size and CRC before touching the machine, keeps a copy
of the current state, and restores it if the payload still does not fit (for example a truncated
component), so a failed load changes nothing. Any change to what is serialized must bump
`System::kStateVersion`; old files are then refused with a message naming both versions.

The player keeps nine slots per game next to its save file: `<title>-<hash8>.ss1` to `.ss9`.

## Finding a missing field

`tests/runtime/state_tests.cpp` compares the complete state after a save and after a load frame by frame,
plus the picture and sound. A field that is not saved shows up there as a divergence a few frames later.
That is how two problems were found while building this: struct padding written into the file made two
identical runs produce different files, and `reset()` left the instruction fetch costs at zero until a
game wrote WAITCNT, which the load path then computed correctly and so changed the timing.
