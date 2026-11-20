"""Per-area test density from classification.json (run after classify_tests.py).

    python -I -B tools/parity/coverage_table.py [<classification.json> [<areas.json>]]

Prints the a33 map's section 6 table: source lines, primary test files, files that reach the
area through secondary imports, test functions and their density, and the pure-logic share.
"""
import collections
import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]


def main(argv: list[str]) -> None:
    classification = Path(argv[0]) if argv else TOOLS / "classification.json"
    areas_path = Path(argv[1]) if len(argv) > 1 else REPO / "docs" / "port" / "areas.json"
    rows = json.loads(classification.read_text(encoding="utf-8"))
    areas = json.loads(areas_path.read_text(encoding="utf-8"))
    prim = collections.defaultdict(list)
    sec = collections.Counter()
    for r in rows:
        prim[r["area"]].append(r)
        for a in r["otherAreas"]:
            sec[a] += 1
    print("| Area | Src lines | Test files (primary) | Also touched by | Test functions "
          "| Tests per 1k src lines | Pure-logic files | Pure-logic tests |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for k, v in areas.items():
        rs = prim.get(k, [])
        t = sum(r["tests"] for r in rs)
        pl = [r for r in rs if r["kind"] == "pure-logic"]
        print(f"| {k} | {v['lines']} | {len(rs)} | {sec[k]} | {t} | {t * 1000 / v['lines']:.0f} "
              f"| {len(pl)} | {sum(r['tests'] for r in pl)} |")
    print("\ntest lines:", sum(r["lines"] for r in rows))


if __name__ == "__main__":
    main(sys.argv[1:])
