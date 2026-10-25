"""Ghidra headless export for `anygba decomp`: one cached project per ROM, AnygbaExport.java as the
post-script (tools/decomp/ghidra/)."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent / "ghidra"
TESTED_VERSION = "12.2"


class GhidraError(RuntimeError):
    pass


def find_ghidra(given: str | None) -> Path:
    for candidate in (given, os.environ.get("ANYGBA_GHIDRA"), os.environ.get("GHIDRA_INSTALL_DIR")):
        if candidate:
            root = Path(candidate)
            if headless_path(root).is_file():
                return root
            raise GhidraError(f"{root} is not a Ghidra install (no support/analyzeHeadless)")
    raise GhidraError("Ghidra not found: pass --ghidra DIR or set ANYGBA_GHIDRA (tested with Ghidra "
                      f"{TESTED_VERSION}, which needs Java 25)")


def headless_path(root: Path) -> Path:
    return root / "support" / ("analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")


def find_java_home(given: str | None) -> Path | None:
    """--java-home, ANYGBA_JAVA_HOME or JAVA_HOME; None lets Ghidra search the PATH."""
    for candidate in (given, os.environ.get("ANYGBA_JAVA_HOME"), os.environ.get("JAVA_HOME")):
        if candidate:
            home = Path(candidate)
            java = home / "bin" / ("java.exe" if os.name == "nt" else "java")
            if not java.is_file():
                raise GhidraError(f"{home} is not a JDK (no bin/java)")
            return home
    return None


def java_major(home: Path | None) -> int | None:
    java = str(home / "bin" / "java") if home else shutil.which("java")
    if not java:
        return None
    try:
        out = subprocess.run([java, "-version"], capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r'version "(\d+)', out.stderr.decode("utf-8", errors="replace"))
    return int(m.group(1)) if m else None


def export(ghidra: Path, java_home: Path | None, project_dir: Path, rom: Path,
           funcs: list[tuple[int, bool, str | None]], symbols: list[tuple[int, str, str]],
           out_json: Path, log: Path) -> dict:
    """Exports `funcs` ((addr, thumb, name)); `symbols` are (addr, mode, name) known names.
    The ROM is imported once into project_dir (copied there first, since analyzeHeadless.bat
    cannot take paths with parentheses) and reused afterwards."""
    project_dir.mkdir(parents=True, exist_ok=True)
    program = "rom.gba"
    local_rom = project_dir / program
    if not local_rom.exists():
        shutil.copyfile(rom, local_rom)
    request = project_dir / "request.txt"
    lines = [f"func 0x{a:08x} {'thumb' if t else 'arm'} {n or ''}".rstrip() for a, t, n in funcs]
    lines += [f"sym 0x{a:08x} {m} {n}" for a, m, n in symbols]
    request.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    if out_json.exists():
        out_json.unlink()

    argv = [str(headless_path(ghidra)), str(project_dir), "anygba"]
    if (project_dir / "anygba.gpr").exists():
        argv += ["-process", program]
    else:
        argv += ["-import", str(local_rom), "-loader", "BinaryLoader", "-loader-baseAddr", "0x08000000",
                 "-processor", "ARM:LE:32:v4t"]
    argv += ["-noanalysis", "-scriptPath", str(SCRIPT_DIR), "-postScript", "AnygbaExport.java", str(request),
             str(out_json)]
    env = dict(os.environ)
    if java_home:
        env["JAVA_HOME"] = str(java_home)
        env["PATH"] = str(java_home / "bin") + os.pathsep + env.get("PATH", "")
    with open(log, "wb") as f:
        # Ghidra looks for the script in the working directory first: run where no stray copy can be.
        rc = subprocess.run(argv, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=project_dir).returncode
    if rc != 0 or not out_json.exists():
        tail = log.read_bytes().decode("utf-8", errors="replace").splitlines()[-25:]
        raise GhidraError(f"Ghidra headless failed (exit code {rc}); log {log}:\n" + "\n".join(tail))
    data = json.loads(out_json.read_text(encoding="utf-8"))
    for f in data.get("functions", []):
        f["pseudo_c"] = f.get("pseudo_c", "").replace("\r\n", "\n").strip()
    return data
