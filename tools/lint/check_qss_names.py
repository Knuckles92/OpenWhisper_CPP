"""lint.qss_names: every #objectName and [property] that a selector of the theme names is used by
some C++ file, or is listed in tools/lint/qss_unused.txt (the features not ported yet; the list
shrinks each milestone). An object name counts as used when it appears as a string literal under
src/; a property when ui_theme/style_props.h declares it or a literal spells it. The sheets are
src/ui_theme/resources/theme.qss and every src/<dir>/resources/<dir>.qss.

Usage: check_qss_names.py [--root <checkout>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fvlint  # noqa: E402

OBJECT_NAME = re.compile(r"#([A-Za-z_][\w-]*)")
PROPERTY = re.compile(r"\[\s*([A-Za-z_][\w-]*)\s*(?:[~|^$*]?=|\])")
IDENTIFIER = re.compile(r"\b([A-Za-z_]\w*)\b")


def selectors(qss: str) -> str:
    """The selector text of a style sheet: comments removed, declaration blocks dropped."""
    qss = re.sub(r"/\*.*?\*/", "", qss, flags=re.DOTALL)
    out: list[str] = []
    depth = 0
    for char in qss:
        if char == "{":
            depth += 1
        elif char == "}":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(char)
    return "".join(out)


def sheets(root: Path) -> list[Path]:
    src = root / "src"
    return sorted(p for p in src.glob("*/resources/*.qss")
                  if p.name == "theme.qss" or p.stem == p.parent.parent.name)


def allowlist(root: Path) -> set[str]:
    path = root / "tools" / "lint" / "qss_unused.txt"
    if not path.exists():
        return set()
    # Entries start with "#" or "[", so comments start with "//".
    entries = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("//"):
            entries.add(line)
    return entries


def check(root: Path) -> tuple[list[str], int]:
    found = sheets(root)
    if not found:
        return [], 0
    names: set[str] = set()
    props: set[str] = set()
    for sheet in found:
        text = selectors(sheet.read_text(encoding="utf-8"))
        names.update(OBJECT_NAME.findall(text))
        props.update(PROPERTY.findall(text))

    literals: set[str] = set()
    for path in fvlint.source_files(root / "src"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for kind, value, *_ in fvlint.cxx_tokens(text):
            if kind == "string":
                literals.add(value)
    style_props = root / "src" / "ui_theme" / "style_props.h"
    declared = set()
    if style_props.exists():
        declared = set(IDENTIFIER.findall(style_props.read_text(encoding="utf-8")))

    allowed = allowlist(root)
    problems = []
    for name in sorted(names):
        if name not in literals and f"#{name}" not in allowed:
            problems.append(f"#{name} is styled by the theme but no C++ file names an object "
                            "so; port it or add it to tools/lint/qss_unused.txt")
    for prop in sorted(props):
        if prop not in literals and prop not in declared and f"[{prop}]" not in allowed:
            problems.append(f"[{prop}] is styled by the theme but neither style_props.h nor a "
                            "literal names it; add it to tools/lint/qss_unused.txt if unported")
    return problems, len(found)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=fvlint.repo_root())
    args = parser.parse_args(argv)
    problems, count = check(args.root)
    return fvlint.report("lint.qss_names", problems,
                         f"{count} style sheets" if count else "no theme.qss in the tree yet")


if __name__ == "__main__":
    sys.exit(main())
