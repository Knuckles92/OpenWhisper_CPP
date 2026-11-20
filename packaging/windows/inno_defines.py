"""Print the ISCC /D arguments FioriVoce.iss is compiled with, one per line.

The AppId, the publisher, the folder names, the executable names and the named mutexes come
from the release identity (src/update/data/release_identity.json), the same file the application
embeds, so the setup and the running app cannot disagree about them::

    python packaging/windows/inno_defines.py --version 0.1.0 --source out/stage \\
        --output-dir out/installer [--identity release_identity.json]
    python packaging/windows/inno_defines.py --check-stage --source out/stage

The mutex names and the update switch are built the way src/update spells them
(ReleaseIdentity::app_mutex, app_mutex_global, setup_mutex; setup_update_switch): Inno's AppMutex
must match the app's mutex exactly to see it running, and the app's launch gate must match
SetupMutex to see a running setup.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "release"))
import release_common as common  # noqa: E402

GUID_RE = re.compile(r"\{([0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12})\}")
# Values ISCC passes through to directives where Inno would read a brace as a constant or a
# quote as the end of the argument.
_UNSAFE = re.compile(r'[{}"\r\n]')


def bare_guid(identity: Mapping) -> str:
    """The AppId GUID without braces; the identity must hold it braced and upper-case."""
    match = GUID_RE.fullmatch(str(identity.get("app_id", "")))
    if not match:
        raise common.ReleaseToolError("Release identity app_id must be a braced upper-case GUID")
    return match.group(1)


def app_mutexes(identity: Mapping) -> List[str]:
    plain = f"{common.product_name(identity)}-App-{bare_guid(identity)}"
    return [plain, "Global\\" + plain]


def setup_mutex(identity: Mapping) -> str:
    return f"Global\\{common.product_name(identity)}-Setup-{bare_guid(identity)}"


def update_parameter(identity: Mapping) -> str:
    """The name of the /<NAME>=1 switch the app passes when it hands an update to the setup."""
    return common.product_name(identity).upper() + "UPDATE"


def defines(identity: Mapping, *, version: str, source: Path, output_dir: Path) -> Dict[str, str]:
    """The defines by name, in the order they are printed."""
    if not common.VERSION_RE.fullmatch(version):
        raise common.ReleaseToolError(f"Version must be X.Y.Z, not {version!r}")
    for key in ("publisher", "install_dir_name", "app_exe", "cli_exe"):
        value = identity.get(key)
        if not isinstance(value, str) or not value.strip():
            raise common.ReleaseToolError(f"Release identity has no \"{key}\"")
    product = common.product_name(identity)
    values = {
        "AppVersion": version,
        "AppName": product,
        "AppGuid": bare_guid(identity),
        "AppPublisher": identity["publisher"],
        "InstallDirName": identity["install_dir_name"],
        # The user data folder is named after the product (base/identity.cpp data_dir_name).
        "DataDirName": product,
        "AppExeName": identity["app_exe"],
        "CliExeName": identity["cli_exe"],
        "AppMutexNames": ",".join(app_mutexes(identity)),
        "SetupMutexName": setup_mutex(identity),
        "UpdateParam": update_parameter(identity),
        "SourceDir": str(Path(source).resolve()),
        "OutputDir": str(Path(output_dir).resolve()),
        "OutputBaseFilename": common.setup_name(identity, version)[: -len(".exe")],
    }
    url = identity.get("publisher_url")
    # Without a URL the script leaves the three URL directives out (it tests #ifdef AppURL).
    if isinstance(url, str) and url.strip():
        values["AppURL"] = url.strip()
    for name, value in values.items():
        if _UNSAFE.search(value):
            raise common.ReleaseToolError(f"{name} cannot hold braces, quotes or line breaks")
    return values


def iscc_arguments(values: Mapping[str, str]) -> List[str]:
    return [f"/D{name}={value}" for name, value in values.items()]


SCRIPT = Path(__file__).resolve().with_name("FioriVoce.iss")
_INSTALL_DELETE = re.compile(r"^\[InstallDelete\][ \t]*$(.*?)(?=^\[|\Z)", re.M | re.S)
_DELETE_ENTRY = re.compile(r'^\s*Type:\s*(files|filesandordirs)\s*;\s*Name:\s*"\{app\}\\([^"\\]+)"',
                           re.I)


def install_delete_patterns(script: str) -> List[tuple]:
    """(type, pattern) of each ``{app}\\<pattern>`` entry of the script's [InstallDelete]."""
    match = _INSTALL_DELETE.search(script)
    if not match:
        return []
    entries = []
    for line in match.group(1).splitlines():
        entry = _DELETE_ENTRY.match(line)
        if entry:
            entries.append((entry.group(1).lower(), entry.group(2).lower()))
    return entries


def unmanaged_entries(stage: Path, script: str) -> List[str]:
    """The top-level files and folders of the staged tree that no [InstallDelete] entry clears.

    Inno never deletes a file an older version installed and a newer one does not, so a name the
    setup installs that [InstallDelete] misses would outlive the version that stops shipping it
    (a retired Qt plugin folder that windeployqt adds or drops, for instance). A folder needs a
    ``filesandordirs`` entry; a file needs any entry whose wildcard matches it.
    """
    patterns = install_delete_patterns(script)
    missing = []
    for entry in sorted(Path(stage).iterdir(), key=lambda p: p.name.lower()):
        name = entry.name.lower()
        kinds = ("filesandordirs",) if entry.is_dir() else ("files", "filesandordirs")
        if not any(kind in kinds and fnmatch.fnmatchcase(name, pattern)
                   for kind, pattern in patterns):
            missing.append(entry.name + ("\\" if entry.is_dir() else ""))
    return missing


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--version", help="X.Y.Z")
    parser.add_argument("--source", type=Path, required=True, help="the staged install tree")
    parser.add_argument("--output-dir", type=Path, help="where ISCC writes the setup exe")
    parser.add_argument("--identity", type=Path,
                        help="release_identity.json (default: the source tree's)")
    parser.add_argument("--check-stage", action="store_true",
                        help="instead of printing the defines, check that the script's "
                             "[InstallDelete] clears every top-level name of --source")
    args = parser.parse_args(argv)
    if args.check_stage:
        try:
            missing = unmanaged_entries(args.source, SCRIPT.read_text(encoding="utf-8"))
        except OSError as exc:
            print(f"Stage check failed: {exc}", file=sys.stderr)
            return 1
        for name in missing:
            print(f"{SCRIPT.name} [InstallDelete] does not clear {{app}}\\{name}", file=sys.stderr)
        return 1 if missing else 0
    if not args.version or not args.output_dir:
        parser.error("--version and --output-dir are required to print the defines")
    try:
        identity = common.load_identity(args.identity, require=("app_id", "setup_prefix"))
        values = defines(identity, version=args.version, source=args.source,
                         output_dir=args.output_dir)
    except (OSError, ValueError) as exc:
        print(f"Inno defines failed: {exc}", file=sys.stderr)
        return 1
    for line in iscc_arguments(values):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
