"""What the release tools share: the release identity file and the names derived from it.

The identity is ``src/update/data/release_identity.json`` (the file the application embeds), so
the setup name, the executable names and the timestamp server are written in one place. A tool
reads it by path at run time; ``--identity <file>`` or ``OPENWHISPER_CPP_RELEASE_IDENTITY`` names
another copy, which is how the tests run without the source tree. Keys a tool does not know are
ignored.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path
from typing import Mapping, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]
IDENTITY_ENV = "OPENWHISPER_CPP_RELEASE_IDENTITY"
DEFAULT_IDENTITY_PATH = REPO_ROOT / "src" / "update" / "data" / "release_identity.json"

VERSION_RE = re.compile(r"\d+\.\d+\.\d+")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")

BUILD_METADATA_NAME = "BUILD-METADATA.json"
CHECKSUMS_NAME = "SHA256SUMS.txt"
SIGNING_REPORT_NAME = "windows-signing.json"
# A checksum line is the digest, two spaces and a file name: the format `sha256sum` writes. The
# updater's parser also takes one space plus a space or "*", so this stays inside what it reads.
CHECKSUM_LINE_RE = re.compile(r"([a-f0-9]{64})  ([A-Za-z0-9_.-]+)")

_CHUNK = 1024 * 1024


class ReleaseToolError(ValueError):
    """A release tool's input is missing, malformed or inconsistent."""


def identity_path(explicit: Optional[Path] = None, env: Mapping[str, str] = os.environ) -> Path:
    if explicit is not None:
        return Path(explicit)
    named = (env.get(IDENTITY_ENV) or "").strip()
    return Path(named) if named else DEFAULT_IDENTITY_PATH


def load_identity(
    path: Optional[Path] = None,
    *,
    env: Mapping[str, str] = os.environ,
    require: Sequence[str] = (),
) -> dict:
    """Read the release identity and check that the keys the caller needs are non-empty strings."""
    where = identity_path(path, env)
    try:
        document = json.loads(where.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise ReleaseToolError(
            f"Release identity not found: {where} (pass --identity or set {IDENTITY_ENV})"
        ) from None
    except (OSError, ValueError) as exc:
        raise ReleaseToolError(f"Release identity is unreadable: {where}: {exc}") from exc
    if not isinstance(document, dict):
        raise ReleaseToolError(f"Release identity is not an object: {where}")
    for key in require:
        value = document.get(key)
        if not isinstance(value, str) or not value.strip():
            raise ReleaseToolError(f"Release identity has no \"{key}\": {where}")
    return document


def product_name(identity: Mapping) -> str:
    """The product's name as it begins the release file names (``OpenWhisper_CPP``).

    ``setup_prefix`` may be spelled ``OpenWhisper_CPP``, ``OpenWhisper_CPP-Setup`` or ``OpenWhisper_CPP-Setup-``;
    all three give the same product.
    """
    prefix = str(identity.get("setup_prefix", "")).strip().rstrip("-")
    if prefix.endswith("-Setup"):
        prefix = prefix[: -len("-Setup")]
    if not re.fullmatch(r"[A-Za-z0-9_.]+(?:-[A-Za-z0-9_.]+)*", prefix):
        raise ReleaseToolError("Release identity setup_prefix does not name a product")
    return prefix


def setup_name(identity: Mapping, version: str) -> str:
    return f"{product_name(identity)}-Setup-{version}.exe"


def setup_name_pattern(identity: Mapping) -> re.Pattern:
    return re.compile(re.escape(product_name(identity)) + r"-Setup-(\d+\.\d+\.\d+)\.exe")


def archive_name(identity: Mapping, version: str) -> str:
    """The native update archive that may sit beside a setup (absent from setup-only releases)."""
    return f"{product_name(identity)}-{version}-win64.zip"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        while block := handle.read(_CHUNK):
            digest.update(block)
    return digest.hexdigest()
