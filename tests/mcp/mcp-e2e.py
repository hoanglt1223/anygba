"""End-to-end test of agent control: a recompiled corpus game driven through anygba-mcp over stdio.

    python tests/mcp/mcp-e2e.py --mcp <anygba-mcp> --game <game exe> --rom <rom> --work <dir>

Launches the game headless with control and the journal on, then over MCP: pauses, looks
(screenshot, state, registers, disassembly), plays frame by frame, checks the input reaches
KEYINPUT, saves and loads a state and compares frame hashes, writes memory, reads named symbols,
uses cheats, watches and breakpoints, toggles the journal and the control server off and on
without a restart, and quits. Then it checks that the journal holds every call, that a wrong
token and a non-loopback bind are refused, and that read-only mode blocks play.
"""

import argparse
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


class Failure(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Failure(what)


class Mcp:
    def __init__(self, exe, control_dir, journal_dir):
        self.proc = subprocess.Popen(
            [exe, "--control-dir", str(control_dir), "--journal-dir", str(journal_dir)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr, bufsize=0)
        self.next_id = 1
        self.game_calls = []  # tools that reached the game, in order, with the journal state

    def request(self, method, params=None):
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.next_id += 1
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            check(line, f"anygba-mcp closed stdout during {method}")
            reply = json.loads(line)
            if reply.get("id") == msg["id"]:
                return reply

    def notify(self, method):
        self.proc.stdin.write((json.dumps({"jsonrpc": "2.0", "method": method}) + "\n").encode())
        self.proc.stdin.flush()

    def call(self, tool, args=None, expect_error=False):
        reply = self.request("tools/call", {"name": tool, "arguments": args or {}})
        check("result" in reply, f"{tool}: JSON-RPC error {reply.get('error')}")
        result = reply["result"]
        if expect_error:
            check(result.get("isError"), f"{tool} {args} should have failed: {result}")
            return result["content"][0]["text"]
        if result.get("isError"):
            raise Failure(f"{tool} {args} failed: {result['content'][0]['text']}")
        texts = [c["text"] for c in result["content"] if c["type"] == "text"]
        data = json.loads(texts[-1]) if texts else None
        images = [c for c in result["content"] if c["type"] == "image"]
        return (data, images) if images else data

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=20)


def kill(pid):
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    else:
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass


def journal_records(journal_dir):
    records = []
    for f in sorted(Path(journal_dir).glob("*/*/session.jsonl")):
        for line in f.read_text(encoding="utf-8").splitlines():
            if line.strip():
                records.append(json.loads(line))
    return records


def wait_for(pred, seconds, what):
    end = time.time() + seconds
    while time.time() < end:
        value = pred()
        if value:
            return value
        time.sleep(0.2)
    raise Failure(f"timed out waiting for {what}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mcp", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()

    work = Path(a.work)
    shutil.rmtree(work, ignore_errors=True)
    control_dir, journal_dir, saves = work / "control", work / "journal", work / "saves"
    for d in (control_dir, journal_dir, saves):
        d.mkdir(parents=True)

    # Named memory for the test: a struct array at the start of EWRAM, a pointer, XOR, text.
    symbols = work / "test-symbols.toml"
    symbols.write_bytes(b"""
[symbols.key]
address = 0x02000100
type = "u32"
[symbols.score]
address = 0x02000104
type = "u32"
xor = "key"
journal = true
[symbols.name]
address = 0x02000108
type = "text"
size = 8
charset = "pokemon"
[symbols.team]
pointer = 0x02000110
address = 0x10
type = "struct"
size = 4
count = 2
journal = true
fields.hp = { offset = 0, type = "u16" }
fields.level = { offset = 2, type = "u8" }
fields.flag = { offset = 3, type = "u8", bit = 1 }
""")
    # The user's F10 turns AI control back on at host frame 1500 (emulated frames run 60 behind after
    # the state load below).
    script = work / "user.txt"
    script.write_bytes(b"1500 key tap F10\n")

    # A non-loopback bind is refused before anything starts.
    bad = subprocess.run([a.game, a.rom, "--headless", "--control", "--control-host", "0.0.0.0", "--frames", "5",
                          "--no-save", "--no-miss-log"], capture_output=True, text=True, timeout=60)
    check(bad.returncode == 2 and "only listens on 127.0.0.1" in bad.stderr, f"non-loopback bind accepted: {bad.stderr}")

    m = Mcp(a.mcp, control_dir, journal_dir)
    launched = []
    try:
        init = m.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                        "clientInfo": {"name": "mcp-e2e", "version": "1"}})
        check(init["result"]["serverInfo"]["name"] == "anygba-mcp", "initialize")
        m.notify("notifications/initialized")
        names = {t["name"] for t in m.request("tools/list")["result"]["tools"]}
        for need in ["screenshot", "get_state", "read_memory", "read_named", "press", "hold", "release", "input_sequence",
                     "step_frames", "pause", "resume", "set_speed", "save_state", "load_state", "list_cheats", "set_cheat",
                     "write_memory", "get_registers", "disassemble", "watch_memory", "break_at_function",
                     "get_call_stack", "get_miss_log", "tail_log", "journal_search", "journal_note", "journal_summary",
                     "list_games", "attach", "launch", "set_journal", "set_control"]:
            check(need in names, f"tool {need} missing")
        check(m.call("list_games")["games"] == [], "no games should run yet")
        m.call("attach", {}, expect_error=True)

        info = m.call("launch", {"exe": a.game, "headless": True, "journal": True,
                                 "args": [a.rom, "--pace", "--speed", "400", "--save-dir", str(saves), "--no-miss-log",
                                          "--symbols", str(symbols), "--script", str(script),
                                          "--journal-snapshot", "1", "--journal-thumbnail", "2"]})
        launched.append(info["pid"])
        check(info["attached"] and info["protocol"] == 1, f"launch: {info}")
        games = m.call("list_games")["games"]
        check(len(games) == 1 and games[0]["attached"], f"list_games: {games}")
        token_file = next(control_dir.glob("*.json"))
        token = json.loads(token_file.read_text())

        # A wrong token is refused and the connection closed.
        with socket.create_connection(("127.0.0.1", token["port"]), timeout=10) as s:
            s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"hello","params":{"token":"wrong"}}\n')
            reply = json.loads(s.makefile().readline())
            check(reply["error"]["code"] == -32001, f"wrong token: {reply}")
            check(s.recv(1) == b"", "connection stays open after a wrong token")
        # So is a command before hello.
        with socket.create_connection(("127.0.0.1", token["port"]), timeout=10) as s:
            s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"get_state"}\n')
            check(json.loads(s.makefile().readline())["error"]["code"] == -32001, "command before hello accepted")

        m.call("pause")
        st = m.call("get_state")
        check(st["paused"] and st["journal"], f"pause: {st}")
        f0 = st["frame"]
        shot, images = m.call("screenshot")
        png = base64.b64decode(images[0]["data"])
        check(png[:8] == b"\x89PNG\r\n\x1a\n" and shot["width"] == 240, "screenshot is not a 240x160 PNG")
        check(shot["frame_hash"] == st["frame_hash"], "screenshot and state disagree")
        check(m.call("get_state")["frame"] == f0, "a paused game advanced")
        half, _ = m.call("screenshot", {"scale": 0.5})
        check(half["width"] == 120, "scaled screenshot")

        r = m.call("press", {"buttons": ["A"], "frames": 5})
        check(r["frames"] == 5 and r["frame"] == f0 + 5, f"press: {r}")
        check(m.call("step_frames", {"frames": 10})["frame"] == f0 + 15, "step_frames")
        m.call("hold", {"buttons": "A+Up"})
        m.call("step_frames", {"frames": 2})
        keys = m.call("read_memory", {"address": "0x04000130", "length": 2})
        check(keys["value"] & 0x41 == 0, f"held A+Up not in KEYINPUT: {keys}")  # active low
        check(m.call("get_state")["agent_buttons"] == "A+Up", "hold")
        m.call("release", {})
        m.call("step_frames", {"frames": 1})
        check(m.call("read_memory", {"address": 0x04000130, "length": 2})["value"] & 0x41 == 0x41, "release")
        seq = m.call("input_sequence", {"steps": [{"buttons": "B", "frames": 2}, {"buttons": [], "frames": 3}]})
        check(seq["frames"] == 5, f"input_sequence: {seq}")

        # Save, play on, load: the same frames follow.
        saved = m.call("save_state", {"slot": 1})
        check(Path(saved["file"]).exists(), "state file")
        named = m.call("save_state", {"name": "before-test"})
        h0 = m.call("get_state")["frame_hash"]
        m.call("step_frames", {"frames": 60})
        st1 = m.call("get_state")
        check(st1["frame_hash"] != h0, "the picture does not change; the state test would prove nothing")
        loaded = m.call("load_state", {"slot": 1})
        check(loaded["frame"] == saved["frame"], f"load_state frame {loaded} vs {saved}")
        m.call("step_frames", {"frames": 60})
        st2 = m.call("get_state")
        check(st2["frame"] == st1["frame"] and st2["frame_hash"] == st1["frame_hash"],
              f"frames after loading differ: {st1['frame_hash']} vs {st2['frame_hash']}")
        states = m.call("list_states")["states"]
        check({"slot": 1} in [{"slot": s["slot"]} for s in states if "slot" in s] and
              any(s.get("name") == "before-test" for s in states), f"list_states: {states}")
        m.call("load_state", {"slot": 7}, expect_error=True)

        # Memory and named symbols.
        w = m.call("write_memory", {"address": "0x02000100", "hex": "0f000000" "0a000000" "c6d4cbc7c3c8ff00"
                                    "10010002" "00000000" "00000000" "00000000"})
        check(w["bytes"] == 32, f"write_memory: {w}")
        m.call("write_memory", {"address": 0x02000120, "hex": "2c00" "07" "02" "0900" "03" "00"})
        check(m.call("read_named", {"name": "score"})["value"] == 5, "xor symbol")
        check(m.call("read_named", {"name": "team[0].hp"})["value"] == 44, "struct field through a pointer")
        team = m.call("read_named", {"name": "team"})["value"]
        check(team == [{"hp": 44, "level": 7, "flag": True}, {"hp": 9, "level": 3, "flag": False}], f"team: {team}")
        check(m.call("read_named", {"name": "name"})["value"] == "LZQMIN", "text symbol")
        m.call("read_named", {"name": "nope"}, expect_error=True)
        check(len(m.call("list_symbols")["symbols"]) == 4, "list_symbols")
        m.call("write_memory", {"address": "0x08000000", "value": 1}, expect_error=True)

        # Debug views.
        regs = m.call("get_registers")
        check(regs["r15"].startswith("0x"), "registers")
        dis = m.call("disassemble", {"count": 4})
        check(len(dis["lines"]) == 4 and dis["lines"][0]["text"], f"disassemble: {dis}")
        check("return_addresses" in m.call("get_call_stack"), "call stack")
        check("count" in m.call("get_miss_log"), "miss log")

        # Cheats: a raw code writes EWRAM every frame.
        c = m.call("set_cheat", {"code": ["02000140:BEEF"], "format": "raw", "name": "test"})
        m.call("step_frames", {"frames": 2})
        check(m.call("read_memory", {"address": "0x02000140", "length": 2})["value"] == 0xBEEF, "cheat did not write")
        check(any(x["name"] == "test" and x["enabled"] for x in m.call("list_cheats")["cheats"]), "list_cheats")
        check("test" in m.call("get_state")["active_cheats"], "active cheats")
        m.call("set_cheat", {"index": c["index"], "enabled": False})

        # A watch that stops the game when the interrupt handler acknowledges VBlank (BIOS IF copy).
        wid = m.call("watch_memory", {"address": "0x03007FF8", "length": 2, "on": "write", "action": "stop"})["id"]
        m.call("resume")
        st = wait_for(lambda: (lambda s: s if s["paused"] and "stop_reason" in s else None)(m.call("get_state")), 20, "watch stop")
        check(st["stop_reason"]["reason"] == "watch" and st["stop_reason"]["id"] == wid, f"watch: {st}")
        check(any("watch" in l for l in m.call("tail_log", {"lines": 20})["lines"]), "watch hit not logged")
        check(m.call("list_debug")["watches"][0]["hits"] >= 1, "watch hits")
        m.call("clear_debug", {"id": wid})
        m.call("step_frames", {"frames": 1})  # from a stop back to a frame boundary

        # A breakpoint at the interrupt handler (interpreted or compiled entry).
        handler = m.call("read_memory", {"address": "0x03007FFC", "length": 4})["value"]
        bp = m.call("break_at_function", {"address": handler, "action": "stop"})
        m.call("resume")
        st = wait_for(lambda: (lambda s: s if s["paused"] and "stop_reason" in s else None)(m.call("get_state")), 20, "breakpoint")
        check(st["stop_reason"]["reason"] == "breakpoint", f"breakpoint: {st}")
        m.call("clear_debug", {})
        m.call("pause")
        m.call("step_frames", {"frames": 1})

        # Speed.
        m.call("set_speed", {"percent": 200})
        check(m.call("get_state")["speed"] == 200, "set_speed")
        m.call("set_speed", {"percent": 400})
        m.call("set_speed", {"percent": 150}, expect_error=True)

        # Journal off and on without a restart.
        before = m.call("get_state")["journal_session"]
        m.call("set_journal", {"enabled": False})
        m.call("journal_note", {"text": "while off"}, expect_error=True)
        m.call("press", {"buttons": "B", "frames": 1})  # not journaled
        j = m.call("set_journal", {"enabled": True})
        check(j["enabled"] and j["session"], f"set_journal: {j}")
        m.call("journal_note", {"text": "reach the second screen", "kind": "goal"})
        m.call("journal_note", {"text": "B does nothing here"})
        summary = m.call("journal_summary")
        check(summary["goals"][0]["text"] == "reach the second screen", f"summary: {summary}")
        found = m.call("journal_search", {"method": "load_state"})["records"]
        check(found and found[0]["method"] == "load_state", f"journal_search: {found}")
        check(any(r["kind"] == "note" for r in m.call("journal_search", {"text": "does nothing"})["records"]), "search notes")

        # Control off: the server stops and the token file goes away; the user's F10 (script) turns it on again.
        m.call("set_control", {"enabled": False})
        check(not list(control_dir.glob("*.json")), "token file left behind")
        m.call("get_state", expect_error=True)
        wait_for(lambda: list(control_dir.glob("*.json")), 60, "AI control back on (script F10)")
        again = m.call("attach", {})
        check(again["attached"], "re-attach")
        check(json.loads(next(control_dir.glob("*.json")).read_text())["token"] != token["token"], "token not renewed")
        st = m.call("get_state")
        check(not st["paused"] and st["frame"] >= 1400, f"after re-enable: {st}")
        m.call("pause")
        m.call("quit")
        wait_for(lambda: not list(control_dir.glob("*.json")), 30, "game exit")
        check(m.call("list_games")["games"] == [], "game still listed")

        # Offline journal access, after the game ended.
        offline = m.call("journal_summary", {"game": again["game"]})
        check(offline["ended_cleanly"], f"offline summary: {offline}")
        m.call("detach")

        # Read-only mode: observe yes, play no.
        m.call("launch", {"exe": a.game, "headless": True, "readonly": True,
                          "args": [a.rom, "--no-save", "--no-miss-log", "--pace"]})
        err = m.call("press", {"buttons": "A"}, expect_error=True)
        check("read-only" in err, f"read-only press: {err}")
        m.call("screenshot")
        m.call("watch_memory", {"address": "0x02000000", "action": "stop"}, expect_error=True)
        launched.append(m.call("list_games")["games"][0]["pid"])
        m.call("set_control", {"enabled": False})
    finally:
        m.close()
        for pid in launched:  # games still running after a failure, and the read-only one
            kill(pid)

    # Every call the game answered while the journal was on is in the journal, in order.
    records = journal_records(journal_dir)
    commands = [r["method"] for r in records if r["kind"] == "command"]
    for need in ["pause", "screenshot", "press", "step_frames", "hold", "release", "input_sequence", "save_state",
                 "load_state", "write_memory", "read_named", "set_cheat", "watch_memory", "break_at_function",
                 "set_speed", "set_journal", "journal_summary", "set_control"]:
        check(need in commands, f"journal lacks {need}")
    check(not any(r.get("params") == {"buttons": "B", "frames": 1} for r in records), "a call made with the journal off was journaled")
    kinds = {r["kind"] for r in records}
    for need in ["session_start", "session_end", "snapshot", "thumbnail", "goal", "note", "stop", "control"]:
        check(need in kinds, f"journal lacks a {need} record")
    check(any(r["kind"] == "command" and r["method"] == "write_memory" and "before" in r["result"] for r in records),
          "memory writes are not journaled with their old value")
    check(any(r["kind"] == "command" and "error" in r for r in records), "errors are not journaled")
    check(any(r["kind"] == "control" and r["who"] == "user" and r["enabled"] for r in records), "user toggle not journaled")
    check(not any("token" in json.dumps(r.get("params", {})) for r in records), "a token reached the journal")
    sessions = list(journal_dir.glob("*/*/session.jsonl"))
    check(len(sessions) == 2, f"expected two sessions (journal toggled), got {len(sessions)}")
    thumbs = list(journal_dir.glob("*/*/thumbs/*.png"))
    check(thumbs, "no thumbnails")
    print(f"mcp-e2e: ok ({len(records)} journal records, {len(commands)} commands, {len(thumbs)} thumbnails)")


if __name__ == "__main__":
    try:
        main()
    except Failure as e:
        print(f"mcp-e2e: FAILED: {e}")
        sys.exit(1)
