"""Self-check of rebrand.py on small fixtures, and of the shipped webui/ against the real map.

Run: python -I -B -m unittest discover -v -s tools/webui -p "test_rebrand.py"
"""

from __future__ import annotations

import contextlib
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rebrand  # noqa: E402


def rule(file: str, literal: str, replacement: str, count: int) -> dict:
    return {"file": file, "literal": literal, "replacement": replacement, "count": count}


class Fixture(unittest.TestCase):
    """A repository root holding a webui/ with two source files, a dist and a map."""

    RULES = [rule("src/a.ts", "OpenWhisper", "OpenWhisper_CPP", 2),
             rule("dist/index.html", "<b>OpenWhisper</b>", "<b>OpenWhisper_CPP</b>", 1),
             rule("dist/assets/app.js", "openwhisper.log", "openwhisper-cpp.log", 1)]

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.webui = self.root / "webui"
        self.write("src/a.ts", b"OpenWhisper and OpenWhisper\r\nmixed\n")
        self.write("src/other.ts", b"nothing\n")
        self.write("dist/index.html", b"<b>OpenWhisper</b>")
        self.write("dist/assets/app.js", b"log('openwhisper.log')")
        self.write("dist/assets/font.woff2", bytes(range(256)))
        self.map = self.write_map(self.RULES, [])

    def write(self, name: str, data: bytes) -> None:
        path = self.webui / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def write_map(self, rules: list[dict], allowed: list[dict]) -> Path:
        path = self.root / "map.json"
        path.write_text(json.dumps({"replacements": rules, "allowed": allowed}), encoding="utf-8")
        return path

    def run_main(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = rebrand.main(["--root", str(self.root), "--map", str(self.map), *args])
        return status, out.getvalue()

    def snapshot(self) -> dict[str, bytes]:
        return rebrand.read_tree(self.webui)


class Apply(Fixture):
    def test_replaces_exact_counts_and_keeps_line_endings(self) -> None:
        status, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertEqual((self.webui / "src/a.ts").read_bytes(),
                         b"OpenWhisper_CPP and OpenWhisper_CPP\r\nmixed\n")
        self.assertEqual((self.webui / "dist/assets/font.woff2").read_bytes(), bytes(range(256)))

    def test_second_run_changes_nothing(self) -> None:
        self.run_main()
        before = self.snapshot()
        status, out = self.run_main()
        self.assertEqual(status, 0)
        self.assertIn("0 files changed", out)
        self.assertEqual(self.snapshot(), before)

    def test_wrong_count_refuses_and_writes_nothing(self) -> None:
        self.write("dist/assets/app.js", b"log('openwhisper.log'); other('openwhisper.log')")
        before = self.snapshot()
        status, out = self.run_main()
        self.assertEqual(status, 1)
        self.assertIn("occurs 2 times, expected 1", out)
        self.assertEqual(self.snapshot(), before)

    def test_missing_literal_refuses(self) -> None:
        self.write("dist/index.html", b"<b>Something</b>")
        status, out = self.run_main()
        self.assertEqual(status, 1)
        self.assertIn("occurs 0 times", out)

    def test_missing_file_refuses(self) -> None:
        (self.webui / "dist/index.html").unlink()
        status, out = self.run_main()
        self.assertEqual(status, 1)
        self.assertIn("file is missing", out)

    def test_a_rule_an_earlier_one_consumed_is_refused(self) -> None:
        rules = [rebrand.Rule("x", b"ab", b"X", 1), rebrand.Rule("x", b"bc", b"Y", 1)]
        _, _, problems = rebrand.apply_rules(rules, {"x": b"abc"})
        self.assertEqual(problems, ["x: 'bc' occurs 0 times, expected 1"])

    def test_node_modules_is_neither_read_nor_written(self) -> None:
        self.write("node_modules/pkg/index.js", b"OpenWhisper openwhisper.log")
        status, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertEqual((self.webui / "node_modules/pkg/index.js").read_bytes(),
                         b"OpenWhisper openwhisper.log")
        self.run_main("--manifest")
        self.assertEqual(self.run_main("--check")[0], 0)

    def test_half_applied_tree_resumes(self) -> None:
        self.write("src/a.ts", b"OpenWhisper_CPP and OpenWhisper_CPP\r\nmixed\n")
        status, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertEqual((self.webui / "dist/index.html").read_bytes(), b"<b>OpenWhisper_CPP</b>")


class MapRules(Fixture):
    def test_overlapping_literals_are_refused(self) -> None:
        self.map = self.write_map([rule("a", "OpenWhisper", "X", 1),
                                   rule("a", "%\\OpenWhisper", "Y", 1)], [])
        with self.assertRaises(ValueError):
            rebrand.load_map(self.map)

    def test_literal_spanning_lines_is_refused(self) -> None:
        self.map = self.write_map([rule("a", "one\ntwo", "x", 1)], [])
        with self.assertRaises(ValueError):
            rebrand.load_map(self.map)

    def test_shipped_map_loads(self) -> None:
        rules, _ = rebrand.load_map()
        self.assertGreater(len(rules), 15)


class Check(Fixture):
    def test_unrebranded_tree_fails_and_rebranded_passes(self) -> None:
        self.run_main("--manifest")
        status, out = self.run_main("--check")
        self.assertEqual(status, 1)
        self.assertIn("old product name", out)
        self.run_main()
        self.run_main("--manifest")
        self.assertEqual(self.run_main("--check")[0], 0)

    def test_allowed_leftovers_are_counted_exactly(self) -> None:
        self.write("src/other.ts", b"open whisper sample\n")
        self.run_main()
        self.run_main("--manifest")
        self.map = self.write_map(self.RULES, [{"file": "src/other.ts", "count": 1}])
        self.assertEqual(self.run_main("--check")[0], 0)
        self.map = self.write_map(self.RULES, [{"file": "src/other.ts", "count": 2}])
        self.assertEqual(self.run_main("--check")[0], 1)

    def test_replacement_gone_is_reported(self) -> None:
        self.run_main()
        self.run_main("--manifest")
        self.write("src/a.ts", b"OpenWhisper_CPP\n")
        status, out = self.run_main("--check")
        self.assertEqual(status, 1)
        self.assertIn("is not in place 2 times", out)


class Manifest(Fixture):
    def setUp(self) -> None:
        super().setUp()
        self.run_main()
        self.run_main("--manifest")

    def test_lists_dist_files_sorted_with_two_spaces(self) -> None:
        lines = (self.webui / "dist.sha256").read_text(encoding="utf-8").splitlines()
        self.assertEqual([line.split("  ", 1)[1] for line in lines],
                         ["assets/app.js", "assets/font.woff2", "index.html"])
        self.assertTrue(all(len(line.split("  ", 1)[0]) == 64 for line in lines))

    def test_tamper_extra_and_missing_files_are_found(self) -> None:
        self.write("dist/index.html", b"<b>OpenWhisper_CPP</b> ")
        self.write("dist/assets/new.css", b"")
        (self.webui / "dist/assets/font.woff2").unlink()
        problems = rebrand.check_manifest(self.webui, self.snapshot())
        self.assertEqual(sorted(problems), [
            "dist/assets/font.woff2: listed but not in dist",
            "dist/assets/new.css: not in the manifest",
            "dist/index.html: hash differs"])

    def test_manifest_is_missing(self) -> None:
        (self.webui / "dist.sha256").unlink()
        self.assertEqual(rebrand.check_manifest(self.webui, self.snapshot()),
                         ["dist.sha256 is missing"])

    def test_a_malformed_line_is_reported_not_raised(self) -> None:
        path = self.webui / "dist.sha256"
        path.write_bytes(path.read_bytes() + b"garbage\n")
        self.assertEqual(rebrand.check_manifest(self.webui, self.snapshot()),
                         ["dist.sha256 line 4 is not `<sha256>  <path>`"])
        status, out = self.run_main("--check")
        self.assertEqual(status, 1)
        self.assertIn("line 4", out)


class Against(Fixture):
    def make_original(self) -> Path:
        original = self.root / "original"
        rebrand.materialize(self.snapshot(), original)
        (original / "node_modules").mkdir()
        (original / "node_modules" / "x.js").write_bytes(b"OpenWhisper")
        return original

    def test_tree_equals_original_with_the_map(self) -> None:
        original = self.make_original()
        self.run_main()
        self.write("node_modules/y.js", b"installed by npm ci")
        self.assertEqual(self.run_main("--against", str(original))[0], 0)
        self.write("src/other.ts", b"edited\n")
        status, out = self.run_main("--against", str(original))
        self.assertEqual(status, 1)
        self.assertIn("src/other.ts: differs beyond the map", out)

    def test_line_endings_are_named(self) -> None:
        original = self.make_original()
        self.run_main()
        self.write("src/other.ts", b"nothing\r\n")
        status, out = self.run_main("--against", str(original))
        self.assertEqual(status, 1)
        self.assertIn("src/other.ts: differs in line endings only", out)

    def test_missing_original_folder(self) -> None:
        status, out = self.run_main("--against", str(self.root / "nowhere"))
        self.assertEqual(status, 1)
        self.assertIn("not a folder", out)


@unittest.skipUnless(shutil.which("git"), "git is not on PATH")
class AgainstCommit(Fixture):
    """A throwaway repository in the temporary folder stands for OpenWhisper's."""

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), "-c", "user.name=t",
                               "-c", "user.email=t@t", "-c", "core.autocrlf=false", *args],
                              capture_output=True, text=True, check=True).stdout.strip()

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.root / "ow"
        rebrand.materialize(self.snapshot(), self.repo / "webui")
        (self.repo / "webui" / "node_modules").mkdir()
        (self.repo / "webui" / "node_modules" / "x.js").write_bytes(b"OpenWhisper")
        (self.repo / "other.txt").write_bytes(b"not webui")
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "one")
        self.commit = self.git("rev-parse", "HEAD")
        self.git_dir = str(self.repo / ".git")
        self.run_main()

    def test_tree_equals_the_commit_with_the_map(self) -> None:
        status, out = self.run_main("--against-commit", self.git_dir, "--commit", self.commit)
        self.assertEqual(status, 0, out)

    def test_working_copy_edits_after_the_commit_do_not_count(self) -> None:
        (self.repo / "webui" / "src" / "other.ts").write_bytes(b"nothing\r\n")
        status, out = self.run_main("--against-commit", self.git_dir, "--commit", self.commit)
        self.assertEqual(status, 0, out)
        status, out = self.run_main("--against", str(self.repo / "webui"))
        self.assertIn("src/other.ts: differs in line endings only", out)

    def test_a_later_commit_differs(self) -> None:
        (self.repo / "webui" / "src" / "new.ts").write_bytes(b"new\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "two")
        status, out = self.run_main("--against-commit", self.git_dir, "--commit", "HEAD")
        self.assertEqual(status, 1)
        self.assertIn("src/new.ts: missing", out)

    def test_an_unknown_commit_is_reported(self) -> None:
        status, out = self.run_main("--against-commit", self.git_dir, "--commit", "0" * 40)
        self.assertEqual(status, 1)
        self.assertIn("git ls-tree", out)

    def test_default_commit_is_the_reference(self) -> None:
        self.assertRegex(rebrand.reference_commit(), r"^[0-9a-f]{40}$")


class ShippedBundle(unittest.TestCase):
    def test_check_passes(self) -> None:
        rules, allowed = rebrand.load_map()
        self.assertEqual(rebrand.check(rebrand.ROOT / "webui", rules, allowed), [])

    def test_self_test_passes(self) -> None:
        rules, _ = rebrand.load_map()
        self.assertEqual(rebrand.self_test(rebrand.ROOT / "webui", rules), [])

    def test_served_files_are_in_the_manifest(self) -> None:
        manifest = (rebrand.ROOT / "webui" / "dist.sha256").read_text(encoding="utf-8")
        names = [line.split("  ", 1)[1] for line in manifest.splitlines()]
        self.assertIn("index.html", names)
        self.assertEqual(names, sorted(names))
        self.assertTrue(any(name.startswith("licenses/") for name in names))


if __name__ == "__main__":
    unittest.main()
