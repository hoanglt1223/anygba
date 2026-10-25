"""LLM back ends for `anygba decomp`: the Claude Code CLI (claude -p, the user's own login), the
Anthropic Messages API (ANTHROPIC_API_KEY), or none (export and scaffold only)."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_CLI_MODEL = "opus"
DEFAULT_API_MODEL = "claude-opus-5-5"  # what the CLI's "opus" alias answered with when this was written
API_URL = "https://api.anthropic.com/v1/messages"


class EngineError(RuntimeError):
    pass


@dataclass
class Reply:
    text: str
    seconds: float
    input_tokens: int = 0
    output_tokens: int = 0
    cache_tokens: int = 0  # cache writes + reads
    cost_usd: float | None = None  # what the provider reports (list price for the CLI)
    raw: dict = field(default_factory=dict)


def find_claude_cli() -> str | None:
    found = shutil.which("claude")
    if found:
        return found
    home = Path.home() / ".local" / "bin"
    for name in ("claude.exe", "claude"):
        if (home / name).is_file():
            return str(home / name)
    return None


def pick_engine(requested: str | None) -> str:
    """claude-cli when the CLI is installed, else anthropic-api with a key, else none."""
    if requested:
        if requested not in ("claude-cli", "anthropic-api", "none"):
            raise EngineError(f"unknown engine {requested!r} (claude-cli, anthropic-api, none)")
        return requested
    if find_claude_cli():
        return "claude-cli"
    if os.environ.get("ANTHROPIC_API_KEY"):
        return "anthropic-api"
    return "none"


class ClaudeCli:
    """`claude -p` with the prompt on stdin, no tools, no user customisations (--safe-mode keeps
    CLAUDE.md, skills, hooks and MCP servers out of the context), JSON output for usage."""

    name = "claude-cli"

    def __init__(self, model: str | None, timeout: float = 900.0, exe: str | None = None):
        self.exe = exe or find_claude_cli()
        if not self.exe:
            raise EngineError("the Claude Code CLI (claude) is not on the PATH; install it or use --engine anthropic-api")
        self.model = model or DEFAULT_CLI_MODEL
        self.timeout = timeout

    def ask(self, system: str, prompt: str, cwd: Path) -> Reply:
        argv = [self.exe, "-p", "--safe-mode", "--model", self.model, "--output-format", "json", "--tools", "",
                "--no-session-persistence", "--disable-slash-commands", "--strict-mcp-config",
                "--system-prompt", system]
        start = time.monotonic()
        try:
            done = subprocess.run(argv, input=prompt.encode("utf-8"), capture_output=True, cwd=cwd,
                                  timeout=self.timeout)
        except subprocess.TimeoutExpired as e:
            raise EngineError(f"claude -p did not answer within {self.timeout:.0f} s") from e
        seconds = time.monotonic() - start
        out = done.stdout.decode("utf-8", errors="replace")
        try:
            data = json.loads(out)
        except json.JSONDecodeError as e:
            err = done.stderr.decode("utf-8", errors="replace").strip()
            raise EngineError(f"claude -p exit code {done.returncode}: {(err or out)[:800]}") from e
        if data.get("is_error") or done.returncode != 0:
            raise EngineError(f"claude -p failed: {str(data.get('result') or data.get('subtype'))[:800]}")
        usage = data.get("usage") or {}
        return Reply(
            text=str(data.get("result") or ""),
            seconds=seconds,
            input_tokens=int(usage.get("input_tokens") or 0),
            output_tokens=int(usage.get("output_tokens") or 0),
            cache_tokens=int(usage.get("cache_creation_input_tokens") or 0) + int(usage.get("cache_read_input_tokens") or 0),
            cost_usd=data.get("total_cost_usd"),
            raw={k: data.get(k) for k in ("model", "modelUsage", "duration_ms", "duration_api_ms", "num_turns")},
        )


class AnthropicApi:
    """The Messages API over HTTPS with ANTHROPIC_API_KEY (never logged)."""

    name = "anthropic-api"

    def __init__(self, model: str | None, timeout: float = 900.0, max_tokens: int = 16000):
        self.key = os.environ.get("ANTHROPIC_API_KEY", "")
        if not self.key:
            raise EngineError("ANTHROPIC_API_KEY is not set; use --engine claude-cli or none")
        self.model = model or DEFAULT_API_MODEL
        self.timeout = timeout
        self.max_tokens = max_tokens

    def ask(self, system: str, prompt: str, cwd: Path) -> Reply:
        body = json.dumps({
            "model": self.model,
            "max_tokens": self.max_tokens,
            "system": system,
            "messages": [{"role": "user", "content": prompt}],
        }).encode("utf-8")
        req = urllib.request.Request(API_URL, data=body, method="POST", headers={
            "x-api-key": self.key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        })
        start = time.monotonic()
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", errors="replace")[:800]
            raise EngineError(f"Anthropic API HTTP {e.code}: {detail}") from e
        except (urllib.error.URLError, TimeoutError) as e:
            raise EngineError(f"Anthropic API request failed: {e}") from e
        seconds = time.monotonic() - start
        text = "".join(part.get("text", "") for part in data.get("content", []) if part.get("type") == "text")
        usage = data.get("usage") or {}
        return Reply(
            text=text,
            seconds=seconds,
            input_tokens=int(usage.get("input_tokens") or 0),
            output_tokens=int(usage.get("output_tokens") or 0),
            cache_tokens=int(usage.get("cache_creation_input_tokens") or 0) + int(usage.get("cache_read_input_tokens") or 0),
            raw={"model": data.get("model"), "stop_reason": data.get("stop_reason")},
        )


def make_engine(name: str, model: str | None):
    if name == "claude-cli":
        return ClaudeCli(model)
    if name == "anthropic-api":
        return AnthropicApi(model)
    return None


_FENCE = re.compile(r"```([A-Za-z0-9_+-]*)[ \t]*\r?\n(.*?)```", re.S)


def code_blocks(text: str) -> list[tuple[str, str]]:
    """(language, body) of every fenced block, in order."""
    return [(m.group(1).lower(), m.group(2)) for m in _FENCE.finditer(text)]


def extract_cpp(text: str) -> str | None:
    """The first C++ block of a reply (```cpp, ```c++ or ```cc; an unlabelled block that uses
    ANYGBA_OVERRIDE also counts)."""
    for lang, body in code_blocks(text):
        if lang in ("cpp", "c++", "cc", "cxx") or (lang == "" and "ANYGBA_OVERRIDE" in body):
            return body
    return None


def extract_toml(text: str) -> str | None:
    for lang, body in code_blocks(text):
        if lang == "toml":
            return body
    return None
