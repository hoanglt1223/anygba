"""Reads the recompiler's translation of a function (f_XXXXXXXX_orig in the generated funcs_NNN.c)
and turns it into the per-block cycle table an override needs for Guest::block().

Generated code ends every basic block with
    AG_SPEND(c, <region>, <instructions>, <extra cycles>);
    AG_TICK(c, <next pc>);
which is exactly Guest::block(next_pc, insns, extra)."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

_INSN = re.compile(r"/\* ([0-9a-f]{8}): (.*?) \*/")
_LABEL = re.compile(r"^L_([0-9a-f]{8}):;")
_SPEND = re.compile(r"AG_SPEND\(c, \d+, (\d+), (\d+)\);")
_TICK = re.compile(r"AG_TICK\(c, 0x([0-9a-f]{8})u\);")
_CALL = re.compile(r"AG_CALL\(c, f_([0-9a-f]{8}), 0x([0-9a-f]{8})u\);")
_ICALL = re.compile(r"AG_ICALL\(c, 0x([0-9a-f]{8})u\);")
_LR = re.compile(r"c->r\[14\] = 0x([0-9a-f]{8})u;")
_GOTO = re.compile(r"goto L_([0-9a-f]{8});")


@dataclass
class Block:
    start: int
    instructions: list[tuple[int, str]] = field(default_factory=list)
    insns: int | None = None  # instructions charged at the end
    extra: int | None = None
    next_pc: int | None = None
    after: list[str] = field(default_factory=list)  # what the translation does after charging


def find_function(gen_dir: Path, addr: int, original: bool = True) -> str:
    """Source text of f_XXXXXXXX_orig (or f_XXXXXXXX) in the generated project."""
    name = f"f_{addr:08x}_orig" if original else f"f_{addr:08x}"
    head = f"void {name}(anygba_cpu* c) {{"
    for path in sorted(gen_dir.glob("funcs_*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        at = text.find(head)
        if at < 0:
            continue
        end = text.find("\n}\n", at)
        return text[at:end + 3 if end >= 0 else len(text)]
    raise LookupError(f"{name} is not in {gen_dir} (is the function overridden in this build?)")


def parse_blocks(source: str, entry: int) -> list[Block]:
    blocks: list[Block] = [Block(entry)]
    pending_lr: int | None = None
    for raw in source.splitlines():
        line = raw.strip()
        if m := _LABEL.match(line):
            start = int(m.group(1), 16)
            if blocks[-1].instructions or blocks[-1].insns is not None:
                blocks.append(Block(start))
            else:
                blocks[-1].start = start
            continue
        if m := _INSN.search(line):
            addr = int(m.group(1), 16)
            if blocks[-1].insns is not None:  # instructions after a charge without a label: a new block
                blocks.append(Block(addr))
            blocks[-1].instructions.append((addr, m.group(2)))
            continue
        b = blocks[-1]
        if m := _SPEND.search(line):
            b.insns, b.extra = int(m.group(1)), int(m.group(2))
        elif m := _TICK.search(line):
            b.next_pc = int(m.group(1), 16)
        elif m := _LR.search(line):
            pending_lr = int(m.group(1), 16)
        elif m := _CALL.search(line):
            lr = f"lr = 0x{pending_lr:08x}, " if pending_lr is not None else ""
            b.after.append(f"call 0x{int(m.group(1), 16):08x} ({lr}continues at 0x{int(m.group(2), 16):08x})")
            pending_lr = None
        elif m := _ICALL.search(line):
            b.after.append(f"indirect call (continues at 0x{int(m.group(1), 16):08x})")
        elif line.startswith("if (") and b.insns is not None:
            b.after.append(line.rstrip("{ ").strip())
        elif m := _GOTO.search(line):
            if b.insns is not None:
                b.after.append(f"goto 0x{int(m.group(1), 16):08x}")
        elif line == "return;" and b.insns is not None and "return" not in b.after:
            b.after.append("return")
    return [b for b in blocks if b.instructions or b.insns is not None]


def describe(blocks: list[Block], entry: int) -> str:
    """The table given to the model and written into scaffolds."""
    out: list[str] = []
    for i, b in enumerate(blocks, 1):
        out.append(f"block {i} at 0x{b.start:08x} (entry + 0x{b.start - entry:02x}):")
        for addr, text in b.instructions:
            out.append(f"    {addr:08x}: {text}")
        if b.insns is not None and b.next_pc is not None:
            out.append(f"  -> g.block(entry + 0x{b.next_pc - entry:02x}, {b.insns}, {b.extra})"
                       f"   // next_pc 0x{b.next_pc:08x}")
        for a in b.after:
            out.append(f"  then {a}")
    return "\n".join(out)
