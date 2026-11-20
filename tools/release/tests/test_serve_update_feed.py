"""The loopback update feed: what it serves, what it refuses, and that it never calls out."""

import contextlib
import hashlib
import io
import json
import queue
import re
import subprocess
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from unittest import mock

import release_support as rs

import serve_update_feed as feed

NO_PROXY = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def fetch(url, *, method="GET", headers=None):
    request = urllib.request.Request(url, method=method, headers=headers or {})
    try:
        with NO_PROXY.open(request, timeout=10) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as exc:
        exc.close()  # an HTTPError holds the response open
        raise


class FeedTestCase(rs.TempDirTestCase):
    SETUP_BYTES = b"S" * 3000 + b"etup"

    def setUp(self):
        super().setUp()
        quiet = mock.patch.object(feed._Handler, "log_message", lambda *args: None)
        quiet.start()
        self.addCleanup(quiet.stop)
        self.setup = self.tmp / rs.SETUP
        self.setup.write_bytes(self.SETUP_BYTES)

    def start(self, **kwargs):
        server = feed.make_server(self.setup, rs.VERSION, identity=rs.IDENTITY, **kwargs)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=10)

        self.addCleanup(stop)
        host, port = server.server_address
        self.assertEqual(host, "127.0.0.1")
        return f"http://{host}:{port}"


