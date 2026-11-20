"""Generate THIRD_PARTY_NOTICES.md and licenses/ for an install tree.

Reads the licence of every library vcpkg installed for the build
(``<vcpkg_installed>/<triplet>/share/<port>/copyright``), the licence files of the Qt kit, and the
hand-written notices in ``tools/release/notices/*.md`` (Qt, Tabler Icons, the speech runtime and
models, and the meeting dashboard's React bundle and fonts). It writes, under ``--out``::

    THIRD_PARTY_NOTICES.md
    licenses/vcpkg/<port>/copyright
    licenses/qt/<licence files of the kit>

    python tools/release/generate_notices.py \\
        --vcpkg-installed out/build/windows-ci/vcpkg_installed --triplet x64-windows \\
        --qt D:/Qt/6.10.3/msvc2022_64 --out out/stage

A port that installs files but has no ``copyright`` file is an error: the installer must not ship
code whose licence it cannot show. A port that installs no code (vcpkg's internal helper ports)
is listed without one. A Qt kit with no licence files is a warning, or an error with
``--require-qt-licenses`` (the kit from the Qt online installer has them; a self-built one may
not).
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path
from typing import List, NamedTuple, Optional, Sequence, Tuple

PRODUCT = "FioriVoce"
NOTICES_DIR = Path(__file__).resolve().parent / "notices"
QT_SOURCE_LICENSES = Path(__file__).resolve().parents[2] / "packaging/windows/qt-licenses"

_PORT_NAME_RE = re.compile(r"[a-z0-9][a-z0-9-]*")
# Folders inside a port that mean it installs something a build links or ships.
_CODE_DIRS = ("bin/", "debug/bin/", "lib/", "debug/lib/", "include/", "tools/")
_QT_LICENSE_NAME_RE = re.compile(r"(?i)(lgpl|gpl|licen[cs]e|copying|exception)")
_MAX_LICENSE_BYTES = 1 << 20


class NoticesError(ValueError):
    """A port lacks its licence, or the vcpkg tree is not one."""


class Port(NamedTuple):
    name: str
    version: str
    copyright: Optional[Path]  # None for a port that installs no code


def installed_ports(vcpkg_installed: Path, triplet: str) -> List[Port]:
    """Every port vcpkg installed for ``triplet``, by name, with its copyright file.

    Raises:
        NoticesError: The tree has no port list, or a port that installs code has no copyright.
    """
    info = Path(vcpkg_installed) / "vcpkg" / "info"
    if not info.is_dir():
        raise NoticesError(f"{info} not found: is this a vcpkg_installed folder?")
    suffix = f"_{triplet}.list"
    ports = []
    for listing in sorted(info.glob(f"*{suffix}")):
        name, separator, version = listing.name[: -len(suffix)].partition("_")
        if not separator or not _PORT_NAME_RE.fullmatch(name) or not version:
            raise NoticesError(f"Unreadable vcpkg port list name: {listing.name}")
        copyright_file = Path(vcpkg_installed) / triplet / "share" / name / "copyright"
        if copyright_file.is_file():
            ports.append(Port(name, version, copyright_file))
        elif _installs_code(listing, triplet):
            raise NoticesError(f"Port {name} {version} has no copyright file ({copyright_file})")
        else:
            ports.append(Port(name, version, None))
    if not ports:
        raise NoticesError(f"No ports for {triplet} in {info}")
    return ports


def _installs_code(listing: Path, triplet: str) -> bool:
    prefix = f"{triplet}/"
    for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line.startswith(prefix) and not line.endswith("/"):
            if line[len(prefix):].startswith(_CODE_DIRS):
                return True
    return False


def qt_license_files(qt_root: Path) -> List[Path]:
    """Licence files of a Qt kit, wherever the kit's layout keeps them, without duplicates."""
    qt_root = Path(qt_root)
    folders = [qt_root, qt_root / "Licenses", qt_root / "LICENSES",
               qt_root / "share" / "qt6" / "licenses"]
    for parent in (qt_root.parent, qt_root.parent.parent):
        folders += [parent / "Licenses", parent / "LICENSES"]
    found, seen = [], set()
    for folder in folders:
        if not folder.is_dir():
            continue
        for path in sorted(folder.iterdir()):
            if (path.is_file() and not path.is_symlink() and path.name not in seen
                    and _QT_LICENSE_NAME_RE.search(path.name)
                    and path.stat().st_size <= _MAX_LICENSE_BYTES):
                seen.add(path.name)
                found.append(path)
    if found:
        return found
    # A minimal binary kit may omit license documents. Match its actual CMake version before
    # using the texts vendored from that release of QtBase and QtSvg.
    version_file = qt_root / "lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
    if version_file.is_file():
        version = re.search(r'^set\(PACKAGE_VERSION "(\d+\.\d+\.\d+)"\)',
                            version_file.read_text(encoding="utf-8"), re.MULTILINE)
        if version:
            return sorted((QT_SOURCE_LICENSES / version[1]).glob("*.txt"))
    return []


