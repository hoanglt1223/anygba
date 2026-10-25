# Task

Write the override of guest function `$name` at $addr ($mode code) of the game "$title".

- Source file: `$file`. Define exactly one override: `ANYGBA_OVERRIDE($impl, g) { ... }`.
- Include `<anygba/override.hpp>` and standard headers only. No other AnyGBA headers, no globals
  with state, no ROM data copied into the source (addresses and small constants from the code are fine).
- Use names that say what the code does. When the function or a callee has a known name, use it in
  comments. Name addresses inside the function relative to `g.entry()` (`entry + 0x12`), so the same
  source works for other games of the same engine at other addresses.
- Start the file with a comment giving the C signature you inferred and one line on what it does.

# Guest API (runtime/include/anygba/override.hpp)

```cpp
$api
```

Rules the runtime relies on:

- End every path with `g.ret()` (bx lr), `g.ret_to(addr)` (bx addr: use it for `pop {rN}; bx rN`
  returns, with the popped value), or `g.run_original()`; or return right after a `g.call()` that
  returned false.
- `g.call(target, return_addr)`: call a guest function as `bl` does. `target` has bit 0 set for Thumb.
  Pass the lr the original's call instruction leaves (Thumb `bl` at X: X + 4 + 1). Set r0-r3 (and any
  stack arguments) first, exactly as the original does; read results from the registers afterwards.
- Push and pop through guest memory exactly as the original does (`push {r4, lr}` stores r4 at sp - 8
  and lr at sp - 4, and the stack slots stay written after the return), and leave sp where the
  original leaves it.
- Timing: call `g.block(next_pc, insns, extra)` once at the end of every basic block of the original,
  in the same order, with the values from the translation's block table below. `next_pc` is the address
  the table gives (entry-relative). Interrupts may run inside `block()` and `call()`.

# What the differential test compares

Gate (must match): r0, r4-r11, sp, where the function returns to (address and instruction set), every
memory write (address, size, value; in order per address, IO registers in global order), memory
contents afterwards, and the guest calls made with their r0-r3. At least $min_returned of the states
must return normally (to the caller's lr within the tick limit) in both versions.
Reported but not gated: r1-r3, r12, lr, flags, cycles used, write order across addresses, the dead
stack below the entry sp. Matching them too is better (flags of the last flag-setting instruction,
scratch registers the original leaves behind).

# Example: a passing override and its spec

```cpp
$example_cpp
```

```toml
$example_spec
```

# Ghidra's view of the function

Ghidra's pseudo-C is a hint, not the truth: parameter types and counts may be wrong, and it reads
`pop {rN}; bx rN` (a Thumb return) as a jump; trust the disassembly. $fixed_note

```c
$pseudo_c
```

Disassembly:

```
$disassembly
```

Callees:
$callees

Globals (addresses the code loads or stores, or loads from its literal pool):
$globals

# The translation's blocks (cycle costs for g.block())

```
$blocks
```
$spec_request
