"""Unit tests for tools/decomp (ctest decomp-tool): parsing of generated code, replies, specs and
symbols, batch order, the module files, and the attempt loop with a stub model and game."""

from __future__ import annotations

import json
import os
import shutil
import struct
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import decomp  # noqa: E402
import engines  # noqa: E402
import generated  # noqa: E402

# The shape of a translation as anygba-recomp writes it (crt0's word fill, simplified).
WORD_FILL = """\
void f_08000186_orig(anygba_cpu* c) {
    AG_ENTER(c, 0x08000186u);
    f_08000186_hits[0]++;
    /* 08000186: movs r2, #3 */
    c->r[2] = 3u;
    /* 08000188: adds r1, r1, r2 */
    /* 0800018a: bics r1, r2 */
    /* 0800018c: beq #0x8000196 */
    AG_SPEND(c, 8, 4, 0);
    AG_TICK(c, 0x0800018cu);
    if (c->z) {
        goto L_08000196;
    }
    f_08000186_hits[1]++;
    /* 0800018e: movs r2, #0 */
    AG_SPEND(c, 8, 1, 0);
    AG_TICK(c, 0x08000190u);
L_08000190:;
    f_08000186_hits[2]++;
    /* 08000190: stm r0!, {r2} */
    /* 08000192: subs r1, #4 */
    /* 08000194: bne #0x8000190 */
    AG_SPEND(c, 8, 3, 1);
    AG_TICK(c, 0x08000194u);
    if (!c->z) {
        goto L_08000190;
    }
L_08000196:;
    f_08000186_hits[3]++;
    /* 08000196: bl #0x8000200 */
    AG_SPEND(c, 8, 1, 2);
    AG_TICK(c, 0x08000196u);
    c->r[14] = 0x0800019bu;
    AG_CALL(c, f_08000200, 0x0800019au);
    /* 0800019a: bx lr */
    AG_SPEND(c, 8, 1, 0);
    AG_TICK(c, 0x0800019au);
    return;
}
"""


class GeneratedTests(unittest.TestCase):
    def test_blocks_and_costs(self):
        blocks = generated.parse_blocks(WORD_FILL, 0x08000186)
        self.assertEqual([b.start for b in blocks], [0x08000186, 0x0800018E, 0x08000190, 0x08000196, 0x0800019A])
        self.assertEqual([(b.insns, b.extra, b.next_pc) for b in blocks],
                         [(4, 0, 0x0800018C), (1, 0, 0x08000190), (3, 1, 0x08000194), (1, 2, 0x08000196),
                          (1, 0, 0x0800019A)])
        self.assertEqual(blocks[0].after, ["if (c->z)", "goto 0x08000196"])
        self.assertIn("call 0x08000200 (lr = 0x0800019b, continues at 0x0800019a)", blocks[3].after)
        text = generated.describe(blocks, 0x08000186)
        self.assertIn("-> g.block(entry + 0x0e, 3, 1)", text)
        self.assertIn("08000192: subs r1, #4", text)

    def test_find_function(self):
        with tempfile.TemporaryDirectory() as d:
            Path(d, "funcs_000.c").write_text("void f_08000100(anygba_cpu* c) {\n}\n" + WORD_FILL)
            self.assertTrue(generated.find_function(Path(d), 0x08000186).startswith("void f_08000186_orig"))
            with self.assertRaises(LookupError):
                generated.find_function(Path(d), 0x08000200)


