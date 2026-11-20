"""Render the declared differences between OpenWhisper and FioriVoce."""
import argparse
from itertools import groupby
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from feature_audit import ROOT, audit


def render(rows):
    lines = ["# Deliberate differences from OpenWhisper", ""]
    order = sorted(rows, key=lambda r: (r["area"], r["id"]))
    for area, group in groupby(order, key=lambda r: r["area"]):
        lines += [f"## {area}", ""]
        for entry in group:
            lines += [f"### {entry['id']}", "", f"OpenWhisper. {entry['openwhisper']}", "",
                      f"FioriVoce. {entry['fiorivoce']}", "", f"{entry['reason']} ({entry['source']})", ""]
    return "\n".join(lines).rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    result = audit(args.root)
    if result["errors"]:
        print("\n".join(result["errors"]), file=sys.stderr)
        return 1
    out = args.out or args.root / "docs/port/differences.md"
    out.write_bytes(render(result["differences"]).encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
