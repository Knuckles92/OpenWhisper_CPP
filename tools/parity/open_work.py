"""List the OpenWhisper test functions the ledgers leave open: pending, or named nowhere.

    python -I -B tools/parity/open_work.py                  totals, and the files with open work
    python -I -B tools/parity/open_work.py test_x.py ...    the open functions of those files

The parity report counts functions; this names them. OPENWHISPER_CPP_OW_ROOT is the OpenWhisper
checkout (read only): a function is "not listed" when pytest would collect it there (a module
level test function, or a test method of a Test class) and no tests/*/PARITY.json has an entry.
"""
from __future__ import annotations

import ast
import collections
import json
import os
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
import parity_report as pr  # noqa: E402


def collected(path: Path) -> list[str]:
    tree = ast.parse(path.read_text(encoding="utf-8"))
    out = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name.startswith("test"):
            out.append(node.name)
        elif isinstance(node, ast.ClassDef) and node.name.startswith("Test"):
            out += [f"{node.name}::{sub.name}" for sub in node.body
                    if isinstance(sub, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and sub.name.startswith("test")]
    return out


def open_work(ow: Path) -> list[dict]:
    rows = json.loads((REPO / "tools/parity/classification.json").read_text(encoding="utf-8"))
    found, _ = pr.load_parity_files(REPO)
    merged = pr.merge([(p.relative_to(REPO).as_posix(), d) for p, d in found])
    named_in = collections.defaultdict(set)
    for path, document in found:
        for test_file in document.get("files", {}):
            named_in[Path(test_file).name].add(path.relative_to(REPO).as_posix())
    work = []
    for row in rows:
        name = row["file"]
        entries = merged.get(name, {})
        source = ow / "tests" / name
        unlisted = [f for f in collected(source) if f not in entries] if source.is_file() else []
        pending = {f: e for f, e in entries.items() if e["status"] == "pending"}
        if pending or unlisted or row["tests"] != len(entries):
            work.append({"file": name, "area": row["area"], "kind": row["kind"],
                         "pending": pending, "unlisted": unlisted,
                         "counted": row["tests"], "listed": len(entries),
                         "ledgers": sorted(named_in.get(name, []))})
    return work


def main(argv: list[str]) -> int:
    ow = Path(os.environ.get("OPENWHISPER_CPP_OW_ROOT", ""))
    if not os.environ.get("OPENWHISPER_CPP_OW_ROOT") or not (ow / "tests").is_dir():
        print("open_work: set OPENWHISPER_CPP_OW_ROOT to the OpenWhisper checkout")
        return 2
    work = open_work(ow)
    only = set(argv)
    if only:
        work = [w for w in work if w["file"] in only]
        for w in work:
            print(f"\n## tests/{w['file']}  ({w['area']}, {w['kind']}): {len(w['pending'])} pending, "
                  f"{len(w['unlisted'])} not listed; named in {', '.join(w['ledgers']) or 'no PARITY.json'}")
            for function, entry in w["pending"].items():
                print(f"  pending  {function}  [{entry['owner']}] {entry['reason']}")
            for function in w["unlisted"]:
                print(f"  unlisted {function}")
            if not w["pending"] and not w["unlisted"]:
                print(f"  the ledgers list {w['listed']} functions, classification.json counts {w['counted']}")
        for name in sorted(only - {w["file"] for w in work}):
            print(f"\n## tests/{name}: nothing open")
    else:
        for w in sorted(work, key=lambda w: -(len(w["pending"]) + len(w["unlisted"]))):
            print(f"{len(w['pending']):4d} pending {len(w['unlisted']):4d} not listed  {w['file']}  ({w['area']})")
    print(f"\n{len(work)} files, {sum(len(w['pending']) for w in work)} pending, "
          f"{sum(len(w['unlisted']) for w in work)} not listed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
