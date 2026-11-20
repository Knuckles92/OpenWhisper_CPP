"""Print a component catalog entry for an archive: its URL, SHA-256, size and extracted bytes.

The runtime catalog (``src/models/data``) pins every download to an exact URL, size and digest.
This tool measures them so nobody types a digest by hand::

    python tools/release/pin_component.py --url https://github.com/<owner>/<repo>/releases/<a>.zip
    python tools/release/pin_component.py --file nemo-speech.zip \\
        --published-url https://example.org/nemo-speech.zip

``--url`` downloads the archive to a temporary folder (HTTPS only; plain HTTP is accepted for a
loopback address, for tests) and removes it afterwards. ``--file`` reads one already on disk.
Nothing is extracted: the archive's member sizes are summed, and a member whose name is absolute
or climbs out of the folder is an error, as the installer would refuse it too.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import posixpath
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Dict, Optional, Sequence, Tuple

_CHUNK = 1 << 20
_DEFAULT_MAX_BYTES = 8 << 30
_USER_AGENT = "openwhisper-cpp-pin-component"
_TAR_SUFFIXES = (".tar.gz", ".tgz", ".tar.xz", ".tar.bz2", ".tar")


class PinError(ValueError):
    """The archive cannot be pinned."""


def infer_extract(name: str) -> str:
    lowered = name.lower()
    if lowered.endswith(".zip"):
        return "zip"
    if lowered.endswith(_TAR_SUFFIXES):
        return "nemo-tar"
    raise PinError(f"Cannot tell how to extract {name}; pass --extract zip, nemo-tar or none")


def _safe_member(name: str) -> str:
    normal = posixpath.normpath(name.replace("\\", "/"))
    if name.startswith(("/", "\\")) or ":" in name.split("/")[0] or normal.startswith("../") \
            or normal == "..":
        raise PinError(f"Archive member escapes its folder: {name}")
    return normal


def measure_archive(path: Path, extract: str) -> Tuple[int, Optional[str]]:
    """Return the bytes the archive extracts to and, for a tarball, its one top-level folder."""
    if extract == "zip":
        try:
            with zipfile.ZipFile(path) as archive:
                total = 0
                for info in archive.infolist():
                    _safe_member(info.filename)
                    if not info.is_dir():
                        total += info.file_size
                return total, None
        except zipfile.BadZipFile as exc:
            raise PinError(f"{path.name} is not a zip archive: {exc}") from exc
    if extract == "nemo-tar":
        try:
            with tarfile.open(path) as archive:
                total, tops = 0, set()
                for member in archive:
                    normal = _safe_member(member.name)
                    if normal == ".":
                        continue
                    tops.add(normal.split("/")[0])
                    if member.isfile():
                        total += member.size
        except tarfile.TarError as exc:
            raise PinError(f"{path.name} is not a tar archive: {exc}") from exc
        return total, (next(iter(tops)) if len(tops) == 1 else None)
    raise PinError(f"Unknown extract kind {extract!r}")


def pin(path: Path, *, name: str, url: str, extract: str) -> Dict:
    """The catalog entry for the archive at ``path``."""
    archive: Dict = {
        "name": name,
        "url": url,
        "sha256": _sha256(path),
        "size_bytes": path.stat().st_size,
    }
    if extract == "none":
        return {"archives": [archive]}
    extracted, root = measure_archive(path, extract)
    archive["extract"] = extract
    if root is not None and extract == "nemo-tar":
        archive["root"] = root
    return {"install_bytes": extracted, "archives": [archive]}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while block := handle.read(_CHUNK):
            digest.update(block)
    return digest.hexdigest()


def _is_loopback(host: str) -> bool:
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host.lower() == "localhost"


def check_url(url: str) -> str:
    parts = urllib.parse.urlsplit(url)
    if parts.hostname and parts.username is None:
        if parts.scheme == "https":
            return url
        if parts.scheme == "http" and _is_loopback(parts.hostname):
            return url
    raise PinError("--url must be an https URL")


class _CheckedRedirects(urllib.request.HTTPRedirectHandler):
    """Follow redirects (release hosts redirect to a CDN) but never off https."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        check_url(urllib.parse.urljoin(req.full_url, newurl))
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(url: str, target: Path, max_bytes: int = _DEFAULT_MAX_BYTES) -> None:
    handlers = [_CheckedRedirects]
    host = urllib.parse.urlsplit(check_url(url)).hostname or ""
    if _is_loopback(host):
        # A loopback URL is a test server on this machine: never route it through a proxy the
        # environment names, which would carry the request off the machine.
        handlers.append(urllib.request.ProxyHandler({}))
    opener = urllib.request.build_opener(*handlers)
    request = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})
    received = 0
    try:
        with opener.open(request, timeout=60) as response, target.open("wb") as out:
            while block := response.read(_CHUNK):
                received += len(block)
                if received > max_bytes:
                    raise PinError(f"The download is larger than {max_bytes} bytes")
                out.write(block)
    except urllib.error.HTTPError as exc:
        exc.close()
        raise PinError(f"Download failed: HTTP {exc.code} {exc.reason}") from None
    except urllib.error.URLError as exc:
        raise PinError(f"Download failed: {exc.reason}") from exc


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--url", help="an https URL to download and measure")
    source.add_argument("--file", type=Path, help="an archive already on disk")
    parser.add_argument("--extract", choices=("zip", "nemo-tar", "none"),
                        help="how the installer unpacks it (default: from the file name)")
    parser.add_argument("--name",
                        help="the archive's name in the entry (default: from the URL or file)")
    parser.add_argument("--published-url",
                        help="with --file: the URL the archive is published at, for the entry")
    parser.add_argument("--max-bytes", type=int, default=_DEFAULT_MAX_BYTES,
                        help="refuse a download larger than this")
    args = parser.parse_args(argv)
    try:
        if args.url:
            url = check_url(args.url)
            name = args.name or posixpath.basename(urllib.parse.urlsplit(url).path)
            if not name:
                raise PinError("The URL names no file; pass --name")
            with tempfile.TemporaryDirectory(prefix="fv-pin-") as folder:
                local = Path(folder) / "archive"
                download(url, local, args.max_bytes)
                entry = pin(local, name=name, url=url,
                            extract=args.extract or infer_extract(name))
        else:
            name = args.name or args.file.name
            entry = pin(args.file, name=name, url=args.published_url or "<published URL>",
                        extract=args.extract or infer_extract(name))
    except (OSError, ValueError) as exc:
        print(f"Pin failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(entry, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
