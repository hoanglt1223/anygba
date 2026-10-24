#!/usr/bin/env python3
"""Fetch libretro-database GBA cheat files and convert them to AnyGBA cheat databases.

The cheats in libretro-database are community data under CC BY-SA 4.0, so AnyGBA does not
ship them. This script downloads them from a pinned commit and writes one database per ROM,
named by the ROM's CRC-32 (from the database's No-Intro list), with cheats grouped into
categories by their names. Drop the output into a `cheats/` folder next to a game or in the
per-user data folder (docs/user/CHEATS.md).

    python tools/cheats/import-libretro.py --out <dir> [--ref <commit>] [--only <text>]

Each cheat keeps libretro's code text with format "auto"; the game decides the device when it
loads the file and reports, then skips, entries it cannot decode.
"""

import argparse
import concurrent.futures
import json
import pathlib
import re
import sys
import urllib.parse
import urllib.request

# libretro-database at 2026-10-10.
DEFAULT_REF = "12c21cf79f1e20eff6ac68b80875816c717d0a7a"
REPO = "libretro/libretro-database"
CHT_DIR = "cht/Nintendo - Game Boy Advance"
DAT = "metadat/no-intro/Nintendo - Game Boy Advance.dat"


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "anygba-import-libretro"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def raw_url(ref: str, path: str) -> str:
    return f"https://raw.githubusercontent.com/{REPO}/{ref}/{urllib.parse.quote(path)}"


def list_cht_files(ref: str) -> list[str]:
    tree = json.loads(fetch(f"https://api.github.com/repos/{REPO}/git/trees/{ref}?recursive=1"))
    if tree.get("truncated"):
        sys.exit("error: the GitHub tree listing was truncated")
    return sorted(e["path"] for e in tree["tree"] if e["path"].startswith(CHT_DIR + "/") and e["path"].endswith(".cht"))


def parse_dat(text: str) -> dict[str, list[str]]:
    """No-Intro clrmamepro dat: game name -> CRC-32s of its ROMs."""
    games: dict[str, list[str]] = {}
    for block in re.finditer(r'game \(\s*name "([^"]+)"(.*?)\n\)', text, re.S):
        crcs = re.findall(r"\bcrc ([0-9A-Fa-f]{8})\b", block.group(2))
        if crcs:
            games.setdefault(block.group(1), []).extend(c.lower() for c in crcs)
    return games


def normalize(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", name.lower()).strip()


TAGS = re.compile(r"\s*(\([^()]*\)|\[[^\[\]]*\])")
REGION_WORDS = {"usa", "europe", "japan", "world", "germany", "france", "spain", "italy", "netherlands",
                "sweden", "australia", "korea", "china", "brazil", "canada", "unknown"}


def base_title(name: str) -> str:
    return normalize(TAGS.sub("", name))


def region_words(name: str) -> set[str]:
    words = set()
    for tag in re.findall(r"\(([^()]*)\)", name):
        words |= {w.strip().lower() for w in re.split(r"[,\s]+", tag)} & REGION_WORDS
    return words


def match_game(stem: str, games: dict[str, list[str]], by_normal: dict[str, list[str]],
               by_base: dict[str, list[str]]) -> list[str]:
    """CRCs for a .cht file name: drop trailing tags ("(Code Breaker)", "[!]") until a dat name
    matches; failing that, the same title without tags, preferring the same regions."""
    candidate = stem
    while True:
        if candidate in games:
            return games[candidate]
        if normalize(candidate) in by_normal:
            return by_normal[normalize(candidate)]
        shorter = re.sub(r"\s*(\([^()]*\)|\[[^\[\]]*\])\s*$", "", candidate)
        if shorter == candidate or not shorter:
            break
        candidate = shorter
    names = by_base.get(base_title(stem), [])
    wanted = region_words(stem) - {"unknown"}
    same_region = [n for n in names if wanted and wanted <= region_words(n)]
    crcs: list[str] = []
    for n in same_region or names:
        crcs += games[n]
    return crcs


def parse_cht(text: str) -> list[dict]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r'\s*(cheat\d+_(?:desc|code|enable))\s*=\s*"?(.*?)"?\s*$', line)
        if m:
            values[m.group(1)] = m.group(2)
    cheats = []
    i = 0
    while f"cheat{i}_desc" in values or f"cheat{i}_code" in values:
        desc = values.get(f"cheat{i}_desc", f"Cheat {i}").strip() or f"Cheat {i}"
        code = values.get(f"cheat{i}_code", "").strip()
        if code:
            cheats.append({"desc": desc, "code": code})
        i += 1
    return cheats


