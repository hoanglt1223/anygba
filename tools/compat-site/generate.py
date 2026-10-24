#!/usr/bin/env python3
"""Renders compat/*.toml (schema: compat/schema.md) into a static progress page and SVG badges.

usage: python tools/compat-site/generate.py [--compat DIR] [--out DIR] [--project NAME]

Writes <out>/progress.html (also as index.html), <out>/compat.json, <out>/badges/compat.svg (titles at
playable or better), one badge per stage, and one per title. Python 3.11+ (tomllib), no other packages.
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import pathlib
import sys
import tomllib

STAGES = ["fails", "boots", "menus", "in-game", "playable", "perfect"]
COLORS = {
    "fails": "#e05d44",
    "boots": "#fe7d37",
    "menus": "#dfb317",
    "in-game": "#a4a61d",
    "playable": "#97ca00",
    "perfect": "#4c1",
}
KINDS = ["commercial", "homebrew", "test-suite"]
REQUIRED = {"title": str, "kind": str, "stage": str}


class CompatError(Exception):
    pass


def load(directory: pathlib.Path) -> list[dict]:
    entries = []
    for path in sorted(directory.glob("*.toml")):
        try:
            data = tomllib.loads(path.read_text(encoding="utf-8"))
        except tomllib.TOMLDecodeError as e:
            raise CompatError(f"{path}: {e}") from e
        for key, kind in REQUIRED.items():
            if not isinstance(data.get(key), kind):
                raise CompatError(f"{path}: '{key}' is required and must be a {kind.__name__}")
        if data["stage"] not in STAGES:
            raise CompatError(f"{path}: stage '{data['stage']}' is not one of {', '.join(STAGES)}")
        if data["kind"] not in KINDS:
            raise CompatError(f"{path}: kind '{data['kind']}' is not one of {', '.join(KINDS)}")
        if data["kind"] == "commercial" and ("rom" in data or "checkpoint_hash" in data):
            raise CompatError(f"{path}: commercial titles carry only rom_id, never a ROM path or hash")
        blockers = data.get("blockers", [])
        if not isinstance(blockers, list) or not all(isinstance(b, str) for b in blockers):
            raise CompatError(f"{path}: 'blockers' must be a list of strings")
        data["id"] = path.stem
        entries.append(data)
    if not entries:
        raise CompatError(f"no *.toml files in {directory}")
    return entries


def text_width(text: str) -> int:
    # Close enough to Verdana 11px for badge layout.
    return int(sum(7 if c.isupper() or c in "mw%" else 6 for c in text)) + 10


def badge(label: str, value: str, color: str) -> str:
    lw, vw = text_width(label), text_width(value)
    w = lw + vw
    label_e, value_e = html.escape(label), html.escape(value)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="20" role="img" aria-label="{label_e}: {value_e}">
<title>{label_e}: {value_e}</title>
<linearGradient id="s" x2="0" y2="100%"><stop offset="0" stop-color="#bbb" stop-opacity=".1"/><stop offset="1" stop-opacity=".1"/></linearGradient>
<clipPath id="r"><rect width="{w}" height="20" rx="3" fill="#fff"/></clipPath>
<g clip-path="url(#r)"><rect width="{lw}" height="20" fill="#555"/><rect x="{lw}" width="{vw}" height="20" fill="{color}"/><rect width="{w}" height="20" fill="url(#s)"/></g>
<g fill="#fff" text-anchor="middle" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11">
<text x="{lw / 2}" y="14">{label_e}</text><text x="{lw + vw / 2}" y="14">{value_e}</text></g>
</svg>
"""


