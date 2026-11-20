"""Self-tests of the parity scripts that run without OpenWhisper (CTest support.parity_tools)."""
from __future__ import annotations

import json
import math
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import check_isolation  # noqa: E402
import golden_format  # noqa: E402
import parity_report  # noqa: E402
import render_reference  # noqa: E402

ROWS = [
    {"file": "test_a.py", "lines": 10, "tests": 3, "area": "a01-app-shell", "otherAreas": [],
     "kind": "pure-logic", "qt": False},
    {"file": "test_b.py", "lines": 10, "tests": 2, "area": "a02-ui-foundation", "otherAreas": [],
     "kind": "qt-widget", "qt": True},
]


class GoldenFormat(unittest.TestCase):
    def test_round_trip_of_tagged_floats(self):
        value = [1.5, math.inf, -math.inf, {"k": None}, "é"]
        encoded = golden_format.encode(value)
        self.assertEqual(encoded[1], {"$float": "inf"})
        self.assertEqual(golden_format.decode(encoded), value)
        nan = golden_format.decode(golden_format.encode(math.nan))
        self.assertTrue(math.isnan(nan))

    def test_repr_values_are_kept_but_not_decoded(self):
        encoded = golden_format.encode(object())
        self.assertIn("$repr", encoded)
        with self.assertRaises(golden_format.Unencodable):
            golden_format.decode(encoded)

    def test_files_are_lf_utf8_with_indent_one(self):
        text = golden_format.dumps({"module": "m", "functions": {"f": [{"args": ["ß"]}]}})
        self.assertNotIn("\r", text)
        self.assertIn('"ß"', text)
        self.assertTrue(text.endswith("}\n"))


class AssetRewrite(unittest.TestCase):
    def test_checkout_urls_become_resource_urls(self):
        sheet = ("QCheckBox::indicator { image: url(D:/ow/ui_qt/assets/check.svg); }\n"
                 "QComboBox::down-arrow { image: url(\"d:/OW/ui_qt/assets/tabler/x.svg\"); }")
        out = render_reference.rewrite_asset_urls(sheet, Path("D:/ow"))
        self.assertIn("url(:/ui_theme/assets/check.svg)", out)
        self.assertIn('url(":/ui_theme/assets/tabler/x.svg")', out)

    def test_a_leftover_checkout_path_fails(self):
        with self.assertRaises(SystemExit):
            render_reference.rewrite_asset_urls("/* D:/ow/notes */", Path("D:/ow"))


