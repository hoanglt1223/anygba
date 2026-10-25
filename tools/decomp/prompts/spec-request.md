
# Argument spec

Also write the argument spec for the random start states, as a ```toml block with exactly one table
(single brackets) and its shapes (double brackets), laid out like this:

```toml
[$spec_key]
dead_stack = 8

[[$spec_key.shape]]
name = "typical"
weight = 10
r0 = { string = "ram", length = [0, 200] }
```

Describe the real inputs of the function: planted
terminated strings (`{ string = "ram", length = [0, 200], terminator = 0xFF }`) for string arguments,
buffers or pointers into the right region for pointer arguments, value ranges for indices and counts
(`{ range = [0, 15] }`), so that at least $min_returned of the states return normally. A few shapes may
cover edge cases (empty strings, maximum indices, other memory regions), with small weights.

Keys of the table: `dead_stack` (bytes below the entry sp that the function's pushes use; default 256),
`tick_limit` (scanlines of 1,232 cycles before a run counts as stuck; default 10000, far too long for
small functions: set a few times the longest normal run, e.g. 50), `min_returned` (leave the default),
`shape`.
Value forms for r0-r12, sp, lr and cycles: an integer; `"any"` (default for r0-r12); a region or list of
regions ("ewram", "iwram", "ram" = both, "io", "palette", "vram", "oam", "rom": a random pointer); or a
table with one of `value`, `range = [lo, hi]`, `pointer = region`, `string = region (length = [min, max],
terminator, bytes = [lo, hi])`, `buffer = region (size = [min, max], bytes = [lo, hi])`,
`near = "reg" (offset = [lo, hi])`, plus `align` (power of two) and `thumb = true`. A shape may also vary
globals the function reads: `memory = [{ addr = 0x03002f90, size = 2, bytes = [0, 16] }]` plants random
bytes at fixed writable addresses (use it when branches depend on a global, so every block is reached).
Defaults: sp in
0x03007A00-0x03007F00, lr a Thumb address in the ROM. Strings and buffers go in writable memory only.
Values and ranges are unsigned 32-bit: write small negative numbers as `[0xFFFFFFF0, 0xFFFFFFFF]` in a
shape of their own. Only `near` offsets may be negative.