class SoakFeed(FeedTestCase):
    def test_soak_feed_can_serve_a_prerelease(self):
        stable = self.start()
        status, headers, body = fetch(stable + "/releases/latest")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "application/json")
        self.assertIs(json.loads(body)["prerelease"], False)
        beta = self.start(prerelease=True)
        payload = json.loads(fetch(beta + "/releases/latest")[2])
        self.assertIs(payload["prerelease"], True)
        self.assertIs(payload["draft"], False)
        self.assertEqual(payload["tag_name"], "v" + rs.VERSION)
        listing = json.loads(fetch(beta + "/releases?per_page=30")[2])
        self.assertEqual(listing, [payload])

    def test_the_release_document_describes_the_one_setup(self):
        base = self.start(notes="Soak build for the demo.")
        payload = json.loads(fetch(base + "/releases/latest")[2])
        self.assertEqual(payload["body"], "Soak build for the demo.")
        self.assertEqual(payload["name"], "OpenWhisper_CPP " + rs.VERSION)
        (asset,) = payload["assets"]
        self.assertEqual(asset["name"], rs.SETUP)
        self.assertEqual(asset["size"], len(self.SETUP_BYTES))
        self.assertEqual(asset["digest"], "sha256:" + hashlib.sha256(self.SETUP_BYTES).hexdigest())
        self.assertEqual(asset["state"], "uploaded")
        self.assertEqual(asset["browser_download_url"], f"{base}/download/{rs.SETUP}")

    def test_archive_metadata_and_downloads(self):
        archive = self.tmp / f"OpenWhisper_CPP-{rs.VERSION}-win64.zip"
        data = b"PK\x03\x04synthetic archive"
        archive.write_bytes(data)
        base = self.start(archive=archive, prerelease=True)
        payload = json.loads(fetch(base + "/releases/latest")[2])
        self.assertEqual([asset["name"] for asset in payload["assets"]],
                         [rs.SETUP, archive.name])
        self.assertEqual(json.loads(fetch(base + "/releases?per_page=30")[2]), [payload])
        asset = payload["assets"][1]
        url = f"{base}/download/{archive.name}"
        self.assertEqual(asset["browser_download_url"], url)
        self.assertEqual(asset["size"], len(data))
        self.assertEqual(asset["digest"], "sha256:" + hashlib.sha256(data).hexdigest())
        self.assertEqual(fetch(url)[2], data)
        status, headers, body = fetch(url, method="HEAD")
        self.assertEqual((status, body), (200, b""))
        self.assertEqual(headers["Content-Length"], str(len(data)))
        for expression, start, end in (("bytes=4-", 4, len(data) - 1),
                                       ("bytes=2-4", 2, 4),
                                       ("bytes=-3", len(data) - 3, len(data) - 1)):
            with self.subTest(expression=expression):
                status, headers, body = fetch(url, headers={"Range": expression})
                self.assertEqual((status, body), (206, data[start:end + 1]))
                self.assertEqual(headers["Content-Range"], f"bytes {start}-{end}/{len(data)}")
        status, headers, body = fetch(url, method="HEAD", headers={"Range": "bytes=2-4"})
        self.assertEqual((status, body, headers["Content-Length"]), (206, b"", "3"))
        with self.assertRaises(urllib.error.HTTPError) as caught:
            fetch(url, headers={"Range": f"bytes={len(data)}-"})
        self.assertEqual(caught.exception.code, 416)
        self.assertEqual(caught.exception.headers["Content-Range"], f"bytes */{len(data)}")

    def test_archive_requires_exact_name_and_never_serves_adjacent_or_replaced_files(self):
        archive = self.tmp / f"OpenWhisper_CPP-{rs.VERSION}-win64.zip"
        wrong = self.tmp / "OpenWhisper_CPP-99.99.99-win64.zip"
        wrong.write_bytes(b"unlisted")
        with self.assertRaises(ValueError):
            feed.make_server(self.setup, rs.VERSION, identity=rs.IDENTITY, archive=wrong)
        with self.assertRaises(ValueError):
            feed.make_server(self.setup, rs.VERSION, identity=rs.IDENTITY, archive=archive)
        archive.write_bytes(b"original")
        base = self.start(archive=archive)
        for path in (wrong.name, f"../{archive.name}", f"%2e%2e/{archive.name}",
                     f"{archive.name}?neighbor=1"):
            with self.subTest(path=path), self.assertRaises(urllib.error.HTTPError) as caught:
                fetch(f"{base}/download/{path}")
            self.assertEqual(caught.exception.code, 404)
        wrong.replace(archive)
        with self.assertRaises(urllib.error.HTTPError) as caught:
            fetch(f"{base}/download/{archive.name}")
        self.assertEqual(caught.exception.code, 404)

    def test_the_list_endpoint_exists_only_for_a_prerelease(self):
        base = self.start()
        with self.assertRaises(urllib.error.HTTPError) as caught:
            fetch(base + "/releases?per_page=30")
        self.assertEqual(caught.exception.code, 404)

    def test_the_setup_downloads_whole_resumed_and_by_head(self):
        base = self.start()
        url = f"{base}/download/{rs.SETUP}"
        status, headers, body = fetch(url)
        self.assertEqual((status, body), (200, self.SETUP_BYTES))
        self.assertEqual(headers["Content-Length"], str(len(self.SETUP_BYTES)))
        status, headers, body = fetch(url, headers={"Range": "bytes=3000-"})
        self.assertEqual((status, body), (206, b"etup"))
        self.assertEqual(headers["Content-Range"], f"bytes 3000-{len(self.SETUP_BYTES) - 1}/"
                                                    f"{len(self.SETUP_BYTES)}")
        status, headers, body = fetch(url, method="HEAD")
        self.assertEqual((status, body), (200, b""))
        self.assertEqual(headers["Content-Length"], str(len(self.SETUP_BYTES)))

    def test_nothing_else_is_served(self):
        base = self.start()
        for path in ("/", "/download/other.exe", "/download/../" + rs.SETUP, "/releases/latest?x=1",
                     "/releases/tag/v" + rs.VERSION):
            with self.subTest(path=path), self.assertRaises(urllib.error.HTTPError) as caught:
                fetch(base + path)
            self.assertEqual(caught.exception.code, 404)


