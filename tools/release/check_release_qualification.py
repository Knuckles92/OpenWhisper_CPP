"""Verify a local release candidate: checksums, build metadata, inventory and signing record.

The candidate is a folder holding the setup exe, ``BUILD-METADATA.json`` (schema 1) and
``SHA256SUMS.txt``, as ``build_metadata.py`` leaves it. This script reads that folder and a saved
draft-asset document only; it never contacts a network or a release service::

    python tools/release/check_release_qualification.py --dir out/installer
    python tools/release/check_release_qualification.py --dir out/installer --require-windows-signed
    python tools/release/check_release_qualification.py --dir out/installer \\
        --draft-assets draft.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Mapping, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_common as common  # noqa: E402
import windows_signing  # noqa: E402

METADATA_SCHEMA = 1
SOURCE_QUALIFICATIONS = ("passed", "not-run")
SIGNING_METHODS = ("certificate", "trusted-signing")


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def expected_inventory(metadata: Mapping, identity: Mapping) -> set:
    """The artifact names a candidate holds besides its metadata."""
    version = metadata["version"]
    expected = {common.setup_name(identity, version)}
    if not metadata["windows_setup_only"]:
        expected.add(common.archive_name(identity, version))
    return expected


def candidate_identity(directory: Path, identity: Optional[Mapping] = None) -> Tuple[dict, dict]:
    """Check the candidate and return its metadata and ``{artifact name: sha256}``.

    Raises:
        ValueError: A checksum is wrong or missing, the metadata is not schema 1 or lacks a
            field, the signing record is incomplete, or the folder holds a file the checksums
            do not name.
    """
    directory = Path(directory)
    if identity is None:
        identity = common.load_identity(require=("setup_prefix",))
    hashes = {}
    for line in (directory / common.CHECKSUMS_NAME).read_text(encoding="utf-8").splitlines():
        match = common.CHECKSUM_LINE_RE.fullmatch(line)
        _require(match is not None, "Candidate has an invalid checksum entry")
        digest, name = match.groups()
        _require(name not in hashes, "Candidate has duplicate checksum entries")
        path = directory / name
        _require(path.is_file() and not path.is_symlink(),
                 f"Candidate file is missing or unsafe: {name}")
        _require(common.sha256_of(path) == digest, f"Candidate checksum mismatch: {name}")
        hashes[name] = digest
    _require(common.BUILD_METADATA_NAME in hashes, "Candidate build metadata is not checksummed")
    metadata = json.loads((directory / common.BUILD_METADATA_NAME).read_text(encoding="utf-8"))
    _require(isinstance(metadata, dict), "Invalid build metadata")
    _require(metadata.get("schema_version") == METADATA_SCHEMA,
             "Candidate build metadata is not schema 1")
    _require(metadata.get("source_qualification") in SOURCE_QUALIFICATIONS,
             "Candidate does not record whether its source was qualified")
    _require(common.COMMIT_RE.fullmatch(str(metadata.get("commit_sha", ""))),
             "Invalid candidate commit")
    version = metadata.get("version")
    _require(isinstance(version, str) and common.VERSION_RE.fullmatch(version),
             "Invalid candidate version")
    _require(type(metadata.get("windows_setup_only")) is bool, "Missing Windows artifact mode")
    _require(type(metadata.get("windows_signed")) is bool, "Missing Windows signing status")
    if metadata["windows_signed"]:
        signing = metadata.get("windows_signing")
        _require(isinstance(signing, dict) and signing.get("method") in SIGNING_METHODS
                 and isinstance(signing.get("signer_subject"), str) and signing["signer_subject"],
                 "Incomplete Windows signing record")
    expected = expected_inventory(metadata, identity)
    optional = {common.SIGNING_REPORT_NAME} & set(hashes)
    _require(set(hashes) == expected | optional | {common.BUILD_METADATA_NAME},
             "Candidate artifact inventory is incomplete or unexpected")
    _require({p.name for p in directory.iterdir()} == set(hashes) | {common.CHECKSUMS_NAME},
             "Candidate directory contains unverified files")
    if optional:
        _check_signing_report(directory, metadata, identity)
    return metadata, {name: hashes[name] for name in sorted(expected)}


def _check_signing_report(directory: Path, metadata: Mapping, identity: Mapping) -> None:
    """A signing report shipped with the candidate must say what the metadata says."""
    try:
        report = json.loads((directory / common.SIGNING_REPORT_NAME).read_text(encoding="utf-8"))
        recorded = windows_signing.release_metadata(report, directory, identity)
    except windows_signing.SigningError as exc:
        raise ValueError(f"Candidate signing report is not valid: {exc}") from exc
    _require(recorded["windows_signed"] == metadata["windows_signed"]
             and recorded.get("windows_signing") == metadata.get("windows_signing"),
             "Candidate signing report disagrees with the build metadata")


def validate_draft_assets(directory: Path, release, *, uploaded: bool = False,
                          identity: Optional[Mapping] = None) -> None:
    """Reject mixed drafts and verify every asset after a verified upload.

    ``release`` is a saved ``{"assets": [{"name", "digest"}]}`` document, the shape a release
    service lists; the directory must already have passed :func:`candidate_identity`. Before an
    upload, a setup-only candidate may replace its same-version native archive; unrelated assets
    need an explicit draft cleanup.
    """
    directory = Path(directory)
    expected = {path.name: path for path in directory.iterdir()}
    assets = release.get("assets") if isinstance(release, dict) else None
    _require(isinstance(assets, list), "Could not read draft asset inventory")
    actual = {}
    for asset in assets:
        _require(isinstance(asset, dict) and isinstance(asset.get("name"), str),
                 "Invalid draft asset")
        name = asset["name"]
        _require(name not in actual, "Draft contains duplicate asset names")
        actual[name] = asset.get("digest")
    if uploaded:
        _require(set(actual) == set(expected),
                 "Uploaded draft asset inventory differs from verified candidate")
        for name, path in expected.items():
            _require(actual[name] == "sha256:" + common.sha256_of(path),
                     f"Uploaded asset checksum mismatch: {name}")
    else:
        if identity is None:
            identity = common.load_identity(require=("setup_prefix",))
        metadata = json.loads((directory / common.BUILD_METADATA_NAME).read_text(encoding="utf-8"))
        removable = ({common.archive_name(identity, metadata["version"])}
                     if metadata["windows_setup_only"] else set())
        _require(set(actual) <= set(expected) | removable,
                 "Draft contains unrelated assets; "
                 "clean up the draft before uploading this candidate")


def signing_summary(metadata: Mapping) -> str:
    """One line for the job log: whether Windows users get a signed build."""
    if not metadata["windows_signed"]:
        return "Windows executables and installer: unsigned (SmartScreen will warn)."
    signing = metadata["windows_signing"]
    return (f"Windows executables and installer: signed via {signing['method']} "
            f"by {signing['signer_subject']}.")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dir", "--candidate", dest="directory", type=Path, required=True,
                        help="the release candidate folder")
    parser.add_argument("--identity", type=Path,
                        help="release_identity.json (default: the source tree's)")
    parser.add_argument("--expected-commit")
    parser.add_argument("--expected-run", type=int)
    parser.add_argument("--require-windows-signed", action="store_true",
                        help="fail unless the Windows build is signed")
    parser.add_argument("--require-source-qualification", action="store_true",
                        help="fail unless the build metadata says the source passed qualification")
    parser.add_argument("--draft-assets", type=Path,
                        help="a saved {\"assets\": [...]} document to check against the "
                             "candidate")
    parser.add_argument("--uploaded", action="store_true",
                        help="with --draft-assets: the assets are what an upload left behind")
    args = parser.parse_args(argv)
    try:
        identity = common.load_identity(args.identity, require=("setup_prefix",))
        metadata, _ = candidate_identity(args.directory, identity)
        if args.expected_commit:
            _require(metadata["commit_sha"] == args.expected_commit,
                     "Release tag moved or candidate commit differs")
        if args.expected_run:
            _require(metadata.get("workflow_run_id") == args.expected_run,
                     "Candidate is from a different build run")
        if args.require_windows_signed:
            _require(metadata["windows_signed"],
                     "Windows build is unsigned but signing is required")
        if args.require_source_qualification:
            _require(metadata["source_qualification"] == "passed",
                     "Candidate source was not qualified but qualification is required")
        if args.draft_assets:
            validate_draft_assets(
                args.directory, json.loads(args.draft_assets.read_text(encoding="utf-8")),
                uploaded=args.uploaded, identity=identity)
        print(signing_summary(metadata))
        print("Candidate checksums, build metadata and inventory passed.")
        return 0
    except (OSError, ValueError, TypeError, KeyError) as exc:
        print(f"Release qualification failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
