"""Windows signing configuration, SignTool/Inno arguments, and the signing record.

SignTool and Authenticode only exist on Windows; these tests cover the decisions around them with
injected inspections.
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import unittest
from pathlib import Path
from unittest import mock

import release_support as rs

import windows_signing as ws

THUMB = "AB" * 20
OTHER_THUMB = "CD" * 20
SIGN_VARIABLES = ("OPENWHISPER_CPP_SIGN_CERT_SHA1", "OPENWHISPER_CPP_SIGN_PFX", "OPENWHISPER_CPP_SIGN_PASS",
                  "OPENWHISPER_CPP_SIGN_DLIB", "OPENWHISPER_CPP_SIGN_METADATA", "OPENWHISPER_CPP_SIGN_TIMESTAMP_URL",
                  "OPENWHISPER_CPP_SIGNTOOL")


class SigningFiles(rs.TempDirTestCase):
    def setUp(self):
        super().setUp()
        pfx = self.tmp / "cert.pfx"
        dlib = self.tmp / "Azure.CodeSigning.Dlib.dll"
        metadata = self.tmp / "metadata.json"
        for path in (pfx, dlib, metadata):
            path.write_bytes(b"x")
        self.files = {"pfx": str(pfx), "dlib": str(dlib), "metadata": str(metadata)}


class TestSigningMode(SigningFiles):
    def test_nothing_configured_is_unsigned(self):
        self.assertEqual(ws.signing_mode({}), ws.NONE)

    def test_each_method(self):
        self.assertEqual(ws.signing_mode({"OPENWHISPER_CPP_SIGN_CERT_SHA1": THUMB}), ws.CERTIFICATE)
        self.assertEqual(ws.signing_mode({
            "OPENWHISPER_CPP_SIGN_PFX": self.files["pfx"], "OPENWHISPER_CPP_SIGN_PASS": "pw"}), ws.PFX)
        self.assertEqual(ws.signing_mode({
            "OPENWHISPER_CPP_SIGN_DLIB": self.files["dlib"],
            "OPENWHISPER_CPP_SIGN_METADATA": self.files["metadata"]}), ws.TRUSTED_SIGNING)

    def test_partial_configuration_fails(self):
        for env in ({"OPENWHISPER_CPP_SIGN_CERT_SHA1": "not-a-thumbprint"},
                    {"OPENWHISPER_CPP_SIGN_PASS": "password without a pfx"},
                    {"OPENWHISPER_CPP_SIGN_PFX": "/nonexistent/cert.pfx"},
                    {"OPENWHISPER_CPP_SIGN_DLIB": "/nonexistent/dlib.dll"}):
            with self.subTest(env=env), self.assertRaises(ws.SigningError):
                ws.signing_mode(env)

    def test_trusted_signing_needs_both_files(self):
        with self.assertRaisesRegex(ws.SigningError, "METADATA"):
            ws.signing_mode({"OPENWHISPER_CPP_SIGN_DLIB": self.files["dlib"]})

    def test_two_methods_fail(self):
        with self.assertRaisesRegex(ws.SigningError, "one signing method"):
            ws.signing_mode({
                "OPENWHISPER_CPP_SIGN_CERT_SHA1": THUMB,
                "OPENWHISPER_CPP_SIGN_DLIB": self.files["dlib"],
                "OPENWHISPER_CPP_SIGN_METADATA": self.files["metadata"],
            })


class TestArguments(SigningFiles):
    def test_every_method_hashes_and_timestamps_with_sha256(self):
        envs = {
            ws.CERTIFICATE: {"OPENWHISPER_CPP_SIGN_CERT_SHA1": THUMB},
            ws.PFX: {"OPENWHISPER_CPP_SIGN_PFX": self.files["pfx"], "OPENWHISPER_CPP_SIGN_PASS": "pw"},
            ws.TRUSTED_SIGNING: {
                "OPENWHISPER_CPP_SIGN_DLIB": self.files["dlib"],
                "OPENWHISPER_CPP_SIGN_METADATA": self.files["metadata"],
            },
        }
        for mode, env in envs.items():
            env = rs.with_identity(env)
            args = ws.sign_arguments(mode, env)
            self.assertEqual(args[:7], ["sign", "/fd", "SHA256", "/tr", args[4], "/td", "SHA256"])
            self.assertTrue(args[4].startswith("http"))
        self.assertIn("/sha1", ws.sign_arguments(
            ws.CERTIFICATE, rs.with_identity(envs[ws.CERTIFICATE])))
        trusted = ws.sign_arguments(ws.TRUSTED_SIGNING, rs.with_identity(envs[ws.TRUSTED_SIGNING]))
        self.assertEqual(trusted[4], ws.TRUSTED_SIGNING_TIMESTAMP_URL)
        self.assertEqual(trusted[-4:],
                         ["/dlib", self.files["dlib"], "/dmdf", self.files["metadata"]])

    def test_timestamp_server_can_be_overridden(self):
        env = rs.with_identity({"OPENWHISPER_CPP_SIGN_CERT_SHA1": THUMB,
                                "OPENWHISPER_CPP_SIGN_TIMESTAMP_URL": "http://ts.example"})
        self.assertEqual(ws.sign_arguments(ws.CERTIFICATE, env)[4], "http://ts.example")

    def test_timestamp_server_defaults_to_the_release_identity(self):
        env = rs.with_identity({"OPENWHISPER_CPP_SIGN_CERT_SHA1": THUMB})
        self.assertEqual(ws.sign_arguments(ws.CERTIFICATE, env)[4], "http://timestamp.fixture.test")
        missing = {"OPENWHISPER_CPP_SIGN_CERT_SHA1": THUMB,
                   "OPENWHISPER_CPP_RELEASE_IDENTITY": str(self.tmp / "absent.json")}
        self.assertEqual(ws.sign_arguments(ws.CERTIFICATE, missing)[4], ws.DEFAULT_TIMESTAMP_URL)
        broken = self.tmp / "broken.json"
        broken.write_text("{not json", encoding="utf-8")
        with self.assertRaises(ws.SigningError):
            ws.sign_arguments(ws.CERTIFICATE,
                              {**missing, "OPENWHISPER_CPP_RELEASE_IDENTITY": str(broken)})

    def test_unsigned_build_has_no_sign_arguments(self):
        with self.assertRaises(ws.SigningError):
            ws.sign_arguments(ws.NONE, {})
        self.assertEqual(ws.inno_arguments(ws.NONE, None, {}), [])

    def test_inno_command_quotes_and_escapes(self):
        env = rs.with_identity({"OPENWHISPER_CPP_SIGN_PFX": self.files["pfx"],
                                "OPENWHISPER_CPP_SIGN_PASS": "pa$s word"})
        define, tool = ws.inno_arguments(ws.PFX, r"C:\Program Files (x86)\signtool.exe", env)
        self.assertEqual(define, "/DSignToolName=openwhisper-cpp")
        self.assertTrue(tool.startswith(
            r"/Sopenwhisper-cpp=$qC:\Program Files (x86)\signtool.exe$q sign "))
        self.assertIn("$qpa$$s word$q", tool)
        self.assertTrue(tool.endswith(" $f"))


class FindSigntool(rs.TempDirTestCase):
    def test_find_signtool_prefers_the_newest_sdk(self):
        with mock.patch.object(ws.shutil, "which", lambda name: None):
            for version in ("10.0.19041.0", "10.0.22621.0", "10.0.9200.0"):
                tool = self.tmp / "Windows Kits" / "10" / "bin" / version / "x64" / "signtool.exe"
                tool.parent.mkdir(parents=True)
                tool.write_bytes(b"")
            found = ws.find_signtool({"ProgramFiles(x86)": str(self.tmp),
                                      "ProgramFiles": str(self.tmp / "none")})
            self.assertEqual(Path(found).parts[-3], "10.0.22621.0")
            with self.assertRaises(ws.SigningError):
                ws.find_signtool({"ProgramFiles(x86)": str(self.tmp / "a"),
                                  "ProgramFiles": str(self.tmp / "b")})

    def test_signtool_can_be_named_by_the_environment(self):
        named = self.tmp / "my-signtool.exe"
        named.write_bytes(b"")
        with mock.patch.object(ws.shutil, "which", lambda name: "from-path.exe"):
            self.assertEqual(ws.find_signtool({"OPENWHISPER_CPP_SIGNTOOL": str(named)}), str(named))
            with self.assertRaises(ws.SigningError):
                ws.find_signtool({"OPENWHISPER_CPP_SIGNTOOL": str(self.tmp / "missing.exe")})
            self.assertEqual(ws.find_signtool({}), "from-path.exe")


def _signed(name, *, status="Valid", thumb=THUMB, timestamped=True):
    return {"name": name, "sha256": "0" * 64, "status": status,
            "signer_subject": "CN=Fiori Labs", "signer_thumbprint": thumb,
            "timestamped": timestamped}


NAMES = ("openwhisper-cpp.exe", "openwhisper-cpp-cli.exe", rs.SETUP)
ALL_VERIFIED = {name: True for name in NAMES}
CERT_ENV = {"OPENWHISPER_CPP_SIGN_CERT_SHA1": THUMB.lower()}


class TestReport(rs.TempDirTestCase):
    def test_signed_build(self):
        report = ws.build_report(ws.CERTIFICATE, [_signed(n) for n in NAMES],
                                 signtool_verified=ALL_VERIFIED, env=CERT_ENV)
        self.assertIs(report["signed"], True)
        self.assertEqual(report["method"], "certificate")
        self.assertEqual(report["signer_thumbprint"], THUMB)

    def test_trusted_signing_method(self):
        report = ws.build_report(ws.TRUSTED_SIGNING, [_signed(n) for n in NAMES],
                                 signtool_verified=ALL_VERIFIED, env={})
        self.assertEqual(report["method"], "trusted-signing")

    def test_any_bad_file_fails_a_signed_build(self):
        for broken in (_signed(NAMES[2], status="NotSigned"),
                       _signed(NAMES[2], status="UnknownError"),
                       _signed(NAMES[2], timestamped=False),
                       _signed(NAMES[2], thumb=OTHER_THUMB)):
            inspections = [_signed(n) for n in NAMES[:2]] + [broken]
            with self.subTest(broken=broken), self.assertRaises(ws.SigningError):
                ws.build_report(ws.CERTIFICATE, inspections,
                                signtool_verified=ALL_VERIFIED, env={})

    def test_signtool_must_agree(self):
        with self.assertRaises(ws.SigningError):
            ws.build_report(ws.CERTIFICATE, [_signed(n) for n in NAMES],
                            signtool_verified={**ALL_VERIFIED, NAMES[0]: False}, env={})

    def test_signature_must_come_from_the_configured_certificate(self):
        with self.assertRaisesRegex(ws.SigningError, "configured certificate"):
            ws.build_report(ws.CERTIFICATE, [_signed(n, thumb=OTHER_THUMB) for n in NAMES],
                            signtool_verified=ALL_VERIFIED, env=CERT_ENV)

    def test_unsigned_build(self):
        report = ws.build_report(ws.NONE, [_signed(n, status="NotSigned") for n in NAMES],
                                 signtool_verified={}, env={})
        self.assertIs(report["signed"], False)
        self.assertIsNone(report["method"])

    def test_unsigned_build_with_a_stray_signature_fails(self):
        inspections = [_signed(NAMES[0])] + [_signed(n, status="NotSigned") for n in NAMES[1:]]
        with self.assertRaisesRegex(ws.SigningError, "not configured"):
            ws.build_report(ws.NONE, inspections, signtool_verified={}, env={})

    def test_verify_files_hashes_each_file(self):
        paths = []
        for name in NAMES:
            (self.tmp / name).write_bytes(name.encode())
            paths.append(self.tmp / name)
        report = ws.verify_files(
            paths, ws.TRUSTED_SIGNING,
            inspect=lambda path: {k: v for k, v in _signed(path.name).items()
                                  if k not in ("name", "sha256")},
            signtool_verify=lambda path: True,
            env={},
        )
        self.assertEqual([f["sha256"] for f in report["files"]],
                         [hashlib.sha256(name.encode()).hexdigest() for name in NAMES])

    def test_verify_command_writes_the_report_and_refuses_a_stray_signature(self):
        target = self.tmp / "openwhisper-cpp.exe"
        target.write_bytes(b"exe")
        report = self.tmp / "out" / "windows-signing.json"
        unsigned = {"status": "NotSigned", "signer_subject": None, "signer_thumbprint": None,
                    "timestamped": False}
        with mock.patch.dict(os.environ), mock.patch.object(
                ws, "_inspect_windows", lambda path: dict(unsigned)):
            for name in SIGN_VARIABLES:
                os.environ.pop(name, None)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ws.main(["verify", "--report", str(report), str(target)]), 0)
            self.assertIn("unsigned", out.getvalue())
            self.assertIs(json.loads(report.read_text(encoding="utf-8"))["signed"], False)
            with mock.patch.object(ws, "_inspect_windows", lambda path: _signed(path.name)):
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    self.assertEqual(ws.main(["verify", "--report", str(report), str(target)]), 1)
                self.assertIn("not configured", err.getvalue())


class TestReleaseMetadata(rs.TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.release = self.tmp / "release"
        self.release.mkdir()
        (self.release / NAMES[2]).write_bytes(b"setup")
        (self.release / rs.ARCHIVE).write_bytes(b"archive")

    def _report(self, *, signed=True, setup_bytes=b"setup"):
        status = "Valid" if signed else "NotSigned"
        files = [dict(_signed(n, status=status)) for n in NAMES]
        files[2]["sha256"] = hashlib.sha256(setup_bytes).hexdigest()
        return {"schema_version": 1, "signed": signed,
                "method": "trusted-signing" if signed else None,
                "signer_subject": "CN=Fiori Labs" if signed else None,
                "signer_thumbprint": THUMB if signed else None, "files": files}

    def test_signed_release(self):
        self.assertEqual(ws.release_metadata(self._report(), self.release, rs.IDENTITY), {
            "windows_signed": True,
            "windows_signing": {"method": "trusted-signing", "signer_subject": "CN=Fiori Labs",
                                "signer_thumbprint": THUMB},
        })

    def test_unsigned_release(self):
        self.assertEqual(
            ws.release_metadata(self._report(signed=False), self.release, rs.IDENTITY),
            {"windows_signed": False})

    def test_report_must_describe_this_setup_exe(self):
        with self.assertRaisesRegex(ws.SigningError, "does not match"):
            ws.release_metadata(self._report(setup_bytes=b"other build"), self.release,
                                rs.IDENTITY)

    def test_malformed_reports_fail(self):
        for change in ({"schema_version": 2}, {"signed": "yes"}, {"files": None},
                       {"method": "magic"}):
            with self.subTest(change=change), self.assertRaises(ws.SigningError):
                ws.release_metadata({**self._report(), **change}, self.release, rs.IDENTITY)

    def test_inconsistent_statuses_fail(self):
        report = self._report()
        report["files"][0]["status"] = "NotSigned"
        with self.assertRaisesRegex(ws.SigningError, "inconsistent"):
            ws.release_metadata(report, self.release, rs.IDENTITY)

    def test_cli(self):
        path = self.tmp / ws.REPORT_NAME
        path.write_text(json.dumps(self._report(signed=False)), encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = ws.main(["--identity", str(rs.IDENTITY_FILE), "metadata", "--report", str(path),
                            "--release", str(self.release)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.getvalue()), {"windows_signed": False})


HAS_POWERSHELL = bool(shutil.which("pwsh") or shutil.which("powershell"))


@unittest.skipUnless(sys.platform == "win32" and HAS_POWERSHELL,
                     "Authenticode inspection needs Windows PowerShell")
class RealInspection(rs.TempDirTestCase):
    """``verify`` through the real Get-AuthenticodeSignature, on an unsigned executable."""

    def test_verify_passes_an_unsigned_executable_only_as_unsigned(self):
        target = self.tmp / "openwhisper-cpp.exe"
        target.write_bytes(rs.minimal_pe())
        report = self.tmp / "windows-signing.json"
        with mock.patch.dict(os.environ):
            for name in SIGN_VARIABLES:
                os.environ.pop(name, None)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ws.main(["verify", "--report", str(report), str(target)]), 0)
        self.assertIn("Windows executables and installer: unsigned", out.getvalue())
        written = json.loads(report.read_text(encoding="utf-8"))
        self.assertIs(written["signed"], False)
        (entry,) = written["files"]
        self.assertEqual(entry["status"], "NotSigned")
        self.assertEqual(entry["sha256"], hashlib.sha256(rs.minimal_pe()).hexdigest())
        self.assertIs(entry["timestamped"], False)
        self.assertIsNone(entry["signer_thumbprint"])

    def test_a_file_that_is_not_an_executable_is_not_passed_as_unsigned(self):
        target = self.tmp / "openwhisper-cpp.exe"
        target.write_bytes(b"MZ but not a PE")
        with mock.patch.dict(os.environ):
            for name in SIGN_VARIABLES:
                os.environ.pop(name, None)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(ws.main(["verify", "--report", str(self.tmp / "r.json"),
                                          str(target)]), 1)
        self.assertIn("not configured but these are signed", err.getvalue())


class UnsignedBuild(rs.TempDirTestCase):
    def test_unsigned_sign_command_is_a_visible_no_op(self):
        target = self.tmp / "openwhisper-cpp.exe"
        target.write_bytes(b"exe")
        with mock.patch.dict(os.environ):
            for name in SIGN_VARIABLES:
                os.environ.pop(name, None)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ws.main(["sign", str(target)]), 0)
            self.assertIn("Not signed", out.getvalue())
            self.assertEqual(target.read_bytes(), b"exe")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ws.main(["inno-args"]), 0)
            self.assertEqual(out.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