class ReplyTests(unittest.TestCase):
    def test_code_blocks(self):
        reply = "Here:\n```cpp\nANYGBA_OVERRIDE(x, g) { g.ret(); }\n```\nand\n```toml\n[\"0x1\"]\n```\n"
        self.assertEqual(engines.extract_cpp(reply).strip(), "ANYGBA_OVERRIDE(x, g) { g.ret(); }")
        self.assertEqual(engines.extract_toml(reply).strip(), '["0x1"]')
        self.assertIsNone(engines.extract_cpp("no code"))
        self.assertIsNotNone(engines.extract_cpp("```\nANYGBA_OVERRIDE(y, g) {}\n```"))
        self.assertIsNone(engines.extract_toml("```cpp\nint x;\n```"))

    def test_impl_name_is_enforced(self):
        src = "ANYGBA_OVERRIDE(my_name, g) {\n    g.ret();\n}\n"
        self.assertIn("ANYGBA_OVERRIDE(decomp_StrLen,", decomp.fix_impl_name(src, "decomp_StrLen"))

    def test_run_original_is_not_a_rewrite(self):
        self.assertTrue(decomp.calls_original("ANYGBA_OVERRIDE(x, g) {\n    g.run_original();\n}\n"))
        self.assertFalse(decomp.calls_original("// not g.run_original()\n/* run_original() */\nint x;\n"))

    def test_spec_problems(self):
        good = '["0x08000186"]\n[["0x08000186".shape]]\nr0 = "ram"\n'
        self.assertIsNone(decomp.spec_problem(good, "0x08000186"))
        self.assertIn("not valid TOML", decomp.spec_problem("[[x", "0x08000186"))
        self.assertIn("exactly one table", decomp.spec_problem('[F]\n[["F".shape]]\n', "0x08000186"))
        self.assertIn("at least one", decomp.spec_problem('["0x08000186"]\ndead_stack = 0\n', "0x08000186"))

    def test_spec_from_user_table_round_trips(self):
        table = tomllib.loads('[StrLen]\ndead_stack = 0\n[[StrLen.shape]]\nname = "s"\n'
                              'r0 = { string = "ram", length = [0, 9], terminator = 0 }\n')["StrLen"]
        text = decomp.spec_from_table("0x08006310", table)
        again = tomllib.loads(text)["0x08006310"]
        self.assertEqual(again["dead_stack"], 0)
        self.assertEqual(again["shape"][0]["r0"]["length"], [0, 9])


class InputTests(unittest.TestCase):
    def symbols(self) -> decomp.Symbols:
        s = decomp.Symbols()
        with tempfile.TemporaryDirectory() as d:
            p = Path(d, "s.toml")
            p.write_text('[functions]\nStrLen = { addr = 0x08006310, mode = "thumb" }\n'
                         'Cmp = { addr = 0x0801091c, mode = "arm" }\n')
            s.load(p)
        return s

    def test_parse_func(self):
        s = self.symbols()
        t = decomp.parse_func("StrLen", s, None)
        self.assertEqual((t.addr, t.thumb, t.name, t.known, t.impl, t.file), (0x08006310, True, "StrLen", True,
                                                                              "decomp_StrLen", "StrLen.cpp"))
        self.assertEqual(decomp.parse_func("0x0801091c", s, {0x0801091C: False}).thumb, False)
        t = decomp.parse_func("0x0801091c", s, None)  # a named address: the table gives the mode
        self.assertEqual((t.name, t.thumb), ("Cmp", False))
        t = decomp.parse_func("0x08000187", s, None)
        self.assertEqual((t.addr, t.thumb, t.name), (0x08000186, True, "sub_08000186"))
        self.assertFalse(decomp.parse_func("0x08000300:arm", s, None).thumb)
        with self.assertRaises(decomp.DecompError):
            decomp.parse_func("0x08000300", s, {})
        with self.assertRaises(decomp.DecompError):
            decomp.parse_func("NoSuchName", s, None)

    def test_conflicting_tables(self):
        s = self.symbols()
        with tempfile.TemporaryDirectory() as d:
            p = Path(d, "t.toml")
            p.write_text('[functions]\nStrLen = { addr = 0x08000000 }\n')
            with self.assertRaises(decomp.DecompError):
                s.load(p)

    def test_recording_entry(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d, "x.states")
            p.write_bytes(decomp.RECORDING_MAGIC + struct.pack("<II", 1, 0x08008BA0) + b"\0" * 8)
            self.assertEqual(decomp.recording_entry(p), 0x08008BA0)
            p.write_bytes(b"nope" * 8)
            with self.assertRaises(decomp.DecompError):
                decomp.recording_entry(p)

    def test_callees_first(self):
        a, b, c = (decomp.Target(x, True, n, True) for x, n in ((1, "A"), (2, "B"), (3, "C")))
        exports = {1: {"callees": [{"addr": "0x00000002"}, {"addr": "0x00000009"}]},
                   2: {"callees": [{"addr": "0x00000003"}]},
                   3: {"callees": [{"addr": "0x00000001"}]}}  # a cycle: the given order breaks it
        self.assertEqual([t.name for t in decomp.order_callees_first([a, b, c], exports)], ["C", "B", "A"])
        self.assertEqual([t.name for t in decomp.order_callees_first([c, a], exports)], ["A", "C"])

    def test_data_dir_override(self):
        old = os.environ.get("ANYGBA_DATA_DIR")
        os.environ["ANYGBA_DATA_DIR"] = "X:/data"
        try:
            self.assertEqual(decomp.data_dir(), Path("X:/data"))
        finally:
            if old is None:
                del os.environ["ANYGBA_DATA_DIR"]
            else:
                os.environ["ANYGBA_DATA_DIR"] = old

    def test_engine_choice(self):
        self.assertEqual(engines.pick_engine("none"), "none")
        self.assertIsNone(engines.make_engine("none", None))
        with self.assertRaises(engines.EngineError):
            engines.pick_engine("gpt")


