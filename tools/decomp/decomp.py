#!/usr/bin/env python3
"""anygba decomp: AI-assisted rewrites of guest functions as overrides (docs/dev/OVERRIDES.md).

For each requested function: Ghidra headless exports pseudo-C, disassembly, callees and globals; the
recompiler's translation gives the per-block cycle costs; an LLM writes a C++ override against the
guest API; the override is compiled into the game and checked with the differential test, and the
failure (compiler errors or the minimised difftest diff) goes back to the model, a bounded number of
times. Everything ROM-derived (Ghidra project, exports, generated sources, overrides, transcripts) is
written below the per-user data folder (or --work), never into the repository."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from string import Template

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import engines  # noqa: E402
import generated  # noqa: E402
import ghidra  # noqa: E402

RECORDING_MAGIC = b"AGDTREC1"
EXIT_CLASSES = {0: "pass", 1: "fail", 3: "insufficient"}


class DecompError(RuntimeError):
    pass


# ---------------------------------------------------------------------------------------------
# Paths and inputs


def data_dir() -> Path:
    """The runtime's per-user data folder (SDL_GetPrefPath("anyport", "anygba")), or ANYGBA_DATA_DIR."""
    forced = os.environ.get("ANYGBA_DATA_DIR")
    if forced:
        return Path(forced)
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming")
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")
    return base / "anyport" / "anygba"


def repo_root() -> Path | None:
    """The AnyGBA source tree this script runs from, if it runs from one."""
    root = HERE.parent.parent
    return root if (root / "runtime").is_dir() and (root / "tools" / "decomp").is_dir() else None


