"""BUILD-METADATA.json and SHA256SUMS.txt as build_metadata.py writes them."""

import contextlib
import hashlib
import io
import json
import re
import unittest

import release_support as rs

import build_metadata as bm
import check_release_qualification as cq
import release_common as common

# The grammar the application's update check reads: a digest, one space, then a space or "*".
UPDATER_LINE = re.compile(r"([a-fA-F0-9]{64}) [ *]([A-Za-z0-9_.-]+)")


class BuildMetadata(rs.TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.directory = self.tmp / "candidate"
        self.directory.mkdir()
        (self.directory / rs.SETUP).write_bytes(b"setup bytes")

    def write(self, **kwargs):
        defaults = dict(version=rs.VERSION, commit=rs.COMMIT, identity=rs.IDENTITY,
                        built_at="2026-10-01T00:00:00Z")
        return bm.write_metadata(self.directory, **{**defaults, **kwargs})

    def test_writes_metadata_and_checksums_the_qualification_accepts(self):
        metadata = self.write()
        self.assertEqual(metadata["schema_version"], 1)
        self.assertEqual(metadata["version"], rs.VERSION)
        self.assertEqual(metadata["commit_sha"], rs.COMMIT)
        self.assertIs(metadata["windows_signed"], False)
        self.assertIs(metadata["windows_setup_only"], True)
        self.assertEqual(metadata["source_qualification"], "not-run")
        self.assertEqual(json.loads((self.directory / "BUILD-METADATA.json").read_text(
            encoding="utf-8")), metadata)
        loaded, assets = cq.candidate_identity(self.directory, rs.IDENTITY)
        self.assertEqual(loaded, metadata)
        self.assertEqual(assets, {rs.SETUP: hashlib.sha256(b"setup bytes").hexdigest()})

    def test_checksums_use_two_spaces_and_the_updaters_parser_agrees(self):
        self.write()
        raw = (self.directory / "SHA256SUMS.txt").read_bytes()
        self.assertNotIn(b"\r", raw)
        self.assertTrue(raw.endswith(b"\n"))
        lines = raw.decode("utf-8").splitlines()
        self.assertEqual(len(lines), 2)
        names = []
        for line in lines:
            self.assertTrue(common.CHECKSUM_LINE_RE.fullmatch(line), line)
            self.assertEqual(line[64:66], "  ")
            match = UPDATER_LINE.fullmatch(line)
            self.assertIsNotNone(match, line)
            digest, name = match.groups()
            self.assertEqual(digest, common.sha256_of(self.directory / name))
            names.append(name)
        self.assertEqual(names, sorted(names))
        self.assertEqual(set(names), {rs.SETUP, "BUILD-METADATA.json"})

    def test_running_twice_replaces_the_previous_files(self):
        first = self.write(source_qualification="not-run")
        second = self.write(source_qualification="passed")
        self.assertNotEqual(first, second)
        self.assertEqual(cq.candidate_identity(self.directory, rs.IDENTITY)[0]
                         ["source_qualification"], "passed")

    def test_optional_fields_appear_only_when_given(self):
        plain = self.write()
        for key in ("source_ref", "repository", "workflow_run_id", "workflow_run_attempt",
                    "workflow_run_url"):
            self.assertNotIn(key, plain)
        full = self.write(source_ref="refs/tags/v0.1.0", repository="owner/name", run_id=7,
                          run_attempt=2, run_url="https://example.invalid/run/7")
        self.assertEqual(
            (full["source_ref"], full["workflow_run_id"], full["workflow_run_attempt"]),
            ("refs/tags/v0.1.0", 7, 2))

    def test_time_stamp_comes_from_source_date_epoch_when_not_given(self):
        metadata = self.write(built_at=None, env={"SOURCE_DATE_EPOCH": "1759276800"})
        self.assertEqual(metadata["built_at_utc"], "2025-10-01T00:00:00Z")
        now = self.write(built_at=None, env={})
        self.assertRegex(now["built_at_utc"], r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ")

    def test_a_native_archive_makes_the_release_not_setup_only(self):
        (self.directory / rs.ARCHIVE).write_bytes(b"archive")
        metadata = self.write()
        self.assertIs(metadata["windows_setup_only"], False)
        self.assertEqual(set(cq.candidate_identity(self.directory, rs.IDENTITY)[1]),
                         {rs.SETUP, rs.ARCHIVE})

    def test_bad_input_fails_before_anything_is_written(self):
        for kwargs in ({"version": "1.0"}, {"version": "v0.1.0"}, {"commit": "abc"},
                       {"commit": "A" * 40}, {"source_qualification": "yes"}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.write(**kwargs)
            self.assertFalse((self.directory / "BUILD-METADATA.json").exists())
        with self.assertRaisesRegex(ValueError, "is not in"):
            self.write(version="0.2.0")
        (self.directory / "notes.txt").write_text("stray", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Not part of a release candidate: notes.txt"):
            self.write()
        self.assertFalse((self.directory / "SHA256SUMS.txt").exists())

    def test_a_signing_report_for_another_build_is_refused(self):
        report = {"schema_version": 1, "signed": False, "method": None, "files": [
            {"name": rs.SETUP, "sha256": "0" * 64, "status": "NotSigned"}]}
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.write(signing_report=report)

    def test_cli(self):
        err = io.StringIO()
        out = io.StringIO()
        arguments = ["--identity", str(rs.IDENTITY_FILE), "--dir", str(self.directory),
                     "--version", rs.VERSION, "--commit", rs.COMMIT,
                     "--source-qualification", "passed", "--built-at", "2026-10-01T00:00:00Z"]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(bm.main(arguments), 0)
            self.assertIn("unsigned", out.getvalue())
            self.assertEqual(bm.main([*arguments[:-4], "--commit", "nope"]), 1)
        self.assertIn("Build metadata failed", err.getvalue())
        self.assertEqual(cq.candidate_identity(self.directory, rs.IDENTITY)[0]
                         ["source_qualification"], "passed")


if __name__ == "__main__":
    unittest.main()
