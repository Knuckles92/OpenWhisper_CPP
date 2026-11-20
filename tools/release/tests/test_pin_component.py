"""pin_component.py: the catalog entry for a fixture archive, read from disk or from loopback."""

import contextlib
import hashlib
import http.server
import io
import json
import os
import tarfile
import threading
import unittest
import zipfile
from unittest import mock

import release_support as rs

import pin_component as pin

MEMBERS = {"bin/runtime.dll": b"d" * 1000, "bin/other.dll": b"o" * 234, "README.txt": b"readme"}
EXTRACTED = sum(len(data) for data in MEMBERS.values())


class PinFixtures(rs.TempDirTestCase):
    def make_zip(self, name="nemo-speech-0.1.0-windows-x86_64-cpu.zip", members=None):
        path = self.tmp / name
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("bin/", b"")
            for member, data in (members or MEMBERS).items():
                archive.writestr(member, data)
        return path

    def make_tar(self, name="nemo-speech-0.1.0-linux-x86_64-cpu.tar.gz", top="nemo-speech-0.1.0"):
        path = self.tmp / name
        with tarfile.open(path, "w:gz") as archive:
            for member, data in MEMBERS.items():
                info = tarfile.TarInfo(f"{top}/{member}")
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        return path

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = pin.main(list(args))
        return code, out.getvalue(), err.getvalue()


class PinComponent(PinFixtures):
    def test_file_prints_the_digest_size_and_extracted_bytes_of_a_zip(self):
        path = self.make_zip()
        code, out, _ = self.run_tool("--file", str(path), "--published-url",
                                     "https://example.org/releases/" + path.name)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out), {
            "install_bytes": EXTRACTED,
            "archives": [{
                "name": path.name,
                "url": "https://example.org/releases/" + path.name,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "size_bytes": path.stat().st_size,
                "extract": "zip",
            }],
        })

    def test_a_tarball_is_measured_and_names_its_top_folder(self):
        path = self.make_tar()
        code, out, _ = self.run_tool("--file", str(path))
        self.assertEqual(code, 0)
        entry = json.loads(out)
        self.assertEqual(entry["install_bytes"], EXTRACTED)
        (archive,) = entry["archives"]
        self.assertEqual(archive["extract"], "nemo-tar")
        self.assertEqual(archive["root"], "nemo-speech-0.1.0")
        self.assertEqual(archive["url"], "<published URL>")

    def test_a_file_that_is_not_extracted_has_no_install_bytes(self):
        path = self.tmp / "model.gguf"
        path.write_bytes(b"weights")
        code, out, _ = self.run_tool("--file", str(path), "--extract", "none")
        self.assertEqual(code, 0)
        entry = json.loads(out)
        self.assertNotIn("install_bytes", entry)
        self.assertEqual(entry["archives"][0]["size_bytes"], 7)
        self.assertNotIn("extract", entry["archives"][0])

    def test_unsafe_members_and_unknown_kinds_are_refused(self):
        for member in ("../evil.dll", "/abs/evil.dll", "C:/evil.dll", "a/../../evil.dll"):
            with self.subTest(member=member):
                path = self.make_zip("unsafe.zip", {member: b"x"})
                code, _, err = self.run_tool("--file", str(path))
                self.assertEqual(code, 1)
                self.assertIn("escapes its folder", err)
        other = self.tmp / "archive.rar"
        other.write_bytes(b"x")
        code, _, err = self.run_tool("--file", str(other))
        self.assertEqual(code, 1)
        self.assertIn("--extract", err)
        broken = self.tmp / "broken.zip"
        broken.write_bytes(b"not a zip")
        self.assertEqual(self.run_tool("--file", str(broken))[0], 1)

    def test_urls_must_be_https_except_loopback(self):
        for url in ("http://example.com/a.zip", "ftp://example.com/a.zip", "file:///a.zip",
                    "https:///a.zip"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                pin.check_url(url)
        with self.assertRaises(ValueError):
            pin.check_url("http://user@127.0.0.1:8000/a.zip")
        for url in ("https://github.com/NVIDIA/NeMo-Speech.cpp/a.zip",
                    "http://127.0.0.1:8000/a.zip"):
            self.assertEqual(pin.check_url(url), url)
        self.assertEqual(self.run_tool("--url", "http://example.com/a.zip")[0], 1)


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass


class PinFromLoopback(PinFixtures):
    def serve(self, directory):
        handler = lambda *a, **k: _Quiet(*a, directory=str(directory), **k)  # noqa: E731
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=10)

        self.addCleanup(stop)
        return f"http://127.0.0.1:{server.server_address[1]}"

    def test_url_downloads_to_a_temporary_folder_and_pins_the_bytes(self):
        path = self.make_zip()
        base = self.serve(self.tmp)
        code, out, _ = self.run_tool("--url", f"{base}/{path.name}")
        self.assertEqual(code, 0)
        entry = json.loads(out)
        self.assertEqual(entry["install_bytes"], EXTRACTED)
        (archive,) = entry["archives"]
        self.assertEqual(archive["url"], f"{base}/{path.name}")
        self.assertEqual(archive["name"], path.name)
        self.assertEqual(archive["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_a_loopback_download_ignores_a_proxy_from_the_environment(self):
        path = self.make_zip()
        base = self.serve(self.tmp)
        # Port 1 on loopback refuses connections: were the proxy used, the download would fail.
        proxy = "http://127.0.0.1:1"
        with mock.patch.dict(os.environ, {"http_proxy": proxy, "HTTP_PROXY": proxy}):
            code, out, err = self.run_tool("--url", f"{base}/{path.name}")
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(json.loads(out)["install_bytes"], EXTRACTED)

    def test_a_redirect_off_https_is_refused_before_it_is_followed(self):
        class Redirect(_Quiet):
            def do_GET(self):  # noqa: N802 - http.server API
                self.send_response(302)
                self.send_header("Location", "http://example.invalid/a.zip")
                self.send_header("Content-Length", "0")
                self.end_headers()

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Redirect)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=10)

        self.addCleanup(stop)
        code, _, err = self.run_tool("--url", f"http://127.0.0.1:{server.server_address[1]}/a.zip")
        self.assertEqual(code, 1)
        self.assertIn("must be an https URL", err)

    def test_a_missing_file_and_an_oversized_download_fail(self):
        path = self.make_zip()
        base = self.serve(self.tmp)
        code, _, err = self.run_tool("--url", f"{base}/missing.zip")
        self.assertEqual(code, 1)
        self.assertIn("Pin failed: Download failed: HTTP 404", err)
        code, _, err = self.run_tool("--url", f"{base}/{path.name}", "--max-bytes", "10")
        self.assertEqual(code, 1)
        self.assertIn("larger than 10 bytes", err)


if __name__ == "__main__":
    unittest.main()