class ModuleTests(unittest.TestCase):
    def test_files(self):
        with tempfile.TemporaryDirectory() as d:
            m = decomp.Module(Path(d, "module"))
            t = decomp.Target(0x08006310, True, "StrLen", True)
            u = decomp.Target(0x0801091C, False, "Cmp", True)
            m.entry(t).update(verified="passed")
            m.entry(u).update(enabled=False)
            m.set_spec(t, '["0x08006310"]\n[["0x08006310".shape]]\nr0 = "ram"\n')
            m.put_source(t, "// x\n", None)
            m.write()
            config = tomllib.loads(Path(d, "module", "overrides.toml").read_text())
            by_addr = {o["addr"]: o for o in config["override"]}
            self.assertEqual(by_addr[0x08006310]["impl"], "decomp_StrLen")
            self.assertTrue(by_addr[0x08006310]["verified"])
            self.assertFalse(by_addr[0x0801091C]["enabled"])
            self.assertEqual(by_addr[0x0801091C]["mode"], "arm")
            spec = tomllib.loads(Path(d, "module", "difftest.toml").read_text())
            self.assertIn("0x08006310", spec)
            self.assertEqual(decomp.Module(Path(d, "module")).state["0x08006310"]["name"], "StrLen")


class StubEngine:
    name = "stub"
    model = "stub-model"

    def __init__(self, replies):
        self.replies = list(replies)
        self.prompts = []

    def ask(self, system, prompt, cwd):
        self.prompts.append(prompt)
        return engines.Reply(text=self.replies.pop(0), seconds=1.0, input_tokens=10, output_tokens=5)


class StubGame:
    def __init__(self, gen: Path, builds, tests):
        self.gen = gen
        self.builds = list(builds)
        self.tests = list(tests)

    def build(self, log: Path) -> bool:
        ok = self.builds.pop(0)
        log.write_text("" if ok else "x.cpp(3): error C2065: 'foo': undeclared identifier\n")
        return ok

    def difftest(self, target, json_out, log, seed, states, recorded):
        status, rep = self.tests.pop(0)
        log.write_text("error: m/difftest.toml:4: r1.range must be an integer\n" if status == "spec-error" else "ok\n")
        return status, rep


def report(status, abi=None):
    return {"states": {"total": 100, "random": 100, "recorded": 0, "passed": 100 if status != "fail" else 90,
                       "returned": 100 if status != "insufficient" else 10,
                       "returned_share": 1.0 if status != "insufficient" else 0.1},
            "abi_differences": abi or {}, "masks": {"min_returned": 0.95},
            "first_failure": {"minimised_input": {"r": [0, 1]},
                              "minimised_differences": [{"key": "r0", "text": "r0: original 0x1, override 0x2"}]}
            if status == "fail" else None,
            "coverage": {"blocks": 3, "covered": 3, "percent": 100.0}}