def page(project: str, entries: list[dict], generated: str) -> str:
    counts = {s: sum(1 for e in entries if e["stage"] == s) for s in STAGES}
    total = len(entries)
    bar = "".join(
        f'<div style="width:{100 * counts[s] / total:.3f}%;background:{COLORS[s]}" title="{s}: {counts[s]}"></div>'
        for s in STAGES if counts[s])
    legend = " ".join(
        f'<span class="tag" style="background:{COLORS[s]}">{s} {counts[s]}</span>' for s in reversed(STAGES))
    rows = []
    order = {s: i for i, s in enumerate(STAGES)}
    for e in sorted(entries, key=lambda e: (-order[e["stage"]], e["kind"], e["title"].lower())):
        title = html.escape(e["title"])
        if e.get("source"):
            title = f'<a href="{html.escape(e["source"])}">{title}</a>'
        blockers = "<br>".join(html.escape(b) for b in e.get("blockers", [])) or "&ndash;"
        misses = e.get("misses")
        rows.append(
            "<tr>"
            f'<td>{title}</td><td>{html.escape(e["kind"])}</td>'
            f'<td><span class="tag" style="background:{COLORS[e["stage"]]}">{e["stage"]}</span></td>'
            f'<td>{html.escape(str(e.get("evidence", "")))}</td><td>{blockers}</td>'
            f'<td class="num">{"" if misses is None else misses}</td>'
            f'<td>{html.escape(str(e.get("updated", "")))}</td>'
            "</tr>")
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(project)} compatibility</title>
<style>
body {{ font-family: system-ui, sans-serif; margin: 2rem auto; max-width: 72rem; padding: 0 1rem; color: #222; }}
.bar {{ display: flex; height: 1.25rem; border-radius: 4px; overflow: hidden; margin: .5rem 0 1rem; }}
.tag {{ color: #fff; padding: .1rem .45rem; border-radius: 3px; font-size: .85rem; white-space: nowrap; }}
table {{ border-collapse: collapse; width: 100%; font-size: .92rem; }}
th, td {{ text-align: left; padding: .35rem .5rem; border-bottom: 1px solid #ddd; vertical-align: top; }}
th {{ background: #f4f4f4; position: sticky; top: 0; }}
.num {{ text-align: right; }}
</style>
</head>
<body>
<h1>{html.escape(project)} compatibility</h1>
<p>{total} titles. Stages and evidence rules: <code>compat/schema.md</code>. Generated {html.escape(generated)}.</p>
<div class="bar" role="img" aria-label="titles by stage">{bar}</div>
<p>{legend}</p>
<table>
<thead><tr><th scope="col">Title</th><th scope="col">Kind</th><th scope="col">Stage</th><th scope="col">Evidence</th>
<th scope="col">Blockers</th><th scope="col">Misses</th><th scope="col">Updated</th></tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody>
</table>
</body>
</html>
"""


def main(argv: list[str]) -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compat", type=pathlib.Path, default=root / "compat")
    parser.add_argument("--out", type=pathlib.Path, default=root / "out" / "compat-site")
    parser.add_argument("--project", default="AnyGBA")
    args = parser.parse_args(argv)
    try:
        entries = load(args.compat)
    except CompatError as e:
        print(f"generate.py: {e}", file=sys.stderr)
        return 1

    generated = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    badges = args.out / "badges"
    badges.mkdir(parents=True, exist_ok=True)
    text = page(args.project, entries, generated)
    for name in ("progress.html", "index.html"):
        (args.out / name).write_bytes(text.encode("utf-8"))
    good = sum(1 for e in entries if STAGES.index(e["stage"]) >= STAGES.index("playable"))
    (badges / "compat.svg").write_bytes(
        badge("compatibility", f"{good}/{len(entries)} playable+", COLORS["playable"] if good else COLORS["fails"]).encode())
    for s in STAGES:
        n = sum(1 for e in entries if e["stage"] == s)
        (badges / f"stage-{s}.svg").write_bytes(badge(s, str(n), COLORS[s]).encode())
    for e in entries:
        (badges / f"{e['id']}.svg").write_bytes(badge(e["title"], e["stage"], COLORS[e["stage"]]).encode())
    summary = [{k: v for k, v in e.items() if not isinstance(v, (datetime.date, datetime.datetime))} | {"updated": str(e.get("updated", ""))}
               for e in entries]
    (args.out / "compat.json").write_bytes(json.dumps(summary, indent=1, sort_keys=True).encode())
    print(f"generate.py: {len(entries)} titles -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
