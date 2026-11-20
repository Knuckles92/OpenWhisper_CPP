"""The release candidate check, over local folders only: no release service, no network."""

import contextlib
import copy
import hashlib
import io
import json
import unittest

import release_support as rs

import build_metadata
import check_release_qualification as cq
import windows_signing as ws

ID_ARG = ["--identity", str(rs.IDENTITY_FILE)]


class Candidate(rs.TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.directory = self.tmp / "candidate"
        self.metadata = rs.write_candidate(self.directory, setup_only=False)
        _, self.assets = cq.candidate_identity(self.directory, rs.IDENTITY)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cq.main([*ID_ARG, "--dir", str(self.directory), *args])
        return code, out.getvalue(), err.getvalue()


class TestReleaseQualification(Candidate):
    def test_tampered_or_extra_candidate_file_fails(self):
        file = self.directory / next(iter(self.assets))
        original = file.read_bytes()
        file.write_bytes(b"different binary")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            cq.candidate_identity(self.directory, rs.IDENTITY)
        file.write_bytes(original)
        (self.directory / "unexpected.exe").write_bytes(b"unverified")
        with self.assertRaisesRegex(ValueError, "unverified"):
            cq.candidate_identity(self.directory, rs.IDENTITY)

    def test_cli_verifies_candidate_without_manual_report(self):
        commit = self.metadata["commit_sha"]
        self.assertEqual(self.run_main("--expected-run", "123", "--expected-commit", commit)[0], 0)
        self.assertEqual(self.run_main("--expected-run", "124")[0], 1)
        self.assertEqual(self.run_main("--expected-commit", "b" * 40)[0], 1)

    def test_draft_rejects_unrelated_and_duplicate_assets(self):
        cq.validate_draft_assets(self.directory, {"assets": []}, identity=rs.IDENTITY)
        with self.assertRaisesRegex(ValueError, "unrelated"):
            cq.validate_draft_assets(self.directory, {"assets": [{"name": "older-installer.exe"}]},
                                     identity=rs.IDENTITY)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            cq.validate_draft_assets(self.directory, {"assets": [{"name": "SHA256SUMS.txt"}] * 2},
                                     identity=rs.IDENTITY)

    def test_uploaded_draft_requires_exact_inventory_and_all_digests(self):
        release = {"assets": [
            {"name": p.name, "digest": "sha256:" + hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in self.directory.iterdir()]}
        cq.validate_draft_assets(self.directory, release, uploaded=True)
        wrong_digest = copy.deepcopy(release)
        wrong_digest["assets"][0]["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            cq.validate_draft_assets(self.directory, wrong_digest, uploaded=True)
        release["assets"].pop()
        with self.assertRaisesRegex(ValueError, "inventory differs"):
            cq.validate_draft_assets(self.directory, release, uploaded=True)

    def test_setup_only_draft_allows_removing_only_its_current_native_archive(self):
        self.metadata["windows_setup_only"] = True
        (self.directory / rs.ARCHIVE).unlink()
        rs.rewrite_metadata(self.directory, self.metadata)
        cq.validate_draft_assets(self.directory, {"assets": [{"name": rs.ARCHIVE}]},
                                 identity=rs.IDENTITY)
        with self.assertRaisesRegex(ValueError, "unrelated"):
            cq.validate_draft_assets(
                self.directory, {"assets": [{"name": "FioriVoce-0.0.9-win64.zip"}]},
                identity=rs.IDENTITY)

    def test_candidate_must_record_windows_signing(self):
        for broken in ({"windows_signed": None}, {"windows_signed": "no"}, {"schema_version": 2},
                       {"windows_signed": True},
                       {"windows_signed": True, "windows_signing": {"method": "x"}}):
            with self.subTest(broken=broken):
                rs.rewrite_metadata(self.directory, {**self.metadata, **broken})
                with self.assertRaises(ValueError):
                    cq.candidate_identity(self.directory, rs.IDENTITY)

    def test_cli_reports_and_can_require_windows_signing(self):
        code, out, _ = self.run_main()
        self.assertEqual(code, 0)
        self.assertIn("unsigned", out)
        self.assertEqual(self.run_main("--require-windows-signed")[0], 1)
        signing = {"method": "trusted-signing", "signer_subject": "CN=Fiori Labs",
                   "signer_thumbprint": "A" * 40}
        rs.rewrite_metadata(self.directory, {**self.metadata, "windows_signed": True,
                                             "windows_signing": signing})
        code, out, _ = self.run_main("--require-windows-signed")
        self.assertEqual(code, 0)
        self.assertIn("signed via trusted-signing by CN=Fiori Labs", out)

    def test_windows_signing_is_optional_verified_and_recorded(self):
        """The signing record travels report, build metadata, qualification, unaltered."""
        report = self.tmp / "windows-signing.json"
        setup = self.directory / rs.SETUP
        (self.directory / rs.ARCHIVE).unlink()
        digest = hashlib.sha256(setup.read_bytes()).hexdigest()

        def report_for(signed):
            status = "Valid" if signed else "NotSigned"
            entry = {"name": rs.SETUP, "sha256": digest, "status": status,
                     "signer_subject": "CN=Fiori Labs", "signer_thumbprint": "A" * 40,
                     "timestamped": True}
            return {"schema_version": 1, "signed": signed,
                    "method": "trusted-signing" if signed else None,
                    "signer_subject": "CN=Fiori Labs" if signed else None,
                    "signer_thumbprint": "A" * 40 if signed else None, "files": [entry]}

        # Unsigned is the default, with or without a report.
        for signing in (None, report_for(False)):
            for name in ("BUILD-METADATA.json", "SHA256SUMS.txt"):
                (self.directory / name).unlink(missing_ok=True)
            metadata = build_metadata.write_metadata(
                self.directory, version=rs.VERSION, commit=rs.COMMIT, identity=rs.IDENTITY,
                signing_report=signing, built_at="2026-10-01T00:00:00Z")
            self.assertIs(metadata["windows_signed"], False)
            self.assertNotIn("windows_signing", metadata)
        # A signed report becomes the metadata's signing record, and the check can require it.
        report.write_text(json.dumps(report_for(True)), encoding="utf-8")
        for name in ("BUILD-METADATA.json", "SHA256SUMS.txt"):
            (self.directory / name).unlink()
        metadata = build_metadata.write_metadata(
            self.directory, version=rs.VERSION, commit=rs.COMMIT, identity=rs.IDENTITY,
            signing_report=json.loads(report.read_text(encoding="utf-8")),
            built_at="2026-10-01T00:00:00Z")
        self.assertEqual(metadata["windows_signing"], {
            "method": "trusted-signing", "signer_subject": "CN=Fiori Labs",
            "signer_thumbprint": "A" * 40})
        self.assertEqual(self.run_main("--require-windows-signed")[0], 0)
        # A report shipped in the candidate must agree with the metadata.
        (self.directory / ws.REPORT_NAME).write_text(json.dumps(report_for(False)),
                                                     encoding="utf-8")
        loaded = json.loads((self.directory / "BUILD-METADATA.json").read_text(encoding="utf-8"))
        rs.rewrite_metadata(self.directory, loaded)
        with self.assertRaisesRegex(ValueError, "disagrees|not valid"):
            cq.candidate_identity(self.directory, rs.IDENTITY)

    def test_source_qualification_can_be_required(self):
        self.assertEqual(self.run_main("--require-source-qualification")[0], 0)
        rs.rewrite_metadata(self.directory, {**self.metadata, "source_qualification": "not-run"})
        self.assertEqual(self.run_main()[0], 0)
        self.assertEqual(self.run_main("--require-source-qualification")[0], 1)
        rs.rewrite_metadata(self.directory, {**self.metadata, "source_qualification": "maybe"})
        self.assertEqual(self.run_main()[0], 1)

    def test_inventory_is_exact(self):
        # An archive is expected exactly when the metadata says the release is not setup-only.
        (self.directory / rs.ARCHIVE).unlink()
        rs.rewrite_metadata(self.directory, self.metadata)
        with self.assertRaisesRegex(ValueError, "inventory"):
            cq.candidate_identity(self.directory, rs.IDENTITY)
        rs.rewrite_metadata(self.directory, {**self.metadata, "windows_setup_only": True})
        cq.candidate_identity(self.directory, rs.IDENTITY)
        (self.directory / "SHA256SUMS.txt").write_text("", encoding="utf-8")
        with self.assertRaises(ValueError):
            cq.candidate_identity(self.directory, rs.IDENTITY)

    def test_checksum_lines_must_be_two_space_lines(self):
        sums = self.directory / "SHA256SUMS.txt"
        good = sums.read_text(encoding="utf-8")
        for bad in (good.replace("  ", " ", 1), good.replace("  ", " *", 1),
                    good + good.splitlines()[0] + "\n", good.upper()):
            with self.subTest(bad=bad[:40]):
                sums.write_text(bad, encoding="utf-8")
                with self.assertRaises(ValueError):
                    cq.candidate_identity(self.directory, rs.IDENTITY)

    def test_draft_assets_can_be_read_from_a_saved_document(self):
        document = self.tmp / "draft.json"
        document.write_text(json.dumps({"assets": [{"name": "older-installer.exe"}]}),
                            encoding="utf-8")
        code, _, err = self.run_main("--draft-assets", str(document))
        self.assertEqual(code, 1)
        self.assertIn("unrelated", err)
        document.write_text(json.dumps({"assets": []}), encoding="utf-8")
        self.assertEqual(self.run_main("--draft-assets", str(document))[0], 0)

    def test_the_check_never_touches_the_network(self):
        source = (rs.TOOLS / "check_release_qualification.py").read_text(encoding="utf-8")
        for name in ("urllib", "http.client", "socket", "subprocess", "requests"):
            self.assertNotIn(name, source)


if __name__ == "__main__":
    unittest.main()
