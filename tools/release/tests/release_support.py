"""What the release tools' tests share: the import path, the fixture identity, a candidate."""

from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
IDENTITY_FILE = FIXTURES / "release_identity.json"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import release_common as common  # noqa: E402

IDENTITY = json.loads(IDENTITY_FILE.read_text(encoding="utf-8"))
VERSION = "0.1.0"
SETUP = f"OpenWhisper_CPP-Setup-{VERSION}.exe"
ARCHIVE = f"OpenWhisper_CPP-{VERSION}-win64.zip"
COMMIT = "a" * 40


def minimal_pe() -> bytes:
    """The smallest x64 executable Windows' Authenticode check reads as a PE: one section holding
    a ``ret``, no signature. Authenticode calls a file that is not a PE "UnknownError", not
    "NotSigned", so a real unsigned check needs a real PE."""
    dos = bytearray(64)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64)  # e_lfanew: the PE header follows the DOS header
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x0022)  # AMD64, 1 section
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)  # PE32+
    struct.pack_into("<II", optional, 16, 0x1000, 0x1000)  # entry point, base of code
    struct.pack_into("<Q", optional, 24, 0x140000000)  # image base
    struct.pack_into("<II", optional, 32, 0x1000, 0x200)  # section and file alignment
    struct.pack_into("<H", optional, 48, 6)  # subsystem version 6.0
    struct.pack_into("<II", optional, 56, 0x2000, 0x200)  # size of image, size of headers
    struct.pack_into("<H", optional, 68, 3)  # console subsystem
    struct.pack_into("<QQQQ", optional, 72, 0x100000, 0x1000, 0x100000, 0x1000)  # stack, heap
    struct.pack_into("<I", optional, 108, 16)  # data directories
    section = struct.pack("<8sIIIIIIHHI", b".text", 0x1000, 0x1000, 0x200, 0x200, 0, 0, 0, 0,
                          0x60000020)  # code, execute, read
    header = bytes(dos) + b"PE\0\0" + coff + bytes(optional) + section
    return header + bytes(0x200 - len(header)) + b"\xc3" + bytes(0x1FF)


def with_identity(env: dict) -> dict:
    """``env`` plus the variable that points the tools at the fixture identity."""
    return {**env, common.IDENTITY_ENV: str(IDENTITY_FILE)}


class TempDirTestCase(unittest.TestCase):
    """A test case with a temporary folder in ``self.tmp``, removed afterwards."""

    def setUp(self):
        holder = tempfile.TemporaryDirectory(prefix="fv-release-test-")
        self.addCleanup(holder.cleanup)
        self.tmp = Path(holder.name)


def write_candidate(directory: Path, *, setup_only: bool = True, signed: bool = False,
                    version: str = VERSION, **overrides) -> dict:
    """Hand-build a release candidate (setup, metadata, checksums); return its metadata."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"OpenWhisper_CPP-Setup-{version}.exe").write_bytes(b"candidate fixture")
    if not setup_only:
        (directory / f"OpenWhisper_CPP-{version}-win64.zip").write_bytes(b"archive fixture")
    metadata = {"schema_version": 1, "source_qualification": "passed", "version": version,
                "commit_sha": COMMIT, "workflow_run_id": 123, "windows_setup_only": setup_only,
                "windows_signed": signed, "built_at_utc": "2026-10-01T00:00:00Z"}
    if signed:
        metadata["windows_signing"] = {"method": "trusted-signing",
                                       "signer_subject": "CN=Fiori Labs",
                                       "signer_thumbprint": "A" * 40}
    metadata.update(overrides)
    rewrite_metadata(directory, metadata)
    return metadata


def rewrite_metadata(directory: Path, metadata: dict) -> None:
    """Write the metadata and a SHA256SUMS.txt that names every file in the folder."""
    (directory / "BUILD-METADATA.json").write_text(json.dumps(metadata), encoding="utf-8")
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(directory.iterdir()) if p.name != "SHA256SUMS.txt"}
    (directory / "SHA256SUMS.txt").write_text(
        "".join(f"{digest}  {name}\n" for name, digest in hashes.items()), encoding="utf-8")
