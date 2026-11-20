"""lint.ownership: every file a work package changed lies inside what the package owns, and no
frozen file changed unless the package owns it by name, or owns the whole frozen folder
(cmake/), as milestone-start packages do (architecture 9.5 and 10.1).

The package's owned paths come from --owns or the OPENWHISPER_CPP_OWNS environment variable
(separated by ";" or new lines): a path ending in "/" owns that folder, anything else is a glob
(fnmatch, "*" crosses "/"). Owning src/<dir>/ also owns tests/<dir>/. Changes are taken against
--base or OPENWHISPER_CPP_BASE_REF, by default the merge base of HEAD and port/parity, and include
uncommitted and untracked files. Without owned paths there is no package to check, and the
check passes.

Usage: check_ownership.py [--root <checkout>] [--base <ref>] [--owns <path>...]
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fvlint  # noqa: E402

# Lead-owned files, edited only by milestone-start packages (architecture 10.1).
FROZEN = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "cmake/*",
    "src/settings/keys.inc",
    "src/appkit/order.h",
    "src/appkit/destinations.h",
    "src/ui_theme/style_props.h",
    "src/ui_theme/resources/theme.qss",
    "CLAUDE.md",
    "BUILDING.md",
)


def git(root: Path, *args: str) -> str:
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                            encoding="utf-8", check=False)
    if result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def changed_files(root: Path, base: str) -> list[str]:
    changed = set(git(root, "diff", "--name-only", base).split("\n"))
    changed |= set(git(root, "ls-files", "--others", "--exclude-standard").split("\n"))
    return sorted(path for path in changed if path)


def owns(patterns: list[str], path: str) -> bool:
    for pattern in patterns:
        if pattern.endswith("/"):
            if path.startswith(pattern):
                return True
        elif fnmatch.fnmatchcase(path, pattern):
            return True
    return False


def expand(patterns: list[str]) -> list[str]:
    expanded = list(patterns)
    for pattern in patterns:
        match = re.fullmatch(r"src/([^/*]+)/", pattern)
        if match:
            expanded.append(f"tests/{match.group(1)}/")
    return expanded


def owns_frozen(patterns: list[str], path: str, frozen: str) -> bool:
    """A frozen file may change only when the package names it: the file itself, or the whole
    frozen folder it is in ("cmake/" or "cmake/*" for cmake/*). Owning an enclosing source folder
    such as src/settings/ is not enough."""
    if path in patterns or frozen in patterns:
        return True
    return frozen.endswith("/*") and frozen[:-1] in patterns


def check(root: Path, base: str, patterns: list[str]) -> list[str]:
    owned = expand(patterns)
    problems = []
    for path in changed_files(root, base):
        frozen = next((f for f in FROZEN if fnmatch.fnmatchcase(path, f)), None)
        if frozen is not None:
            if not owns_frozen(patterns, path, frozen):
                problems.append(f"{path}: frozen file changed (only a package naming it in its "
                                "owned paths may change it)")
        elif not owns(owned, path):
            problems.append(f"{path}: changed outside the package's owned paths")
    return problems


def default_base(root: Path) -> str:
    try:
        return git(root, "merge-base", "HEAD", "port/parity").strip()
    except RuntimeError:
        return "HEAD"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=fvlint.repo_root())
    parser.add_argument("--base", default=os.environ.get("OPENWHISPER_CPP_BASE_REF"))
    parser.add_argument("--owns", nargs="*", default=None)
    args = parser.parse_args(argv)

    patterns = args.owns
    if patterns is None:
        patterns = [p.strip() for p in re.split(r"[;\n]", os.environ.get("OPENWHISPER_CPP_OWNS", ""))]
    patterns = [p for p in patterns if p]
    if not patterns:
        print("lint.ownership: ok (no package declared: set OPENWHISPER_CPP_OWNS to check one)")
        return 0
    if not (args.root / ".git").exists():
        print("lint.ownership: ok (not a git checkout)")
        return 0
    base = args.base or default_base(args.root)
    return fvlint.report("lint.ownership", check(args.root, base, patterns),
                         f"changes since {base[:12]}")


if __name__ == "__main__":
    sys.exit(main())
