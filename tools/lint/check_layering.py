"""lint.layering: every file under src/<dir> includes only headers of its own module and of the
modules in its declared DEPS closure, never another module's detail/ headers, and a core module
includes no Qt header; each module's DEPS respect the kinds of architecture section 2.2.

Usage: check_layering.py [--root <checkout>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fvlint  # noqa: E402

QT_HEADER = re.compile(r"^(Q[A-Z]\w*|Qt\w*/.*|q\w+\.h)$")


def check(root: Path) -> list[str]:
    src = root / "src"
    modules = fvlint.load_modules(src)
    problems: list[str] = []

    for module in modules.values():
        if module.kind is None:
            continue
        if module.kind not in fvlint.KINDS:
            problems.append(f"src/{module.dir}: unknown KIND {module.kind}")
            continue
        allowed = fvlint.ALLOWED_DEP_KINDS[module.kind]
        for dep in module.deps:
            target = modules.get(dep[3:]) if dep.startswith("fv_") else None
            if target is None or target.kind is None:
                if module.kind != "exe":
                    problems.append(
                        f"src/{module.dir}: DEPS names {dep}, which is not an fv_module")
            elif target.kind not in allowed:
                problems.append(
                    f"src/{module.dir} ({module.kind}) may not depend on {dep} ({target.kind})")

    for module in modules.values():
        if module.kind is None or module.kind == "exe":
            continue
        reachable = fvlint.closure(modules, module.dir)
        for path in fvlint.source_files(src / module.dir):
            relative = path.relative_to(root).as_posix()
            text = path.read_text(encoding="utf-8", errors="replace")
            for line, delimiter, header in fvlint.includes(text):
                where = f"{relative}:{line}"
                if delimiter == "<":
                    if module.kind == "core" and QT_HEADER.match(header):
                        problems.append(f"{where}: core module {module.target} includes Qt "
                                        f"header <{header}>")
                    continue
                parts = header.split("/")
                if len(parts) < 2 or parts[0] not in modules:
                    continue
                other = parts[0]
                if other != module.dir and "detail" in parts[1:-1]:
                    problems.append(f"{where}: includes {header}, a detail header of fv_{other}")
                elif other not in reachable:
                    problems.append(f"{where}: includes {header} but {module.target} does not "
                                    f"depend on fv_{other}")
                elif module.kind == "core" and modules[other].kind not in (None, "core"):
                    problems.append(f"{where}: core module {module.target} includes {header} "
                                    f"of {modules[other].kind} module fv_{other}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=fvlint.repo_root())
    args = parser.parse_args(argv)
    modules = fvlint.load_modules(args.root / "src")
    checked = sum(1 for m in modules.values() if m.kind is not None)
    return fvlint.report("lint.layering", check(args.root), f"{checked} modules")


if __name__ == "__main__":
    sys.exit(main())
