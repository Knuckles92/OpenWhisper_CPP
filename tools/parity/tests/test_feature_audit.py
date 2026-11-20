import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import differences
import feature_audit
import parity_report


class Audit(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.maps = self.root / "docs/port/map"
        self.records = self.root / "docs/port/audit"
        self.maps.mkdir(parents=True)
        self.records.mkdir()
        (self.maps / "a05-audio.md").write_text("| A05-100a | audio |\n")
        (self.maps / "a32-ui.md").write_text("- [ ] SP2-03 See the device.\n")

    def write(self, name, features, checklist=None):
        record = dict(area="a05-audio", package="M10.7", features=features,
                      checklist=checklist or {}, differences=[])
        (self.records / name).write_text(json.dumps(record))

    def test_references_and_missing_checklist(self):
        item = {"status": "verified", "cpp": ["no.cpp"], "tests": ["no.cpp"]}
        self.write("one.json", {"A05-100a": item})
        result = feature_audit.audit(self.root)
        self.assertEqual(result["missing"], ["SP2-03"])
        self.assertTrue(any("A05-100a: missing cpp" in e for e in result["errors"]))
        self.assertFalse(feature_audit.audit(self.root, {"a05"})["missing"])

    def test_duplicate_invalid_status_and_final_handoff(self):
        self.write("one.json", {"A05-100a": {"status": "invalid"}})
        self.write("two.json", {"A05-100a": {"status": "handoff", "to": "M10.7", "note": "Finish"}})
        errors = feature_audit.audit(self.root, final=True)["errors"]
        for text in ("duplicate", "invalid status", "handoff remains"):
            self.assertTrue(any("A05-100a" in e and text in e for e in errors))

    def test_strict_uses_merged_evidence(self):
        path = self.root / "tools/parity/classification.json"
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps([dict(file="test_a.py", tests=1, area="a05-audio", kind="unit")]))
        self.write("one.json", {"A05-100a": {"status": "dropped", "reason": "Retired."}},
                   {"SP2-03": {"status": "other-platform", "reason": "Mac widget."}})
        parity = self.root / "tests/first/PARITY.json"
        parity.parent.mkdir(parents=True)
        def write(entry, dest):
            dest.write_text(json.dumps({"package": "M10.7", "files": {"tests/test_a.py": {"test_a": entry}}}))
        args = ["--root", str(self.root), "--check", "--strict"]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(parity_report.main(args), 1)
            write(dict(status="pending", owner="M10.7", reason="Work"), parity)
            self.assertEqual(parity_report.main(args), 1)
            other = self.root / "tests/second/PARITY.json"
            other.parent.mkdir()
            write(dict(status="other-platform", reason="Mac widget."), other)
            self.assertEqual(parity_report.main(args), 0)

    def test_differences_sorted_by_area_and_id(self):
        def row(area, key):
            return dict(area=area, id=key, openwhisper="Reference.", openwhisper-cpp="Port.",
                        reason="Different platform.", source="Design")
        report = differences.render([row("b", "x"), row("a", "q"), row("a", "p")])
        self.assertLess(report.index("### p"), report.index("### q"))
        self.assertLess(report.index("### q"), report.index("### x"))