def inside(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def in_repository(path: Path, root: Path) -> bool:
    """True when `path` is inside the source tree and not git-ignored (build folders such as out/ are)."""
    if not inside(path, root):
        return False
    try:
        done = subprocess.run(["git", "-C", str(root), "check-ignore", "-q", str(path.resolve())], capture_output=True)
    except OSError:
        return True
    return done.returncode != 0


def exe_name(name: str) -> str:
    return name + (".exe" if os.name == "nt" else "")


def ident(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9_]", "_", name)
    return s if s and not s[0].isdigit() else "f_" + s


@dataclass
class Target:
    addr: int
    thumb: bool
    name: str
    known: bool  # the name comes from a symbols table

    @property
    def mode(self) -> str:
        return "thumb" if self.thumb else "arm"

    @property
    def impl(self) -> str:
        return "decomp_" + ident(self.name)

    @property
    def file(self) -> str:
        return ident(self.name) + ".cpp"

    @property
    def key(self) -> str:
        return f"0x{self.addr:08x}"


@dataclass
class Symbols:
    by_name: dict[str, tuple[int, bool]] = field(default_factory=dict)
    by_addr: dict[int, str] = field(default_factory=dict)

    def load(self, path: Path) -> None:
        try:
            data = tomllib.loads(path.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError) as e:
            raise DecompError(f"{path}: {e}") from e
        for name, entry in (data.get("functions") or {}).items():
            if not isinstance(entry, dict) or not isinstance(entry.get("addr"), int):
                raise DecompError(f"{path}: [functions] {name} needs addr = 0x...")
            mode = entry.get("mode", "thumb")
            if mode not in ("thumb", "arm"):
                raise DecompError(f"{path}: [functions] {name}: mode must be thumb or arm")
            addr = entry["addr"] & ~1
            known = self.by_name.get(name)
            if known and known[0] != addr:
                raise DecompError(f"{path}: {name} is 0x{addr:08x} here and 0x{known[0]:08x} in another table")
            self.by_name[name] = (addr, mode == "thumb")
            self.by_addr[addr] = name


def parse_func(spec: str, symbols: Symbols, listed: dict[int, bool] | None) -> Target:
    """NAME, 0xADDR, 0xADDR:thumb|arm (an odd address means Thumb)."""
    spec = spec.strip()
    if spec in symbols.by_name:
        addr, thumb = symbols.by_name[spec]
        return Target(addr, thumb, spec, True)
    m = re.fullmatch(r"(0[xX][0-9a-fA-F]+)(?::(thumb|arm))?", spec)
    if not m:
        raise DecompError(f"--func {spec}: not a known symbol (pass --symbols FILE) nor an address 0x...")
    value = int(m.group(1), 16)
    addr = value & ~1
    name = symbols.by_addr.get(addr)
    if m.group(2):
        thumb = m.group(2) == "thumb"
    elif value & 1:
        thumb = True
    elif name is not None:
        thumb = symbols.by_name[name][1]
    elif listed is not None and addr in listed:
        thumb = listed[addr]
    else:
        raise DecompError(f"--func {spec}: instruction set unknown; write 0x{addr:08x}:thumb or 0x{addr:08x}:arm")
    return Target(addr, thumb, name or f"sub_{addr:08x}", name is not None)


def recording_entry(path: Path) -> int:
    with open(path, "rb") as f:
        head = f.read(16)
    if len(head) < 16 or head[:8] != RECORDING_MAGIC:
        raise DecompError(f"{path}: not a recorded states file (<game> --record-entry)")
    return struct.unpack_from("<I", head, 12)[0]


def order_callees_first(targets: list[Target], exports: dict[int, dict]) -> list[Target]:
    """Callees before callers within the requested set; the given order breaks ties and cycles."""
    by_addr = {t.addr: t for t in targets}
    deps = {t.addr: [int(c["addr"], 16) for c in exports[t.addr].get("callees", [])
                     if int(c["addr"], 16) in by_addr and int(c["addr"], 16) != t.addr] for t in targets}
    out: list[Target] = []
    state: dict[int, int] = {}

    def visit(a: int) -> None:
        if state.get(a):
            return  # done, or on the current path (a cycle)
        state[a] = 1
        for d in deps[a]:
            visit(d)
        state[a] = 2
        out.append(by_addr[a])

    for t in targets:
        visit(t.addr)
    return out


# ---------------------------------------------------------------------------------------------
# Building and testing


def run(argv: list, log: Path, timeout: float | None = None, cwd: Path | None = None) -> int:
    with open(log, "ab") as f:
        f.write(("$ " + " ".join(str(a) for a in argv) + "\n").encode("utf-8"))
        f.flush()
        try:
            return subprocess.run([str(a) for a in argv], stdout=f, stderr=subprocess.STDOUT, timeout=timeout,
                                  cwd=cwd).returncode
        except subprocess.TimeoutExpired:
            f.write(f"\n(timed out after {timeout:.0f} s)\n".encode("utf-8"))
            return -1
        except OSError as e:
            raise DecompError(f"cannot run {argv[0]}: {e}") from e


def tail(path: Path, lines: int = 30) -> str:
    try:
        return "\n".join(path.read_bytes().decode("utf-8", errors="replace").splitlines()[-lines:])
    except OSError:
        return ""


def compiler_errors(log: Path, file: str, limit: int = 50) -> str:
    text = log.read_bytes().decode("utf-8", errors="replace").splitlines()
    picked = [l.strip() for l in text if file in l and re.search(r"\berror\b|\bwarning C4\d+|: note:", l)]
    if not picked:
        picked = [l.strip() for l in text if re.search(r"\berror\b", l, re.I)]
    seen, out = set(), []
    for l in picked:
        if l not in seen:
            seen.add(l)
            out.append(l)
    return "\n".join(out[:limit]) or tail(log, 40)


class Game:
    """The generated project of the ROM with the decomp module, and its build."""

    def __init__(self, args, work: Path, rom: Path, prefix: Path):
        self.args = args
        self.work = work
        self.rom = rom
        self.prefix = prefix
        self.gen = work / "gen"
        self.build_dir = work / "cmake"
        self.module = work / "module"
        self.recomp = prefix / "bin" / exe_name("anygba-recomp")
        if not self.recomp.is_file():
            raise DecompError(f"no {self.recomp}; pass --prefix <anygba install prefix>")
        if not (prefix / "lib" / "cmake" / "anygba").is_dir():
            raise DecompError(f"no anygba runtime package under {prefix} (lib/cmake/anygba); pass --prefix")
        self.cmake = shutil.which("cmake")
        if not self.cmake:
            raise DecompError("cmake is not on the PATH")

    def list_functions(self, log: Path) -> dict[int, bool]:
        argv = [self.recomp, self.rom, "--list"] + self.common_args()
        out = subprocess.run([str(a) for a in argv], capture_output=True)
        if out.returncode != 0:
            raise DecompError("anygba-recomp --list failed: " + out.stderr.decode("utf-8", errors="replace")[-800:])
        found = {}
        for m in re.finditer(rb"^function 0x([0-9a-f]{8}) (thumb|arm)", out.stdout, re.M):
            found[int(m.group(1), 16)] = m.group(2) == b"thumb"
        log.write_text(f"{len(found)} functions listed\n", encoding="utf-8")
        return found

    def common_args(self) -> list:
        out: list = []
        for m in self.args.miss_log:
            out += ["--miss-log", Path(m).resolve()]
        if self.args.config:
            out += ["--config", Path(self.args.config).resolve()]
        return out

    def generate(self, entries: Path, log: Path) -> None:
        if self.gen.exists():
            shutil.rmtree(self.gen)
        argv = [self.recomp, self.rom, "--output", self.gen, "--overrides", self.module, "--miss-log", entries]
        argv += self.common_args()
        if run(argv, log) != 0:
            raise DecompError(f"anygba-recomp failed; log {log}:\n{tail(log)}")

    def build(self, log: Path) -> bool:
        if not (self.build_dir / "CMakeCache.txt").exists():
            argv = [self.cmake, "-S", self.gen, "-B", self.build_dir, f"-DCMAKE_PREFIX_PATH={self.prefix}",
                    "-DCMAKE_BUILD_TYPE=Release"]
            if self.args.generator:
                argv += ["-G", self.args.generator]
            if run(argv, log) != 0:
                raise DecompError(f"configuring the game failed; log {log}:\n{tail(log)}")
        return run([self.cmake, "--build", self.build_dir, "--config", "Release"], log, timeout=7200) == 0

    def exe(self) -> Path:
        for candidate in (self.build_dir / "Release" / exe_name("game"), self.build_dir / exe_name("game")):
            if candidate.is_file():
                return candidate
        raise DecompError(f"no game program under {self.build_dir}")

    def difftest(self, target: Target, json_out: Path, log: Path, seed: int, states: int,
                 recorded: list[Path]) -> tuple[str, dict | None]:
        if json_out.exists():
            json_out.unlink()
        argv = [self.exe(), self.rom, "--difftest", target.key, "--spec", self.module / "difftest.toml",
                "--states", states, "--seed", seed, "--json", json_out]
        if self.args.load_state:
            argv += ["--load-state", Path(self.args.load_state).resolve()]
        for r in recorded:
            argv += ["--recorded", r]
        rc = run(argv, log, timeout=self.args.difftest_timeout, cwd=self.work)
        if rc == -1:
            return "timeout", None
        report = None
        if json_out.exists():
            try:
                report = json.loads(json_out.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                report = None
        # The game exits with 1 for errors too (a bad spec, a missing implementation): a verdict
        # always comes with its report.
        status = EXIT_CLASSES.get(rc, "error") if report is not None else "error"
        if status == "error" and re.search(r"^error: .*difftest\.toml:\d+:", tail(log, 60), re.M):
            status = "spec-error"
        return status, report


# ---------------------------------------------------------------------------------------------
# The module: overrides.toml, difftest.toml and one source per function


SCAFFOLD = Template("""\
// $name at $addr ($mode): scaffold written by `anygba decomp`. Replace g.run_original() with the
// rewrite and run the difftest; the Ghidra export and the prompt are in $run_dir.
//
// The translation's blocks (Guest::block(next_pc, insns, extra) at the end of each):
$blocks
#include <anygba/override.hpp>

ANYGBA_OVERRIDE($impl, g) {
    g.run_original();
}
""")


class Module:
    def __init__(self, root: Path):
        self.root = root
        self.state_path = root / "decomp-state.json"
        self.state: dict[str, dict] = {}
        if self.state_path.exists():
            self.state = json.loads(self.state_path.read_text(encoding="utf-8"))

    def entry(self, t: Target) -> dict:
        return self.state.setdefault(t.key, {"name": t.name, "mode": t.mode, "file": t.file, "impl": t.impl})

    def spec(self, t: Target) -> str | None:
        p = self.root / "specs" / f"{ident(t.name)}.toml"
        return p.read_text(encoding="utf-8") if p.exists() else None

    def set_spec(self, t: Target, text: str) -> None:
        (self.root / "specs").mkdir(parents=True, exist_ok=True)
        (self.root / "specs" / f"{ident(t.name)}.toml").write_bytes(text.strip().encode("utf-8") + b"\n")

    def write(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        lines = ["# Overrides written by `anygba decomp` (docs/dev/OVERRIDES.md). Generated from the game:",
                 "# keep this module outside the repository.", ""]
        for key, e in sorted(self.state.items()):
            lines += [f"# {e['name']}", "[[override]]", f"addr = {key}", f"mode = \"{e['mode']}\"",
                      f"impl = \"{e['impl']}\""]
            if not e.get("enabled", True):
                lines.append("enabled = false  # no passing rewrite yet: the source runs the original")
            if e.get("verified"):
                lines.append(f"verified = true  # {e['verified']}")
            lines += ["engine = \"decomp\"", ""]
        (self.root / "overrides.toml").write_bytes("\n".join(lines).encode("utf-8"))
        specs = []
        for key, e in sorted(self.state.items()):
            p = self.root / "specs" / f"{ident(e['name'])}.toml"
            if p.exists():
                specs.append(p.read_text(encoding="utf-8").strip())
        (self.root / "difftest.toml").write_bytes(("\n\n".join(specs) + "\n").encode("utf-8"))
        self.state_path.write_bytes(json.dumps(self.state, indent=2).encode("utf-8"))

    def put_source(self, t: Target, text: str, gen: Path | None) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        (self.root / t.file).write_bytes(text.encode("utf-8"))
        if gen is not None and gen.is_dir():
            (gen / t.file).write_bytes(text.encode("utf-8"))


def calls_original(source: str) -> bool:
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
    return re.search(r"\brun_original\s*\(", code) is not None


def fix_impl_name(source: str, impl: str) -> str:
    """The prompt fixes the implementation name; repair a model that renamed it."""
    return re.sub(r"ANYGBA_OVERRIDE\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", f"ANYGBA_OVERRIDE({impl},", source, count=1)


def spec_problem(text: str, key: str) -> str | None:
    """None when `text` is a TOML spec with exactly the table for `key`."""
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        return f"the spec is not valid TOML: {e}"
    if list(data.keys()) != [key]:
        return f"the spec must hold exactly one table, [\"{key}\"] (found: {', '.join(data.keys()) or 'none'})"
    if not isinstance(data[key], dict):
        return f"[\"{key}\"] must be a table (one [\"{key}\"] header, then [[\"{key}\".shape]] entries)"
    if not isinstance(data[key].get("shape"), list) or not data[key]["shape"]:
        return f"the spec needs at least one [[\"{key}\".shape]]"
    return None


# ---------------------------------------------------------------------------------------------
# Prompts


def read_prompt(name: str) -> Template:
    return Template((HERE / "prompts" / name).read_text(encoding="utf-8"))


def find_overrides_dir(prefix: Path) -> Path | None:
    for d in (HERE.parent.parent / "overrides", prefix / "share" / "anygba" / "overrides", HERE.parent / "overrides"):
        if (d / "pokemon-gen3" / "string_copy.cpp").is_file():
            return d
    return None


def example_for(t: Target, prefix: Path) -> tuple[str, str]:
    """A passing override and its spec; the word fill when the target is the reference's own function."""
    over = find_overrides_dir(prefix)
    if over and t.name != "StringCopy":
        cpp = (over / "pokemon-gen3" / "string_copy.cpp").read_text(encoding="utf-8")
        spec = (over / "pokemon-gen3" / "difftest.toml").read_text(encoding="utf-8")
        return cpp.strip(), spec.strip()
    return ((HERE / "examples" / "word_fill.cpp").read_text(encoding="utf-8").strip(),
            (HERE / "examples" / "word_fill.toml").read_text(encoding="utf-8").strip())


def api_header(prefix: Path) -> str:
    for p in (HERE.parent.parent / "runtime" / "include" / "anygba" / "override.hpp",
              prefix / "include" / "anygba" / "override.hpp"):
        if p.is_file():
            return p.read_text(encoding="utf-8").strip()
    raise DecompError("cannot find anygba/override.hpp (runtime/include or <prefix>/include)")


def describe_export(t: Target, export: dict, symbols: Symbols) -> dict[str, str]:
    asm = "\n".join(f"{int(l['addr'], 16):08x}: {l['text']}" + (f"   ; {l['note']}" if l.get("note") else "")
                    for l in export.get("disassembly", []))
    callees = []
    for c in export.get("callees", []):
        site = int(c["call_site"], 16)
        ret = site + 4 + (1 if t.thumb else 0)
        target = int(c["addr"], 16) | (1 if c["mode"] == "thumb" else 0)
        known = "known name" if c.get("known") else "no known name"
        callees.append(f"- {c['name']} at {c['addr']} ({c['mode']}, {known}): called at 0x{site:08x}, so "
                       f"g.call(0x{target:08x}, 0x{ret:08x})")
    globals_ = []
    for g in export.get("globals", []):
        if g["region"] == "rom" and "pool" not in g["access"] and int(g["addr"], 16) > t.addr and \
                int(g["addr"], 16) < t.addr + int(export.get("size", 0)) + 0x100:
            continue  # the literal pool slot itself; its value is listed as "pool"
        name = g.get("name") or symbols.by_addr.get(int(g["addr"], 16))
        globals_.append(f"- {g['addr']} ({g['region']}{', ' + name if name else ''}): "
                        f"{', '.join(g['access'])} at {', '.join(g['used_at'])}")
    fixed = export.get("fixed_returns") or []
    fixed_note = (f"This export already marks the bx at {', '.join(fixed)} as a return." if fixed else "")
    return {
        "pseudo_c": export.get("pseudo_c") or f"(Ghidra could not decompile: {export.get('decompile_error', '?')})",
        "disassembly": asm,
        "callees": "\n".join(callees) or "(none)",
        "globals": "\n".join(globals_) or "(none)",
        "fixed_note": fixed_note,
    }


def naming_hints(t: Target, symbols: Symbols) -> str:
    near = sorted((a, n) for a, n in symbols.by_addr.items() if abs(a - t.addr) <= 0x400 and a != t.addr)
    lines = [f"- This function is {'named ' + t.name if t.known else 'unnamed (' + t.name + ')'}."]
    if near:
        lines.append("- Known functions nearby (often the same source file): " +
                     ", ".join(f"{n} (0x{a:08x})" for a, n in near[:12]))
    return "\n".join(lines)


def build_prompt(t: Target, title: str, export: dict, blocks_text: str, symbols: Symbols, prefix: Path,
                 want_spec: bool, min_returned: str) -> str:
    example_cpp, example_spec = example_for(t, prefix)
    parts = describe_export(t, export, symbols)
    spec_request = ""
    if want_spec:
        spec_request = read_prompt("spec-request.md").substitute(spec_key=f'"{t.key}"', min_returned=min_returned)
    text = read_prompt("task.md").substitute(
        name=t.name, addr=f"0x{t.addr:08x}", mode=t.mode, title=title, file=t.file, impl=t.impl,
        api=api_header(prefix), min_returned=min_returned, example_cpp=example_cpp, example_spec=example_spec,
        blocks=blocks_text, spec_request=spec_request, **parts)
    return text + "\n# Naming hints\n\n" + naming_hints(t, symbols) + "\n"


def summarise_report(report: dict | None, status: str) -> str:
    if not report:
        return "(no JSON report)"
    s = report.get("states", {})
    lines = [f"Result: {status}. States: {s.get('total')} ({s.get('random')} random, {s.get('recorded')} recorded); "
             f"ABI pass {s.get('passed')}/{s.get('total')}; returned normally {s.get('returned')} "
             f"({100 * float(s.get('returned_share') or 0):.1f}%, required "
             f"{100 * float(report.get('masks', {}).get('min_returned', 0.95)):.0f}%)."]
    if report.get("abi_differences"):
        lines.append("ABI differences (states): " + ", ".join(f"{k} {v}" for k, v in report["abi_differences"].items()))
    if report.get("full_differences"):
        lines.append("Other differences, not gated (states): " +
                     ", ".join(f"{k} {v}" for k, v in report["full_differences"].items()))
    shapes = [f"{x['name']}: {x['states'] - x['failed']}/{x['states']} pass, {x['stopped']} stopped"
              for x in report.get("shapes", [])]
    if shapes:
        lines.append("Per shape: " + "; ".join(shapes) + ".")
    if s.get("same_stop"):
        lines.append("Both stopped the same way: " + "; ".join(f"{v}x {k}" for k, v in s["same_stop"].items()))
    f = report.get("first_failure")
    if f:
        mi = f.get("minimised_input", {})
        lines.append("First failing state, minimised: " + json.dumps(mi)[:1500])
        lines.append("Its differences (ABI): " + "; ".join(d.get("text", str(d)) for d in f.get("minimised_differences", [])))
        other = f.get("minimised_full_differences", [])
        if other:
            lines.append("Its other differences: " + "; ".join(d.get("text", str(d)) for d in other))
    cov = report.get("coverage", {})
    if cov.get("blocks"):
        lines.append(f"Coverage of the original: {cov.get('covered')}/{cov.get('blocks')} blocks.")
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------
# The loop


@dataclass
class Attempt:
    number: int
    outcome: str  # pass, fail, insufficient, compile-error, no-code, spec-error, runtime-error, engine-error
    detail: str = ""
    llm_seconds: float = 0.0
    build_seconds: float = 0.0
    test_seconds: float = 0.0
    input_tokens: int = 0
    output_tokens: int = 0
    cache_tokens: int = 0
    cost_usd: float = 0.0
    model: str = ""  # the model that answered, as the engine reports it


@dataclass
class Result:
    target: Target
    status: str = "not-run"  # pass, failed, scaffold
    attempts: list[Attempt] = field(default_factory=list)
    report: dict | None = None
    note: str = ""


def save_attempts(run_dir: Path, result: Result) -> None:
    """Attempts so far, so an interrupted run still shows what it cost."""
    (run_dir / "attempts.json").write_bytes(json.dumps([a.__dict__ for a in result.attempts], indent=2).encode("utf-8"))


def decompile_one(t: Target, ctx: dict, result: Result) -> None:
    args, game, module, engine = ctx["args"], ctx["game"], ctx["module"], ctx["engine"]
    run_dir: Path = ctx["run_dir"] / ident(t.name)
    run_dir.mkdir(parents=True, exist_ok=True)
    fixed_spec = module.spec(t) is not None and ctx["spec_fixed"].get(t.key, False)
    want_spec = not fixed_spec
    base_prompt = build_prompt(t, ctx["title"], ctx["exports"][t.addr], ctx["blocks"][t.key], ctx["symbols"],
                               ctx["prefix"], want_spec, ctx["min_returned"])
    (run_dir / "export.json").write_bytes(json.dumps(ctx["exports"][t.addr], indent=2).encode("utf-8"))
    system = (HERE / "prompts" / "system.md").read_text(encoding="utf-8").strip()
    scaffold = ctx["scaffolds"][t.key]
    prompt = base_prompt
    previous_cpp = None
    entry = module.entry(t)
    for n in range(1, args.attempts + 1):
        a = Attempt(n, "engine-error")
        result.attempts.append(a)
        (run_dir / f"prompt-{n}.md").write_bytes(prompt.encode("utf-8"))
        print(f"  attempt {n}: asking {engine.name} ({engine.model}) ...", flush=True)
        try:
            reply = engine.ask(system, prompt, run_dir)
        except engines.EngineError as e:
            a.detail = str(e)
            print(f"  attempt {n}: engine error: {e}", flush=True)
            break  # the engine is down or out of quota: retrying the same prompt will not help
        a.llm_seconds, a.input_tokens, a.output_tokens = reply.seconds, reply.input_tokens, reply.output_tokens
        a.cache_tokens, a.cost_usd = reply.cache_tokens, float(reply.cost_usd or 0.0)
        a.model = ",".join(reply.raw.get("modelUsage") or {}) or str(reply.raw.get("model") or engine.model)
        (run_dir / f"reply-{n}.md").write_bytes(reply.text.encode("utf-8"))
        cpp = engines.extract_cpp(reply.text)
        spec_text = engines.extract_toml(reply.text) if want_spec else None
        failure, details, spec_retry = "", "", ""
        problem = spec_problem(spec_text, t.key) if spec_text is not None else None
        if spec_text is not None and problem is None:
            module.set_spec(t, spec_text)  # kept even when the code is rejected below
            module.write()
        if problem:
            a.outcome, failure, details = "spec-error", "the argument spec is unusable", problem
            spec_retry = " Also write the corrected argument spec as a ```toml block."
        elif want_spec and module.spec(t) is None:
            a.outcome, failure = "spec-error", "the reply has no ```toml argument spec"
            details = "The first answer must also hold the argument spec as a ```toml block."
            spec_retry = " Also write the argument spec as a ```toml block."
        if cpp is None:
            a.outcome, failure = "no-code", "the reply has no ```cpp block"
            details = "Reply with the complete source file in one ```cpp block." + ("\n" + details if problem else "")
        elif calls_original(cpp):
            # run_original() passes any difftest trivially: it is the translation itself.
            a.outcome, failure = "uses-original", "the source calls g.run_original()"
            details = "Rewrite the function's behaviour; do not call g.run_original()." + ("\n" + details if problem else "")
        if failure:
            print(f"  attempt {n}: {a.outcome}: {failure}", flush=True)
        else:
            cpp = fix_impl_name(cpp, t.impl)
            previous_cpp = cpp
            (run_dir / f"candidate-{n}.cpp").write_bytes(cpp.encode("utf-8"))
            module.put_source(t, cpp, game.gen)
            start = time.monotonic()
            built = game.build(run_dir / f"build-{n}.log")
            a.build_seconds = time.monotonic() - start
            if not built:
                a.outcome, failure = "compile-error", "the source does not compile"
                details = "Compiler output:\n```\n" + compiler_errors(run_dir / f"build-{n}.log", t.file) + "\n```"
            else:
                start = time.monotonic()
                status, report = game.difftest(t, run_dir / f"difftest-{n}.json", run_dir / f"difftest-{n}.log",
                                               args.seed, args.states, ctx["recorded"].get(t.addr, []))
                a.test_seconds = time.monotonic() - start
                result.report = report
                if status == "pass":
                    a.outcome = "pass"
                    a.detail = summarise_report(report, status).splitlines()[0]
                    print(f"  attempt {n}: PASS ({a.detail})", flush=True)
                    result.status = "pass"
                    s = report["states"] if report else {}
                    cov = (report or {}).get("coverage", {})
                    entry.update(enabled=True, verified=(
                        f"anygba decomp: difftest passed {datetime.date.today().isoformat()}: {s.get('total')} states "
                        f"({100 * float(s.get('returned_share') or 0):.1f}% returned normally), block coverage "
                        f"{float(cov.get('percent') or 0):.1f}%"))
                    module.write()
                    save_attempts(run_dir, result)
                    return
                if status == "fail":
                    a.outcome = "fail:" + ",".join(sorted((report or {}).get("abi_differences", {}).keys()) or ["?"])
                    failure = "the differential test found differences"
                    details = summarise_report(report, status)
                elif status == "insufficient":
                    a.outcome = "insufficient"
                    failure = "too few states returned normally (insufficient)"
                    details = summarise_report(report, status)
                    if fixed_spec:
                        details += ("\nThe argument spec is fixed by the user, so a rewrite cannot change this: "
                                    "the spec or the tick limit needs a change.")
                        a.detail = failure
                        print(f"  attempt {n}: insufficient with a fixed spec: stopping", flush=True)
                        break
                    spec_retry = (" Most states did not return normally: if the original also runs past the tick "
                                  "limit or returns elsewhere, the spec's inputs are the problem; then also write a "
                                  "corrected argument spec as a ```toml block.")
                elif status == "timeout":
                    a.outcome = "timeout"
                    failure = f"the differential test did not finish within {args.difftest_timeout:.0f} s"
                    details = ("States that never return run until the tick limit (default 10000 scanlines, about "
                               "12 million cycles, each). Either the code loops forever on some inputs, or the spec "
                               "needs bounded inputs and a tick_limit a few times the longest normal run.")
                    if not fixed_spec:
                        spec_retry = " If the spec is the cause, also write a corrected argument spec as a ```toml block."
                elif status == "spec-error" and not fixed_spec:
                    a.outcome = "spec-error"
                    failure = "the argument spec was rejected"
                    details = "\n".join(l for l in tail(run_dir / f"difftest-{n}.log", 60).splitlines()
                                        if l.startswith("error:"))
                    spec_retry = " Also write the corrected argument spec as a ```toml block."
                else:
                    a.outcome = "runtime-error"
                    failure = "the game stopped with an error during the differential test"
                    details = "Output:\n```\n" + tail(run_dir / f"difftest-{n}.log", 25) + "\n```"
            print(f"  attempt {n}: {a.outcome}", flush=True)
        a.detail = failure
        if spec_retry and not fixed_spec:
            want_spec = True
        else:
            want_spec = module.spec(t) is None
        save_attempts(run_dir, result)
        prompt = base_prompt + read_prompt("retry.md").substitute(
            attempt=n, failure=failure, previous_cpp=(previous_cpp or "(none)").strip(), details=details,
            spec_retry=spec_retry)
    result.status = "failed"
    save_attempts(run_dir, result)
    module.put_source(t, scaffold, game.gen)
    entry.update(enabled=False, verified=None)
    module.write()


def print_summary(results: list[Result], out_md: Path, engine_desc: str) -> None:
    head = ("| Function | Address | Result | Attempts | Outcomes | Tokens in/out (cache) | LLM s | Build s | Test s "
            "| Cost USD | Returned | Coverage |")
    rows = [head, "|" + "---|" * 12]
    totals = Attempt(0, "")
    for r in results:
        att = r.attempts
        for a in att:
            for k in ("llm_seconds", "build_seconds", "test_seconds", "input_tokens", "output_tokens", "cache_tokens",
                      "cost_usd"):
                setattr(totals, k, getattr(totals, k) + getattr(a, k))
        s = (r.report or {}).get("states", {}) if r.status == "pass" else {}
        cov = (r.report or {}).get("coverage", {}) if r.status == "pass" else {}
        rows.append(
            f"| {r.target.name} | 0x{r.target.addr:08x} | {r.status} | {len(att)} | {', '.join(a.outcome for a in att) or '-'} "
            f"| {sum(a.input_tokens for a in att)}/{sum(a.output_tokens for a in att)} ({sum(a.cache_tokens for a in att)}) "
            f"| {sum(a.llm_seconds for a in att):.0f} | {sum(a.build_seconds for a in att):.0f} "
            f"| {sum(a.test_seconds for a in att):.0f} | {sum(a.cost_usd for a in att):.2f} "
            f"| {100 * float(s.get('returned_share') or 0):.1f}% | "
            f"{(str(cov.get('covered')) + '/' + str(cov.get('blocks'))) if cov else '-'} |")
    passed = sum(r.status == "pass" for r in results)
    text = (f"anygba decomp: {passed}/{len(results)} functions pass ({engine_desc})\n\n" + "\n".join(rows) +
            f"\n\nTotal: {totals.input_tokens} input + {totals.cache_tokens} cache + {totals.output_tokens} output "
            f"tokens, LLM {totals.llm_seconds:.0f} s, builds {totals.build_seconds:.0f} s, difftests "
            f"{totals.test_seconds:.0f} s, reported cost {totals.cost_usd:.2f} USD\n")
    out_md.write_bytes(text.encode("utf-8"))
    print("\n" + text)


# ---------------------------------------------------------------------------------------------


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="anygba decomp",
        description="Rewrite guest functions as overrides with Ghidra and an LLM, checked by the differential test "
                    "(docs/dev/OVERRIDES.md).")
    p.add_argument("rom", help="the ROM (.gba)")
    p.add_argument("--func", action="append", default=[], help="NAME | 0xADDR[:thumb|arm]; repeatable (batch)")
    p.add_argument("--funcs", help="file with one --func value per line (# comments)")
    p.add_argument("--symbols", action="append", default=[], help="TOML file with a [functions] table; repeatable")
    p.add_argument("--engine", choices=["claude-cli", "anthropic-api", "none"],
                   help="default: claude-cli when installed, else anthropic-api with ANTHROPIC_API_KEY, else none")
    p.add_argument("--model", help=f"model (claude-cli default {engines.DEFAULT_CLI_MODEL}, anthropic-api default "
                                   f"{engines.DEFAULT_API_MODEL})")
    p.add_argument("--attempts", type=int, default=4, help="model answers per function (default 4)")
    p.add_argument("--states", type=int, default=10000, help="random difftest states (default 10000)")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--difftest-timeout", type=float, default=300.0,
                   help="seconds one difftest run may take (default 300)")
    p.add_argument("--load-state", help="save state the random states start from")
    p.add_argument("--recorded", action="append", default=[],
                   help="states from <game> --record-entry; each goes to the function at its entry; repeatable")
    p.add_argument("--spec", help="difftest.toml with tables for the functions: used as is, never rewritten")
    p.add_argument("--miss-log", action="append", default=[], help="passed to the recompiler; repeatable")
    p.add_argument("--config", help="game config passed to the recompiler")
    p.add_argument("--work", help="work folder (default: <data folder>/decomp/<game code>-<hash>)")
    p.add_argument("--prefix", help="anygba install prefix (runtime package and anygba-recomp)")
    p.add_argument("--generator", help="CMake generator for the game build")
    p.add_argument("--ghidra", help="Ghidra install folder (or ANYGBA_GHIDRA / GHIDRA_INSTALL_DIR)")
    p.add_argument("--java-home", help="JDK for Ghidra (or ANYGBA_JAVA_HOME / JAVA_HOME)")
    a = p.parse_args(argv)
    if a.funcs:
        for line in Path(a.funcs).read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                a.func.append(line)
    if not a.func:
        p.error("give --func (or --funcs FILE)")
    if a.attempts < 1 or a.attempts > 20:
        p.error("--attempts must be 1..20")
    if a.states < 0:
        p.error("--states must be at least 0")
    return a


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    rom = Path(args.rom).resolve()
    if not rom.is_file():
        raise DecompError(f"no such ROM: {rom}")
    data = rom.read_bytes()
    if len(data) < 0xC0:
        raise DecompError(f"{rom} is too small for a GBA ROM")
    code = data[0xAC:0xB0].decode("ascii", errors="replace")
    title = data[0xA0:0xAC].decode("ascii", errors="replace").rstrip("\0 ") or rom.stem
    label = code if re.fullmatch(r"[A-Z0-9]{4}", code) else (re.sub(r"[^A-Za-z0-9]+", "-", title).strip("-") or "game")
    tag = label + "-" + hashlib.sha256(data).hexdigest()[:8]

    if args.prefix:
        prefix = Path(args.prefix).resolve()
    elif os.environ.get("ANYGBA_PREFIX"):
        prefix = Path(os.environ["ANYGBA_PREFIX"]).resolve()
    else:
        raise DecompError("pass --prefix <anygba install prefix> (anygba decomp passes its own)")

    work = Path(args.work).resolve() if args.work else data_dir() / "decomp" / tag
    root = repo_root()
    if root and in_repository(work, root):
        raise DecompError(f"{work} is inside the AnyGBA source tree; ROM-derived output must stay outside it "
                          "(or in a git-ignored build folder)")
    work.mkdir(parents=True, exist_ok=True)
    run_dir = work / "runs" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    print(f"anygba decomp: {title} ({code}); work folder {work}", flush=True)

    symbols = Symbols()
    shipped = prefix / "share" / "anygba" / "symbols" / f"{code}.toml"
    if shipped.is_file():
        symbols.load(shipped)
    for s in args.symbols:
        symbols.load(Path(s))

    engine_name = engines.pick_engine(args.engine)
    engine = engines.make_engine(engine_name, args.model)
    game = Game(args, work, rom, prefix)

    listed = None
    if any(re.fullmatch(r"0[xX][0-9a-fA-F]*[02468aAcCeE]", f.strip()) for f in args.func):
        listed = game.list_functions(run_dir / "list.log")
    targets: list[Target] = []
    for f in args.func:
        t = parse_func(f, symbols, listed)
        if any(x.addr == t.addr for x in targets):
            continue
        if any(x.file == t.file for x in targets):
            raise DecompError(f"two functions named {t.name}")
        targets.append(t)

    # Ghidra export of every target in one run.
    gh = ghidra.find_ghidra(args.ghidra)
    java_home = ghidra.find_java_home(args.java_home)
    major = ghidra.java_major(java_home)
    if major is not None and major < 25:
        print(f"anygba decomp: warning: Java {major} found; Ghidra {ghidra.TESTED_VERSION} needs Java 25", flush=True)
    sym_list = [(a, "thumb" if symbols.by_name[n][1] else "arm", n) for a, n in symbols.by_addr.items()]
    print(f"anygba decomp: Ghidra export of {len(targets)} function(s) ...", flush=True)
    start = time.monotonic()
    exported = ghidra.export(gh, java_home, work / "ghidra", rom, [(t.addr, t.thumb, t.name) for t in targets],
                             sym_list, run_dir / "export.json", run_dir / "ghidra.log")
    ghidra_seconds = time.monotonic() - start
    exports = {int(f["addr"], 16): f for f in exported["functions"]}
    targets = order_callees_first(targets, exports)
    print(f"anygba decomp: exported in {ghidra_seconds:.0f} s; order: {', '.join(t.name for t in targets)}", flush=True)

    # The module: scaffolds for this run's targets, earlier results kept.
    module = Module(work / "module")
    spec_fixed: dict[str, bool] = {}
    user_spec = None
    if args.spec:
        try:
            user_spec = tomllib.loads(Path(args.spec).read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError) as e:
            raise DecompError(f"--spec {args.spec}: {e}") from e
    for t in targets:
        e = module.entry(t)
        e.update(name=t.name, mode=t.mode, file=t.file, impl=t.impl, enabled=True, verified=None)
        table = None
        if user_spec is not None:
            table = user_spec.get(t.key) or user_spec.get(t.name)
        if table is not None:
            module.set_spec(t, spec_from_table(t.key, table))
            spec_fixed[t.key] = True
    entries = work / "entries.log"
    entries.write_bytes("".join(f"0x{t.addr:08x} {t.mode}\n" for t in targets).encode("utf-8"))
    placeholder = {t.key: SCAFFOLD.substitute(name=t.name, addr=f"0x{t.addr:08x}", mode=t.mode, run_dir=run_dir,
                                              blocks="//   (see the run folder)", impl=t.impl) for t in targets}
    for t in targets:
        module.put_source(t, placeholder[t.key], None)
    module.write()

    print("anygba decomp: generating the game's C++ with the scaffolds ...", flush=True)
    game.generate(entries, run_dir / "recomp.log")
    blocks: dict[str, str] = {}
    scaffolds: dict[str, str] = {}
    for t in targets:
        src = generated.find_function(game.gen, t.addr)
        blocks[t.key] = generated.describe(generated.parse_blocks(src, t.addr), t.addr)
        scaffolds[t.key] = SCAFFOLD.substitute(
            name=t.name, addr=f"0x{t.addr:08x}", mode=t.mode, run_dir=run_dir,
            blocks="\n".join("//   " + l for l in blocks[t.key].splitlines()), impl=t.impl)
        module.put_source(t, scaffolds[t.key], game.gen)

    recorded: dict[int, list[Path]] = {}
    for r in args.recorded:
        p = Path(r).resolve()
        recorded.setdefault(recording_entry(p), []).append(p)

    min_returned = "95%"
    ctx = dict(args=args, game=game, module=module, engine=engine, run_dir=run_dir, title=title, exports=exports,
               blocks=blocks, symbols=symbols, prefix=prefix, scaffolds=scaffolds, recorded=recorded,
               spec_fixed=spec_fixed, min_returned=min_returned)

    if engine is None:
        for t in targets:
            d = run_dir / ident(t.name)
            d.mkdir(parents=True, exist_ok=True)
            (d / "export.json").write_bytes(json.dumps(exports[t.addr], indent=2).encode("utf-8"))
            prompt = build_prompt(t, title, exports[t.addr], blocks[t.key], symbols, prefix, module.spec(t) is None,
                                  min_returned)
            (d / "prompt.md").write_bytes(prompt.encode("utf-8"))
            module.entry(t).update(enabled=False)
        module.write()
        print(f"anygba decomp: engine none: exports, prompts and scaffolds written.\n  module: {module.root}\n"
              f"  run: {run_dir}\nWrite the overrides by hand and their argument specs as "
              f"{module.root / 'specs'}/<name>.toml, then rerun with --spec, or build the game with "
              f"--overrides {module.root} and run anygba difftest.")
        return 0

    print("anygba decomp: building the game (the first build compiles all generated code) ...", flush=True)
    start = time.monotonic()
    if not game.build(run_dir / "build-scaffold.log"):
        raise DecompError(f"the scaffold build failed; log {run_dir / 'build-scaffold.log'}:\n"
                          f"{tail(run_dir / 'build-scaffold.log')}")
    print(f"anygba decomp: built in {time.monotonic() - start:.0f} s", flush=True)

    results: list[Result] = []
    for t in targets:
        print(f"\n== {t.name} at 0x{t.addr:08x} ({t.mode})", flush=True)
        r = Result(t)
        results.append(r)
        try:
            decompile_one(t, ctx, r)
        except Exception as e:  # noqa: BLE001 - one function's failure must not end the batch
            r.status, r.note = "failed", f"{type(e).__name__}: {e}"
            print(f"  error: {r.note}", flush=True)
            if not isinstance(e, (DecompError, engines.EngineError)):
                import traceback
                traceback.print_exc()
            module.put_source(t, scaffolds[t.key], game.gen)
            module.entry(t).update(enabled=False, verified=None)
            module.write()
    desc = f"engine {engine.name}, model {engine.model}, up to {args.attempts} attempts, {args.states} states, seed {args.seed}"
    print_summary(results, run_dir / "summary.md", desc)
    (run_dir / "summary.json").write_bytes(json.dumps([{
        "function": r.target.name, "addr": r.target.key, "mode": r.target.mode, "status": r.status, "note": r.note,
        "attempts": [a.__dict__ for a in r.attempts],
        "states": (r.report or {}).get("states"), "coverage": {k: v for k, v in ((r.report or {}).get("coverage") or {}).items()
                                                               if k in ("blocks", "covered", "percent")},
    } for r in results], indent=2).encode("utf-8"))
    print(f"module: {module.root}\nrun: {run_dir}")
    return 0 if all(r.status == "pass" for r in results) else 1


def spec_from_table(key: str, table: dict) -> str:
    """TOML text for one spec table taken from a user's --spec file (re-keyed by address)."""
    lines = [f'["{key}"]']
    shapes = table.get("shape", [])
    for k, v in table.items():
        if k != "shape":
            lines.append(f"{k} = {toml_value(v)}")
    for s in shapes:
        lines += ["", f'[["{key}".shape]]'] + [f"{k} = {toml_value(v)}" for k, v in s.items()]
    return "\n".join(lines) + "\n"


def toml_value(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return json.dumps(v)
    if isinstance(v, list):
        return "[" + ", ".join(toml_value(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{ " + ", ".join(f"{k} = {toml_value(x)}" for k, x in v.items()) + " }"
    raise DecompError(f"unsupported value in --spec: {v!r}")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (DecompError, ghidra.GhidraError, engines.EngineError) as e:
        print(f"anygba decomp: {e}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
    except Exception:  # a bug: show it, and exit like other errors rather than like "a function failed"
        import traceback
        traceback.print_exc()
        sys.exit(2)
