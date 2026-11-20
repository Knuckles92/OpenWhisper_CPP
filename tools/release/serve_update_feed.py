"""Serve a setup and optional native update archive on this machine only.

Lets an installed OpenWhisper_CPP update to a build that has not been published, so the update path
can be soaked before it ships::

    python tools/release/serve_update_feed.py --setup out/installer/OpenWhisper_CPP-Setup-0.1.1.exe \\
        --version 0.1.1

Then, in a second shell, start the *installed* app with the feed override::

    $env:OPENWHISPER_CPP_UPDATE_FEED_URL = "http://127.0.0.1:<port>/releases/latest"
    & "$env:LOCALAPPDATA\\Programs\\OpenWhisper_CPP\\openwhisper-cpp.exe"

Digests and sizes are the file's own, so the app verifies the setup exactly as it would a
published one. ``--prerelease`` serves the build as a GitHub prerelease (and as the one entry of
``/releases?per_page=30``), to soak the beta track: only a copy with beta updates switched on may
install it. The server binds a loopback address only and answers GET and HEAD for the supplied
artifacts.
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional, Sequence
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_common as common  # noqa: E402

_CHUNK = 1 << 20
_RANGE_RE = re.compile(r"bytes=([0-9]{0,20})-([0-9]{0,20})", re.IGNORECASE)


def resolve_host(host: str) -> str:
    """Return the IPv4 loopback address to bind, or raise for any other host."""
    if host.lower() == "localhost":
        return "127.0.0.1"
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        raise ValueError(f"--host must be a loopback address, not {host!r}") from None
    if address.version != 4 or not address.is_loopback:
        raise ValueError(f"--host must be an IPv4 loopback address, not {host!r}")
    return str(address)


def build_payload(
    setup: Path,
    version: str,
    base_url: str,
    notes: str,
    *,
    product: str,
    prerelease: bool = False,
    archive: Optional[Path] = None,
) -> dict:
    """The GitHub-shaped release document for the supplied artifacts."""
    files = [Path(setup)]
    if archive is not None:
        files.append(Path(archive))
    return {
        "tag_name": f"v{version}",
        "name": f"{product} {version}",
        "html_url": f"{base_url}/releases/tag/v{version}",
        "body": notes,
        "draft": False,
        "prerelease": prerelease,
        "assets": [
            {
                "name": artifact.name,
                "size": artifact.stat().st_size,
                "state": "uploaded",
                "digest": f"sha256:{common.sha256_of(artifact)}",
                "browser_download_url": f"{base_url}/download/{artifact.name}",
            }
            for artifact in files
        ],
    }


def _stamp(info: os.stat_result) -> tuple[int, int, int, int]:
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def _range_bounds(header: str, total: int) -> Optional[tuple[int, int]]:
    """Return inclusive bounds or None for a Range the server ignores."""
    match = _RANGE_RE.fullmatch(header)
    if not match:
        return None
    first, last = match.groups()
    if first:
        start = int(first)
        end = min(int(last), total - 1) if last else total - 1
        if last and int(last) < start:
            return None
        return start, end
    if last:
        return max(0, total - int(last)), total - 1
    return None


class _Handler(BaseHTTPRequestHandler):
    # Set on the subclass make_server builds, so two servers in one process do not share them.
    payload: dict = {}
    setup_file: Path = Path(".")
    files: dict = {}
    serve_list = False

    def do_GET(self) -> None:  # noqa: N802 - http.server API
        self._respond(send_body=True)

    def do_HEAD(self) -> None:  # noqa: N802 - http.server API
        self._respond(send_body=False)

    def _respond(self, *, send_body: bool) -> None:
        try:
            parts = urlsplit(self.path)
        except ValueError:
            self.send_error(404)
            return
        if parts.scheme or parts.netloc or parts.fragment:
            self.send_error(404)
            return
        if parts.path == "/releases/latest" and not parts.query:
            self._send_json(self.payload, send_body)
        elif self.serve_list and parts.path == "/releases" and parts.query == "per_page=30":
            self._send_json([self.payload], send_body)
        elif parts.path in self.files and not parts.query:
            self._send_file(*self.files[parts.path], send_body)
        else:
            self.send_error(404)

    def _send_json(self, document, send_body: bool) -> None:
        body = json.dumps(document, indent=2).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if send_body:
            self.wfile.write(body)

    def _send_file(self, file: Path, stamp: tuple[int, int, int, int], send_body: bool) -> None:
        try:
            if file.resolve(strict=True) != file or _stamp(file.stat()) != stamp:
                self.send_error(404)
                return
            handle = file.open("rb")
        except (OSError, RuntimeError):
            self.send_error(404)
            return
        with handle:
            if _stamp(os.fstat(handle.fileno())) != stamp:
                self.send_error(404)
                return
            total = stamp[2]
            bounds = _range_bounds(self.headers.get("Range", ""), total)
            start, end = bounds if bounds is not None else (0, total - 1)
            if bounds is not None and start > end:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{total}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(206 if bounds is not None else 200)
            self.send_header("Accept-Ranges", "bytes")
            if bounds is not None:
                self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
            self.send_header("Content-Type", "application/octet-stream")
            remaining = end - start + 1
            self.send_header("Content-Length", str(remaining))
            self.end_headers()
            if send_body:
                handle.seek(start)
                while remaining and (block := handle.read(min(remaining, _CHUNK))):
                    self.wfile.write(block)
                    remaining -= len(block)

    def log_message(self, fmt: str, *args) -> None:  # noqa: D401 - http.server API
        sys.stdout.write("%s - %s\n" % (self.address_string(), fmt % args))
        sys.stdout.flush()


def make_server(
    setup: Path,
    version: str,
    *,
    identity: dict,
    host: str = "127.0.0.1",
    port: int = 0,
    notes: str = "Local soak build.",
    prerelease: bool = False,
    archive: Optional[Path] = None,
) -> ThreadingHTTPServer:
    """Bind a loopback server without serving yet. Pass ``port=0`` for a free port.

    Raises:
        ValueError: The host is not loopback, the version is malformed, or an artifact does
            not have the expected filename.
    """
    bind = resolve_host(host)
    if not common.VERSION_RE.fullmatch(version):
        raise ValueError(f"--version must be X.Y.Z, not {version!r}")

    def validated(path: Path, name: str) -> Path:
        path = Path(path)
        if path.name != name:
            raise ValueError(f"{path.name} is not {name}")
        resolved = path.resolve()
        if resolved.name != name or not resolved.is_file():
            raise ValueError(f"{path} is not a file named {name}")
        return resolved

    setup = validated(setup, common.setup_name(identity, version))
    if archive is not None:
        archive = validated(archive, common.archive_name(identity, version))
    artifacts = [setup] if archive is None else [setup, archive]
    files = {f"/download/{file.name}": (file, _stamp(file.stat())) for file in artifacts}
    handler = type("FeedHandler", (_Handler,), {"setup_file": setup, "files": files,
                                                "serve_list": prerelease})
    server = ThreadingHTTPServer((bind, port), handler)
    try:
        base_url = f"http://{bind}:{server.server_address[1]}"
        handler.payload = build_payload(setup, version, base_url, notes,
                                        product=common.product_name(identity),
                                        prerelease=prerelease, archive=archive)
        if any(_stamp(file.stat()) != stamp for file, stamp in files.values()):
            raise ValueError("An artifact changed while the feed was being prepared")
    except BaseException:
        # The server is bound already: a failure here must not leave the port held.
        server.server_close()
        raise
    return server


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--setup", type=Path, required=True,
                        help="the OpenWhisper_CPP-Setup-X.Y.Z.exe to offer")
    parser.add_argument("--archive", type=Path, help="the optional OpenWhisper_CPP-X.Y.Z-win64.zip")
    parser.add_argument("--version", required=True, help="X.Y.Z, the version the setup carries")
    parser.add_argument("--host", default="127.0.0.1", help="a loopback address (the default)")
    parser.add_argument("--port", type=int, default=0, help="0 picks a free port (the default)")
    parser.add_argument("--notes", default="Local soak build.",
                        help="release notes shown in the prompt")
    parser.add_argument("--prerelease", action="store_true",
                        help="serve the build as a prerelease (installable on the beta track only)")
    parser.add_argument("--identity", type=Path,
                        help="release_identity.json (default: the source tree's)")
    args = parser.parse_args(argv)
    try:
        identity = common.load_identity(args.identity, require=("setup_prefix",))
        server = make_server(args.setup, args.version, identity=identity, host=args.host,
                             port=args.port, notes=args.notes, prerelease=args.prerelease,
                             archive=args.archive)
    except (OSError, ValueError) as exc:
        print(f"Feed not started: {exc}", file=sys.stderr)
        return 2
    handler = server.RequestHandlerClass
    feed_url = f"http://{server.server_address[0]}:{server.server_address[1]}/releases/latest"
    print(f"Serving {common.product_name(identity)} {args.version} from {handler.setup_file}")
    for asset in handler.payload["assets"]:
        print(f"  {asset['name']}  {asset['size']} bytes  {asset['digest']}")
    print()
    print("Start the installed app with the feed override, then check for updates:")
    print(f'  $env:OPENWHISPER_CPP_UPDATE_FEED_URL = "{feed_url}"')
    exe = identity.get("app_exe") or "openwhisper-cpp.exe"
    folder = identity.get("install_dir_name") or common.product_name(identity)
    print(f'  & "$env:LOCALAPPDATA\\Programs\\{folder}\\{exe}"')
    print(flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