def categorize(cheats: list[dict]) -> None:
    """Sets each cheat's "name" to "Category/Leaf" where the names suggest a grouping."""
    first_words: dict[str, int] = {}
    for c in cheats:
        words = c["desc"].split()
        if words:
            first_words[words[0].lower()] = first_words.get(words[0].lower(), 0) + 1
    seen: dict[str, int] = {}
    for c in cheats:
        desc = c["desc"].replace("/", "-")
        lower = desc.lower()
        if "master code" in lower or "enable code" in lower or lower.startswith("(m)") or "must be on" in lower:
            name = "Enable codes/" + desc
        elif " - " in desc:
            head, tail = desc.split(" - ", 1)
            name = head.strip() + "/" + tail.strip()
        elif ": " in desc:
            head, tail = desc.split(": ", 1)
            name = head.strip() + "/" + tail.strip()
        else:
            words = desc.split()
            if len(words) > 1 and first_words.get(words[0].lower(), 0) >= 3:
                name = words[0] + "/" + desc
            else:
                name = "Other/" + desc
        count = seen.get(name, 0) + 1
        seen[name] = count
        c["name"] = name if count == 1 else f"{name} ({count})"


def toml_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def to_toml(title: str, crcs: list[str], ref: str, source_file: str, cheats: list[dict]) -> str:
    out = [
        f"# Converted by tools/cheats/import-libretro.py from {source_file}",
        f"# libretro-database {ref}, CC BY-SA 4.0: https://github.com/{REPO}",
        "",
        "[game]",
        f"title = {toml_string(title)}",
        "crc32 = [" + ", ".join(toml_string(c.upper()) for c in sorted(set(crcs))) + "]",
        f"source = {toml_string('libretro-database ' + ref[:12])}",
        'license = "CC-BY-SA-4.0"',
    ]
    for c in cheats:
        out += ["", "[[cheat]]", f"name = {toml_string(c['name'])}", 'format = "auto"', f"code = [{toml_string(c['code'])}]"]
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, type=pathlib.Path, help="folder for the <crc32>.toml databases")
    parser.add_argument("--ref", default=DEFAULT_REF, help="libretro-database commit (default: pinned)")
    parser.add_argument("--only", help="convert only cheat files whose name contains this text")
    args = parser.parse_args()

    games = parse_dat(fetch(raw_url(args.ref, DAT)).decode("utf-8", "replace"))
    by_normal: dict[str, list[str]] = {}
    by_base: dict[str, list[str]] = {}
    for name, crcs in games.items():
        by_normal.setdefault(normalize(name), []).extend(crcs)
        by_base.setdefault(base_title(name), []).append(name)

    paths = list_cht_files(args.ref)
    if args.only:
        paths = [p for p in paths if args.only.lower() in p.lower()]
    print(f"{len(paths)} cheat files at {args.ref[:12]}")

    def convert(path: str) -> tuple[str, str | None, list[str], str]:
        stem = pathlib.PurePosixPath(path).stem
        crcs = match_game(stem, games, by_normal, by_base)
        if not crcs:
            return stem, "no matching ROM in the No-Intro list", [], ""
        cheats = parse_cht(fetch(raw_url(args.ref, path)).decode("utf-8", "replace"))
        if not cheats:
            return stem, "no cheats", [], ""
        categorize(cheats)
        return stem, None, sorted(set(crcs)), to_toml(stem, crcs, args.ref, pathlib.PurePosixPath(path).name, cheats)

    args.out.mkdir(parents=True, exist_ok=True)
    skipped = 0
    used: dict[str, int] = {}
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for stem, problem, crcs, text in pool.map(convert, paths):
            if problem:
                skipped += 1
                print(f"skipped {stem}: {problem}")
                continue
            written = []
            for crc in crcs:
                # Several .cht files can name one ROM (lists per device): <crc>.toml, <crc>-2.toml...
                used[crc] = used.get(crc, 0) + 1
                name = f"{crc}.toml" if used[crc] == 1 else f"{crc}-{used[crc]}.toml"
                (args.out / name).write_bytes(text.encode("utf-8"))
                written.append(name)
            print(f"{stem} -> {', '.join(written)}")
    print(f"done: {len(paths) - skipped} converted, {skipped} skipped, into {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
