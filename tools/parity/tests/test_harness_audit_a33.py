"""Checks of the parity harness tools that had none (a33 map, A33-H1, A33-H3, A33-H9).

trace_plugin.py is run as pytest runs it on OpenWhisper's suite, against a two-line module; the
classification is rebuilt from the OpenWhisper checkout when FIORIVOCE_OW_ROOT names one.
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
import coverage_table  # noqa: E402


@unittest.skipUnless(importlib.util.find_spec("pytest"), "pytest is not installed")
class TracePlugin(unittest.TestCase):
    def test_records_each_distinct_call_with_its_result_or_exception(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "sample_mod.py").write_text(textwrap.dedent("""\
                def half(value, *, scale=1):
                    if value < 0:
                        raise ValueError("negative")
                    return value * scale / 2
                """), encoding="utf-8")
            (root / "test_sample.py").write_text(textwrap.dedent("""\
                import pytest
                from sample_mod import half

                def test_calls():
                    assert half(3) == 1.5
                    assert half(3) == 1.5
                    assert half(4, scale=2) == 4.0
                    with pytest.raises(ValueError):
                        half(-1)
                """), encoding="utf-8")
            out = root / "trace" / "sample_mod.json"
            env = dict(os.environ, PYTHONPATH=os.pathsep.join([str(TOOLS), str(root)]),
                       PYTHONDONTWRITEBYTECODE="1")
            run = subprocess.run(
                [sys.executable, "-B", "-m", "pytest", "-q", "-p", "trace_plugin",
                 "-p", "no:cacheprovider", "--trace-golden", "sample_mod:half",
                 "--trace-out", str(out), str(root / "test_sample.py")],
                cwd=root, env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            traced = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(traced["exitstatus"], 0)
            calls = traced["functions"]["sample_mod.half"]
            # The repeated call is kept once; kwargs, results and exceptions are recorded.
            self.assertEqual(len(calls), 3)
            self.assertEqual(calls[0]["args"], [3])
            self.assertEqual(calls[1]["kwargs"], {"scale": 2})
            self.assertEqual(calls[2], {"args": [-1], "raises": "ValueError"})
            self.assertIn("3 distinct calls", run.stdout)


class CoverageTable(unittest.TestCase):
    def test_every_area_has_a_row_whose_counts_come_from_the_classification(self):
        classification = json.loads((TOOLS / "classification.json").read_text(encoding="utf-8"))
        areas = json.loads((REPO / "docs/port/areas.json").read_text(encoding="utf-8"))
        text = io.StringIO()
        with redirect_stdout(text):
            coverage_table.main([])
        rows = {line.split("|")[1].strip(): line for line in text.getvalue().splitlines()
                if line.startswith("| a")}
        self.assertEqual(set(rows), {key for key in areas if key.startswith("a")})
        for area in rows:
            functions = sum(r["tests"] for r in classification if r["area"] == area)
            self.assertEqual(int(rows[area].split("|")[5]), functions, area)
        self.assertIn(f"test lines: {sum(r['lines'] for r in classification)}", text.getvalue())


class Isolation(unittest.TestCase):
    """ow_isolation.isolate, in a child process because it rewrites the environment."""

    PROBE = textwrap.dedent("""\
        import json, os, sys
        sys.path.insert(0, sys.argv[1])
        import ow_isolation
        from pathlib import Path
        try:
            data = ow_isolation.isolate(Path(sys.argv[2]))
        except ow_isolation.IsolationError as error:
            print(json.dumps({"refused": str(error)}))
        else:
            print(json.dumps({"data": str(data), "env": {k: os.environ.get(k) for k in (
                "OPENWHISPER_DATA_DIR", "LOCALAPPDATA", "APPDATA", "XDG_DATA_HOME",
                "HF_HUB_OFFLINE", "https_proxy", "FIORIVOCE_REAL_LOCALAPPDATA")},
                "bytecode": sys.dont_write_bytecode, "path0": sys.path[0]}))
        """)

    def probe(self, checkout: Path, scratch: Path, appdata: Path) -> dict:
        env = {k: v for k, v in os.environ.items() if k.lower() not in ("https_proxy",)}
        env.update(FIORIVOCE_OW_ROOT=str(checkout), LOCALAPPDATA=str(appdata),
                   https_proxy="http://127.0.0.1:9")
        env.pop("FIORIVOCE_REAL_LOCALAPPDATA", None)
        run = subprocess.run([sys.executable, "-I", "-B", "-c", self.PROBE, str(TOOLS),
                              str(scratch)], env=env, capture_output=True, text=True,
                             timeout=60)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def test_every_openwhisper_path_moves_into_scratch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            checkout, scratch, appdata = root / "ow", root / "scratch", root / "appdata"
            checkout.mkdir()
            (checkout / "config.py").write_text("", encoding="utf-8")
            result = self.probe(checkout, scratch, appdata)
            env = result["env"]
            self.assertEqual(Path(result["data"]), scratch / "ow-data")
            self.assertEqual(env["OPENWHISPER_DATA_DIR"], str(scratch / "ow-data"))
            for name in ("LOCALAPPDATA", "APPDATA", "XDG_DATA_HOME"):
                self.assertEqual(env[name], str(scratch / "ow-appdata"), name)
            self.assertEqual(env["HF_HUB_OFFLINE"], "1")
            self.assertIsNone(env["https_proxy"])
            # The owner's real folder is remembered for check_isolation, never written.
            self.assertEqual(env["FIORIVOCE_REAL_LOCALAPPDATA"], str(appdata))
            self.assertTrue(result["bytecode"])
            self.assertEqual(Path(result["path0"]), checkout.resolve())

    def test_scratch_inside_the_checkout_or_the_real_data_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            checkout, appdata = root / "ow", root / "appdata"
            checkout.mkdir()
            (checkout / "config.py").write_text("", encoding="utf-8")
            inside = self.probe(checkout, checkout / "scratch", appdata)
            self.assertIn("is inside the checkout", inside["refused"])
            owner = self.probe(checkout, appdata / "OpenWhisper" / "scratch", appdata)
            self.assertIn("is inside", owner["refused"])
            self.assertFalse((checkout / "scratch").exists())
            missing = self.probe(root / "nothing", root / "scratch", appdata)
            self.assertIn("not an OpenWhisper checkout", missing["refused"])


@unittest.skipUnless(os.environ.get("FIORIVOCE_OW_ROOT"), "FIORIVOCE_OW_ROOT is not set")
class Classification(unittest.TestCase):
    def test_a_rerun_on_the_reference_checkout_gives_the_committed_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            run = subprocess.run(
                [sys.executable, "-I", "-B", str(TOOLS / "classify_tests.py"),
                 os.environ["FIORIVOCE_OW_ROOT"], str(REPO / "docs/port/areas.json"), temp],
                capture_output=True, text=True, timeout=600)
            self.assertEqual(run.returncode, 0, run.stderr)
            for name in ("classification.json", "classification.md"):
                self.assertEqual((Path(temp) / name).read_bytes(),
                                 (TOOLS / name).read_bytes(), name)


if __name__ == "__main__":
    unittest.main()