class CommandLine(FeedTestCase):
    """The command the demo runs, as a child process that the test always stops."""

    def start_command(self, *extra):
        command = [sys.executable, "-I", "-B", "-u", str(rs.TOOLS / "serve_update_feed.py"),
                   "--identity", str(rs.IDENTITY_FILE), "--setup", str(self.setup),
                   "--version", rs.VERSION, "--port", "0", *extra]
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, text=True, encoding="utf-8")
        lines = queue.Queue()

        def read():
            for line in child.stdout:
                lines.put(line)

        reader = threading.Thread(target=read, daemon=True)

        def stop():
            if child.poll() is None:
                child.kill()
            child.wait(timeout=30)
            if reader.is_alive():
                reader.join(timeout=30)  # the pipe ends with the child
            child.stdout.close()

        # Registered before anything can fail, so the server never outlives the test.
        self.addCleanup(stop)
        reader.start()
        deadline = time.monotonic() + 30
        seen = []
        while time.monotonic() < deadline:
            try:
                line = lines.get(timeout=max(0.1, deadline - time.monotonic()))
            except queue.Empty:
                break
            seen.append(line)
            match = re.search(r'OPENWHISPER_CPP_UPDATE_FEED_URL = "(http://127\.0\.0\.1:\d+/releases/'
                              r'latest)"', line)
            if match:
                return child, match.group(1), seen
        self.fail(f"the feed printed no URL: {''.join(seen)!r}")

    def test_command_accepts_explicit_archive(self):
        archive = self.tmp / f"OpenWhisper_CPP-{rs.VERSION}-win64.zip"
        archive.write_bytes(b"synthetic")
        _, url, _ = self.start_command("--archive", str(archive))
        setup, packed = json.loads(fetch(url)[2])["assets"]
        self.assertEqual(setup["name"], rs.SETUP)
        self.assertEqual(packed["name"], archive.name)
        self.assertEqual(fetch(packed["browser_download_url"])[2], b"synthetic")

    def test_the_command_serves_until_it_is_stopped(self):
        child, url, seen = self.start_command("--prerelease")
        self.assertIn(f"Serving OpenWhisper_CPP {rs.VERSION}", "".join(seen))
        payload = json.loads(fetch(url)[2])
        self.assertIs(payload["prerelease"], True)
        self.assertEqual(payload["assets"][0]["browser_download_url"],
                         url.replace("/releases/latest", f"/download/{rs.SETUP}"))
        self.assertEqual(fetch(payload["assets"][0]["browser_download_url"])[2], self.SETUP_BYTES)
        self.assertIsNone(child.poll())
        child.kill()
        child.wait(timeout=30)
        with self.assertRaises(OSError):
            fetch(url)


class Refusals(FeedTestCase):
    def test_only_loopback_hosts_are_accepted(self):
        for host in ("127.0.0.1", "127.0.0.2", "localhost", "LOCALHOST"):
            self.assertTrue(feed.resolve_host(host).startswith("127."), host)
        for host in ("0.0.0.0", "", "192.168.1.5", "10.0.0.1", "example.com", "::1", "::",
                     "8.8.8.8", "127.0.0.1.example.com"):
            with self.subTest(host=host), self.assertRaises(ValueError):
                feed.resolve_host(host)

    def test_make_server_refuses_a_foreign_host_before_binding(self):
        with mock.patch.object(feed, "ThreadingHTTPServer") as bind:
            with self.assertRaises(ValueError):
                feed.make_server(self.setup, rs.VERSION, identity=rs.IDENTITY, host="0.0.0.0")
            bind.assert_not_called()

    def test_a_failure_after_binding_releases_the_port(self):
        real_close = feed.ThreadingHTTPServer.server_close
        with mock.patch.object(feed, "build_payload", side_effect=OSError("unreadable")), \
                mock.patch.object(feed.ThreadingHTTPServer, "server_close", autospec=True,
                                  side_effect=real_close) as close:
            with self.assertRaises(OSError):
                feed.make_server(self.setup, rs.VERSION, identity=rs.IDENTITY)
        close.assert_called_once()

    def test_the_command_line_refuses_a_foreign_host_and_a_wrong_setup(self):
        base = ["--identity", str(rs.IDENTITY_FILE), "--setup", str(self.setup),
                "--version", rs.VERSION]
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(feed.main([*base, "--host", "0.0.0.0"]), 2)
            self.assertIn("loopback", err.getvalue())
            self.assertEqual(feed.main([*base[:-1], "0.2.0"]), 2)
            self.assertEqual(feed.main([*base[:2], "--setup", str(self.tmp / "missing.exe"),
                                        *base[4:]]), 2)
        other = self.tmp / "Other-Setup-0.1.0.exe"
        other.write_bytes(b"x")
        with self.assertRaisesRegex(ValueError, "is not"):
            feed.make_server(other, rs.VERSION, identity=rs.IDENTITY)
        with self.assertRaisesRegex(ValueError, "X.Y.Z"):
            feed.make_server(self.setup, "1.0", identity=rs.IDENTITY)

    def test_the_server_never_makes_an_outbound_request(self):
        source = (rs.TOOLS / "serve_update_feed.py").read_text(encoding="utf-8")
        for name in ("urllib.request", "http.client", "requests", "create_connection", "socket",
                     "subprocess", "ftplib", "smtplib"):
            self.assertNotIn(name, source)


if __name__ == "__main__":
    unittest.main()
