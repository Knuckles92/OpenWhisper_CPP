"""Write BUILD-METADATA.json (schema 1) and SHA256SUMS.txt for a release candidate folder.

The folder holds the setup exe (and, once there is one, its native update archive). The signing
record comes from the report ``windows_signing.py verify`` wrote; without ``--signing`` the build
is recorded as unsigned. The result is checked again with ``check_release_qualification`` before
the script returns::

    python tools/release/build_metadata.py --dir out/installer --version 0.1.0 --commit <sha> \\
        --signing out/installer/windows-signing.json
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import sys
from pathlib import Path
from typing import Mapping, Optional, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_release_qualification as qualification  # noqa: E402
import release_common as common  # noqa: E402
import windows_signing  # noqa: E402


def _utc_stamp(built_at: Optional[str], env: Mapping[str, str]) -> str:
    if built_at:
        return built_at
    epoch = (env.get("SOURCE_DATE_EPOCH") or "").strip()
    moment = (datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
              if epoch.isdigit() else datetime.datetime.now(datetime.timezone.utc))
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


def write_metadata(
    directory: Path,
    *,
    version: str,
    commit: str,
    identity: Mapping,
    signing_report: Optional[Mapping] = None,
    source_qualification: str = "not-run",
    built_at: Optional[str] = None,
    source_ref: Optional[str] = None,
    repository: Optional[str] = None,
    run_id: Optional[int] = None,
    run_attempt: Optional[int] = None,
    run_url: Optional[str] = None,
    env: Mapping[str, str] = os.environ,
) -> dict:
    """Write both files into ``directory`` and return the metadata.

    Raises:
        ValueError: The version or commit is malformed, the setup exe is missing, the folder holds
            a file that is not part of a candidate, or the signing report is not for this setup.
    """
    directory = Path(directory)
    if not common.VERSION_RE.fullmatch(version):
        raise common.ReleaseToolError(f"Version must be X.Y.Z, not {version!r}")
    if not common.COMMIT_RE.fullmatch(commit):
        raise common.ReleaseToolError("Commit must be a 40-digit lowercase SHA-1")
    if source_qualification not in qualification.SOURCE_QUALIFICATIONS:
        raise common.ReleaseToolError(
            f"Source qualification must be one of {', '.join(qualification.SOURCE_QUALIFICATIONS)}")
    setup = common.setup_name(identity, version)
    archive = common.archive_name(identity, version)
    if not (directory / setup).is_file():
        raise common.ReleaseToolError(f"{setup} is not in {directory}")
    setup_only = not (directory / archive).exists()
    allowed = {setup, archive, common.SIGNING_REPORT_NAME,
               common.BUILD_METADATA_NAME, common.CHECKSUMS_NAME}
    stray = sorted(path.name for path in directory.iterdir() if path.name not in allowed)
    if stray:
        raise common.ReleaseToolError(
            f"Not part of a release candidate: {', '.join(stray)}")

    signing = (windows_signing.release_metadata(signing_report, directory, identity)
               if signing_report is not None else {"windows_signed": False})
    metadata = {
        "schema_version": qualification.METADATA_SCHEMA,
        "source_qualification": source_qualification,
        "windows_setup_only": setup_only,
        **signing,
        "version": version,
        "commit_sha": commit,
    }
    for key, value in (("source_ref", source_ref), ("built_at_utc", _utc_stamp(built_at, env)),
                       ("repository", repository), ("workflow_run_id", run_id),
                       ("workflow_run_attempt", run_attempt), ("workflow_run_url", run_url)):
        if value is not None:
            metadata[key] = value
    (directory / common.BUILD_METADATA_NAME).write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8", newline="\n")

    names = sorted(path.name for path in directory.iterdir()
                   if path.name != common.CHECKSUMS_NAME)
    lines = "".join(f"{common.sha256_of(directory / name)}  {name}\n" for name in names)
    (directory / common.CHECKSUMS_NAME).write_text(lines, encoding="utf-8", newline="\n")
    qualification.candidate_identity(directory, identity)
    return metadata


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dir", dest="directory", type=Path, required=True,
                        help="the release candidate folder")
    parser.add_argument("--version", required=True, help="X.Y.Z")
    parser.add_argument("--commit", required=True, help="the 40-digit commit the build came from")
    parser.add_argument("--signing", type=Path,
                        help="windows-signing.json from `windows_signing.py verify`")
    parser.add_argument("--identity", type=Path,
                        help="release_identity.json (default: the source tree's)")
    parser.add_argument("--source-qualification", choices=qualification.SOURCE_QUALIFICATIONS,
                        default="not-run", help="whether the full test suite passed on this commit")
    parser.add_argument("--built-at", help="UTC time stamp; default SOURCE_DATE_EPOCH, else now")
    parser.add_argument("--ref", dest="source_ref")
    parser.add_argument("--repository")
    parser.add_argument("--run-id", type=int)
    parser.add_argument("--run-attempt", type=int)
    parser.add_argument("--run-url")
    args = parser.parse_args(argv)
    try:
        identity = common.load_identity(args.identity, require=("setup_prefix",))
        report = (json.loads(args.signing.read_text(encoding="utf-8"))
                  if args.signing is not None else None)
        metadata = write_metadata(
            args.directory, version=args.version, commit=args.commit, identity=identity,
            signing_report=report, source_qualification=args.source_qualification,
            built_at=args.built_at, source_ref=args.source_ref, repository=args.repository,
            run_id=args.run_id, run_attempt=args.run_attempt, run_url=args.run_url)
    except (OSError, ValueError) as exc:
        print(f"Build metadata failed: {exc}", file=sys.stderr)
        return 1
    print(qualification.signing_summary(metadata))
    print(f"Wrote {common.BUILD_METADATA_NAME} and {common.CHECKSUMS_NAME} in {args.directory}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
