#!/usr/bin/env python3
"""Builds the AnyGBA website: site/pages/*.html wrapped in one layout, plus the compatibility page
rendered from compat/*.toml by tools/compat-site/generate.py.

usage: python site/build.py [--out DIR] [--check]

--check verifies after the build that every internal link and #fragment resolves. Python 3.11+, no
other packages.
"""

from __future__ import annotations

import argparse
import datetime
import html
import importlib.util
import pathlib
import re
import shutil
import sys
from html.parser import HTMLParser

SITE = pathlib.Path(__file__).resolve().parent
ROOT = SITE.parent
VERSION = "0.3.0"
REPO = "https://github.com/hoanglt1223/anygba"

NAV = [
    ("index.html", "Home"),
    ("features.html", "Features"),
    ("guides.html", "Guides"),
    ("tools.html", "Tools"),
    ("compatibility.html", "Compatibility"),
    ("about.html", "About"),
]

META = re.compile(r"\A<!--\s*title:\s*(?P<title>.*?)\s*\|\s*description:\s*(?P<desc>.*?)\s*-->\s*", re.S)


def load_generator():
    sys.dont_write_bytecode = True  # keep tools/compat-site free of bytecode caches
    spec = importlib.util.spec_from_file_location("compat_generate", ROOT / "tools" / "compat-site" / "generate.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def layout(name: str, title: str, desc: str, content: str) -> str:
    nav = "\n".join(
        f'<li><a href="{href}"{' aria-current="page"' if href == name else ""}>{label}</a></li>'
        for href, label in NAV)
    full_title = "AnyGBA" if name == "index.html" else f"{title} | AnyGBA"
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(full_title)}</title>
<meta name="description" content="{html.escape(desc)}">
<meta name="color-scheme" content="light dark">
<link rel="icon" href="assets/favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="assets/site.css">
<script>document.documentElement.dataset.theme = localStorage.getItem("theme") || "";</script>
<script src="assets/site.js" defer></script>
</head>
<body>
<a class="skip" href="#main">Skip to content</a>
<header class="top">
<div class="wrap top-row">
<a class="brand" href="index.html"><img src="assets/favicon.svg" alt="" width="28" height="28"> AnyGBA</a>
<button class="nav-toggle" type="button" aria-expanded="false" aria-controls="nav">Menu</button>
<nav id="nav" aria-label="Main"><ul>
{nav}
</ul></nav>
<button class="theme-toggle" type="button" aria-label="Toggle dark mode" title="Toggle dark mode">&#9680;</button>
</div>
</header>
<main id="main" class="wrap">
{content}
</main>
<footer class="foot">
<div class="wrap">
<p>AnyGBA {VERSION} &middot; MIT licence &middot; <a href="{REPO}">Source on GitHub</a> &middot;
<a href="{REPO}/releases/latest">Releases</a> &middot; <a href="{REPO}/issues">Issues</a></p>
<p class="small">AnyGBA is an independent project, not affiliated with or endorsed by any console maker.
Game Boy Advance is a trademark of its owner. No games, ROMs or BIOS images are provided.</p>
</div>
</footer>
</body>
</html>
"""


def build(out: pathlib.Path) -> None:
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(SITE / "assets", out / "assets")

    gen = load_generator()
    compat_dir = out / "compat"
    if gen.main(["--compat", str(ROOT / "compat"), "--out", str(compat_dir)]) != 0:
        raise SystemExit("build.py: the compatibility generator failed")
    entries = gen.load(ROOT / "compat")
    generated = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    compat_body = gen.body("AnyGBA", entries, generated).replace(
        "<code>compat/schema.md</code>", f'<a href="{REPO}/blob/main/compat/schema.md"><code>compat/schema.md</code></a>')
    playable = sum(1 for e in entries if gen.STAGES.index(e["stage"]) >= gen.STAGES.index("playable"))

    values = {"version": VERSION, "repo": REPO, "compat_body": compat_body,
              "compat_total": str(len(entries)), "compat_playable": str(playable)}
    for page in sorted((SITE / "pages").glob("*.html")):
        text = re.sub(r"\{\{(\w+)\}\}", lambda v: values[v.group(1)], page.read_text(encoding="utf-8"))
        m = META.match(text)
        if not m:
            raise SystemExit(f"build.py: {page.name} must start with <!-- title: ... | description: ... -->")
        (out / page.name).write_bytes(layout(page.name, m["title"], m["desc"], text[m.end():]).encode("utf-8"))
    (out / ".nojekyll").write_bytes(b"")


class Links(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []
        self.ids: set[str] = set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        for key in ("href", "src"):
            if a.get(key):
                self.links.append(a[key])


def check(out: pathlib.Path) -> list[str]:
    parsed = {}
    for f in out.rglob("*.html"):
        p = Links()
        p.feed(f.read_text(encoding="utf-8"))
        parsed[f.resolve()] = p
    errors = []
    for f, p in parsed.items():
        for link in p.links:
            if re.match(r"^(https?:|mailto:)", link):
                continue
            path, _, frag = link.partition("#")
            target = (f.parent / path).resolve() if path else f
            if not target.exists():
                errors.append(f"{f.relative_to(out.resolve())}: missing {link}")
            elif frag and (target not in parsed or frag not in parsed[target].ids):
                errors.append(f"{f.relative_to(out.resolve())}: missing anchor {link}")
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "out" / "site")
    parser.add_argument("--check", action="store_true", help="verify internal links and anchors")
    args = parser.parse_args(argv)
    build(args.out)
    print(f"build.py: site -> {args.out}")
    if args.check:
        errors = check(args.out)
        for e in errors:
            print(f"build.py: {e}", file=sys.stderr)
        if errors:
            return 1
        print("build.py: all internal links resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