class ParityReport(unittest.TestCase):
    def make_repo(self, documents: dict[str, dict]) -> Path:
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (root / "tools" / "parity").mkdir(parents=True)
        (root / "tools" / "parity" / "classification.json").write_text(json.dumps(ROWS))
        (root / "tests" / "x").mkdir(parents=True)
        (root / "tests" / "x" / "test_a.cpp").write_text("")
        for folder, document in documents.items():
            (root / "tests" / folder).mkdir(parents=True, exist_ok=True)
            (root / "tests" / folder / "PARITY.json").write_text(json.dumps(document))
        return root

    def test_most_finished_status_wins_across_packages(self):
        early = {"package": "M0.6", "files": {"tests/test_a.py": {
            "test_one": {"status": "pending", "owner": "M1.1", "reason": "later"},
            "test_two": {"status": "pending", "owner": "M1.1", "reason": "later"}}}}
        late = {"package": "M1.1", "files": {"tests/test_a.py": {
            "test_one": {"status": "ported", "cpp": "tests/x/test_a.cpp"},
            "test_two": {"status": "dropped", "reason": "engine"}}}}
        merged = parity_report.merge([("a", early), ("b", late)])["test_a.py"]
        self.assertEqual(merged["test_one"]["status"], "ported")
        self.assertEqual(merged["test_two"]["status"], "dropped")
        self.assertEqual(merged["test_one"]["packages"], ["M0.6", "M1.1"])
        only_pending = parity_report.merge([("a", early)])["test_a.py"]
        self.assertEqual(only_pending["test_one"]["status"], "pending")

    def test_report_lists_every_file_and_unassigned_ones(self):
        root = self.make_repo({"x": {"package": "M0.9", "files": {"tests/test_a.py": {
            "test_one": {"status": "ported", "cpp": "tests/x/test_a.cpp"}}}}})
        out = root / "report.md"
        self.assertEqual(parity_report.main(["--root", str(root), "--out", str(out)]), 0)
        report = out.read_text(encoding="utf-8")
        self.assertIn("| test_a.py | pure-logic | 3 | in progress | 1 |", report)
        self.assertIn("| test_b.py | qt-widget | 2 | unassigned |", report)

    def test_schema_errors_fail_the_check(self):
        root = self.make_repo({"x": {"package": "M0.9", "files": {
            "tests/test_a.py": {
                "test_one": {"status": "pending", "owner": "M1.1"},
                "test_two": {"status": "ported", "cpp": "tests/x/missing.cpp"},
                "test_three": {"status": "finished"}},
            "tests/test_unknown.py": {}}}})
        problems = parity_report.validate(
            root, "tests/x/PARITY.json",
            json.loads((root / "tests" / "x" / "PARITY.json").read_text()),
            {"test_a.py", "test_b.py"})
        joined = "\n".join(problems)
        self.assertIn('test_one: a pending entry needs "reason"', joined)
        self.assertIn("missing.cpp does not exist", joined)
        self.assertIn("test_three: status must be one of", joined)
        self.assertIn("test_unknown.py: not an OpenWhisper test file", joined)
        self.assertEqual(parity_report.main(["--root", str(root), "--check"]), 1)

    def test_the_committed_parity_files_are_valid(self):
        self.assertEqual(parity_report.main(["--check"]), 0)


class Isolation(unittest.TestCase):
    def test_watches_the_real_local_appdata_after_isolation_moved_it(self):
        real = Path(self.enterContext(tempfile.TemporaryDirectory()))
        moved = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (real / "OpenWhisper").mkdir()
        (moved / "OpenWhisper").mkdir()
        self.enterContext(mock.patch.dict(os.environ))
        os.environ.pop("OPENWHISPER_CPP_OW_ROOT", None)
        os.environ["LOCALAPPDATA"] = str(moved)
        os.environ["OPENWHISPER_CPP_REAL_LOCALAPPDATA"] = str(real)
        self.assertEqual(check_isolation.watched_roots(None), [real / "OpenWhisper"])
        del os.environ["OPENWHISPER_CPP_REAL_LOCALAPPDATA"]
        self.assertEqual(check_isolation.watched_roots(None), [moved / "OpenWhisper"])

    def test_lists_only_entries_newer_than_the_start(self):
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (root / "old.txt").write_text("x")
        time.sleep(0.05)
        since = time.time()
        time.sleep(0.05)
        self.assertEqual(check_isolation.newer_than([root], since), [])
        (root / "sub").mkdir()
        (root / "sub" / "new.txt").write_text("y")
        changed = {p.name for _, p in check_isolation.newer_than([root], since)}
        self.assertIn("new.txt", changed)
        self.assertIn("sub", changed)


class RecordTranscripts(unittest.TestCase):
    def test_all_lists_only_fastapi_scenarios(self):
        # --all loads each listed file and calls its build(); a self-recording scenario (one
        # with no build()) listed here makes the whole run fail.
        import record_transcripts

        names = record_transcripts.all_scenarios()
        self.assertIn("agent_api", names)
        self.assertNotIn("channel", names)
        for name in names:
            text = (record_transcripts.SCENARIOS / f"{name}.py").read_text(encoding="utf-8")
            self.assertIn("def build(", text, name)


if __name__ == "__main__":
    unittest.main()