def read_fragments(directory: Path) -> List[Tuple[str, str]]:
    fragments = []
    for path in sorted(Path(directory).glob("*.md")):
        text = path.read_text(encoding="utf-8").strip()
        if text:
            fragments.append((path.name, text))
    if not fragments:
        raise NoticesError(f"No notice fragments in {directory}")
    return fragments


def render(product: str, fragments: Sequence[Tuple[str, str]], ports: Sequence[Port],
           qt_files: Sequence[str], triplet: str) -> str:
    lines = [
        "# Third-party notices", "",
        f"{product} includes third-party software. Each component stays under its own licence; "
        f"nothing here relicenses it, and this file does not replace the terms that come with it.",
        "",
    ]
    for _, text in fragments:
        lines += [text, ""]
    lines += ["## Licence texts", "", "### Qt", ""]
    if qt_files:
        lines += [f"- [{name}](licenses/qt/{name})" for name in qt_files]
    else:
        lines += ["The Qt kit this build used did not include licence files. The Qt licences are "
                  "published at <https://www.qt.io/licensing/> and the LGPL-3.0 text at "
                  "<https://www.gnu.org/licenses/lgpl-3.0.html>."]
    lines += ["", f"### Libraries built with vcpkg ({triplet})", "",
              "| Library | Version | Licence |", "| --- | --- | --- |"]
    for port in ports:
        text = (f"[copyright](licenses/vcpkg/{port.name}/copyright)" if port.copyright
                else "none (installs no code)")
        lines.append(f"| {port.name} | {port.version} | {text} |")
    return "\n".join(lines) + "\n"


def generate(vcpkg_installed: Path, triplet: str, qt_root: Optional[Path], out: Path,
             *, notices_dir: Path = NOTICES_DIR, product: str = PRODUCT,
             require_qt_licenses: bool = False) -> Tuple[Path, int, int]:
    """Write the notices and licence copies under ``out``; return the file and the two counts.

    The counts are the vcpkg ports with a licence text and the Qt licence files copied.
    """
    ports = installed_ports(vcpkg_installed, triplet)
    fragments = read_fragments(notices_dir)
    qt_files = qt_license_files(qt_root) if qt_root is not None else []
    if require_qt_licenses and not qt_files:
        raise NoticesError(f"No Qt licence files found for {qt_root}")
    out = Path(out)
    licenses = out / "licenses"
    if licenses.exists():
        shutil.rmtree(licenses)
    for port in ports:
        if port.copyright:
            target = licenses / "vcpkg" / port.name / "copyright"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(port.copyright, target)
    for path in qt_files:
        target = licenses / "qt" / path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    notices = out / "THIRD_PARTY_NOTICES.md"
    out.mkdir(parents=True, exist_ok=True)
    notices.write_text(render(product, fragments, ports, [p.name for p in qt_files], triplet),
                       encoding="utf-8", newline="\n")
    return notices, sum(1 for port in ports if port.copyright), len(qt_files)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--vcpkg-installed", type=Path, required=True,
                        help="the build's vcpkg_installed folder")
    parser.add_argument("--triplet", default="x64-windows")
    parser.add_argument("--qt", type=Path, help="the Qt kit (Qt6_ROOT) whose licence files to copy")
    parser.add_argument("--out", type=Path, required=True, help="the install tree to write into")
    parser.add_argument("--notices", type=Path, default=NOTICES_DIR,
                        help="folder of hand-written notice fragments")
    parser.add_argument("--require-qt-licenses", action="store_true",
                        help="fail when the Qt kit has no licence files")
    args = parser.parse_args(argv)
    try:
        notices, ports, qt_files = generate(
            args.vcpkg_installed, args.triplet, args.qt, args.out,
            notices_dir=args.notices, require_qt_licenses=args.require_qt_licenses)
    except (OSError, ValueError) as exc:
        print(f"Notices failed: {exc}", file=sys.stderr)
        return 1
    if args.qt is not None and qt_files == 0:
        print(f"warning: no Qt licence files found under {args.qt}; the notices point to the "
              f"published texts", file=sys.stderr)
    print(f"Wrote {notices} ({ports} vcpkg licence texts, {qt_files} Qt licence files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