SPEC = '```toml\n["0x08006310"]\n[["0x08006310".shape]]\nr0 = "ram"\n```\n'
CODE = "```cpp\nANYGBA_OVERRIDE(whatever, g) {\n    g.ret();\n}\n```\n"


class LoopTests(unittest.TestCase):
    def run_loop(self, replies, builds, tests, attempts=4):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        gen = Path(d, "gen")
        gen.mkdir()
        t = decomp.Target(0x08006310, True, "StrLen", True)
        engine = StubEngine(replies)
        game = StubGame(gen, builds, tests)
        module = decomp.Module(Path(d, "module"))
        args = type("A", (), {"attempts": attempts, "seed": 1, "states": 100})()
        ctx = dict(args=args, game=game, module=module, engine=engine, run_dir=Path(d, "run"), title="T",
                   exports={t.addr: {"disassembly": [], "callees": [], "globals": [], "pseudo_c": "int f(void)"}},
                   blocks={t.key: "block 1"}, symbols=decomp.Symbols(), prefix=Path(d), recorded={},
                   scaffolds={t.key: "// scaffold\n"}, spec_fixed={}, min_returned="95%")
        result = decomp.Result(t)
        decomp.decompile_one(t, ctx, result)
        return result, engine, module, gen, t

    def test_retries_until_pass(self):
        result, engine, module, gen, t = self.run_loop(
            ["no code here", SPEC + CODE, CODE, CODE],
            builds=[False, True, True], tests=[("fail", report("fail", {"r0": 10})), ("pass", report("pass"))])
        self.assertEqual(result.status, "pass")
        self.assertEqual([a.outcome for a in result.attempts], ["no-code", "compile-error", "fail:r0", "pass"])
        self.assertIn("undeclared identifier", engine.prompts[2])
        self.assertIn("r0: original 0x1, override 0x2", engine.prompts[3])
        self.assertIn("ANYGBA_OVERRIDE(decomp_StrLen,", (gen / t.file).read_text())
        self.assertIn("anygba decomp: difftest passed", module.state[t.key]["verified"])
        self.assertTrue(module.state[t.key]["enabled"])

    def test_a_candidate_that_runs_the_original_is_rejected(self):
        cheat = "```cpp\nANYGBA_OVERRIDE(x, g) {\n    g.run_original();\n}\n```\n"
        result, engine, module, gen, t = self.run_loop(
            [SPEC + cheat, CODE], builds=[True], tests=[("pass", report("pass"))])
        self.assertEqual([a.outcome for a in result.attempts], ["uses-original", "pass"])
        self.assertIn("do not call g.run_original()", engine.prompts[1])

    def test_a_rejected_spec_goes_back_to_the_model(self):
        result, engine, module, gen, t = self.run_loop(
            [SPEC + CODE, SPEC + CODE], builds=[True, True], tests=[("spec-error", None), ("pass", report("pass"))])
        self.assertEqual([a.outcome for a in result.attempts], ["spec-error", "pass"])
        self.assertIn("r1.range must be an integer", engine.prompts[1])
        self.assertIn("```toml", engine.prompts[1])

    def test_gives_up_and_restores_the_scaffold(self):
        result, engine, module, gen, t = self.run_loop(
            [SPEC + CODE, CODE], builds=[True, True],
            tests=[("insufficient", report("insufficient")), ("error", None)], attempts=2)
        self.assertEqual(result.status, "failed")
        self.assertEqual([a.outcome for a in result.attempts], ["insufficient", "runtime-error"])
        self.assertIn("```toml", engine.prompts[1])  # an insufficient run may revise the spec
        self.assertEqual((gen / t.file).read_text(), "// scaffold\n")
        self.assertFalse(module.state[t.key]["enabled"])


if __name__ == "__main__":
    unittest.main()
