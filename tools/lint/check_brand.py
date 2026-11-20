"""lint.brand: C++ string literals under src/ may name the upstream OpenWhisper product only in
the files whose job is to name it: base/identity.cpp (legacy_identity), backup/import* (the
importer) and remote/wire.h (protocol constants). Everything else takes product, file and
variable names from fv::identity(). Comments may name OpenWhisper: they cite the reference. So
may an #include: a header named after what it finds is a file name, not text the user sees.

Usage: check_brand.py [--root <checkout>]
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fvlint  # noqa: E402

ALLOWED = ("src/base/identity.cpp", "src/backup/import*", "src/remote/wire.h")
INCLUDE = re.compile(r"\s*#\s*include\b")
UPSTREAM_BRAND = re.compile(r"openwhisper(?![-_]?cpp)", re.IGNORECASE)


def check(root: Path) -> tuple[list[str], int]:
    problems: list[str] = []
    files = fvlint.source_files(root / "src")
    for path in files:
        relative = path.relative_to(root).as_posix()
        if any(fnmatch.fnmatch(relative, pattern) for pattern in ALLOWED):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        includes = {number for number, source in enumerate(text.splitlines(), 1)
                    if INCLUDE.match(source)}
        for kind, value, line, _, _ in fvlint.cxx_tokens(text):
            if line in includes:
                continue
            if kind == "string" and UPSTREAM_BRAND.search(value):
                problems.append(f"{relative}:{line}: string literal names OpenWhisper: "
                                f'"{value[:60]}"; use fv::identity() or legacy_identity()')
    return problems, len(files)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=fvlint.repo_root())
    args = parser.parse_args(argv)
    problems, count = check(args.root)
    return fvlint.report("lint.brand", problems, f"{count} files")


if __name__ == "__main__":
    sys.exit(main())
