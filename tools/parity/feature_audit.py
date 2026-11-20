"""Check implementation and test evidence against the port's behavior inventory."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
FEATURE = re.compile(r"^\|\s*(A\d{2}-[A-Z]?\d+[a-z]?)\s*\|", re.M)
CHECK = re.compile(r"^- \[[ xX]\]\s+([A-Z][A-Z0-9]*-\d+[a-z]?)\b", re.M)
STATES = {"verified", "fixed", "difference", "dropped", "other-platform", "handoff"}


def inventory(root: Path) -> dict[str, str]:
    ids = {}
    maps = root / "docs/port/map"
    for path in sorted(maps.glob("a*.md")):
        text = path.read_text(encoding="utf-8")
        for key in FEATURE.findall(text):
            ids.setdefault(key, path.stem)
        if path.name.startswith("a32-"):
            ids.update((key, path.stem) for key in CHECK.findall(text))
    harness = root / "docs/port/packages/M0.md"
    if harness.exists():
        for key in FEATURE.findall(harness.read_text(encoding="utf-8")):
            if key.startswith("A33-H"):
                ids[key] = "a33-tests-and-parity-harness"
    return ids


def present(root: Path, path: object) -> bool:
    if not isinstance(path, str) or not path or Path(path).is_absolute():
        return False
    target = (root / path).resolve()
    return target.is_relative_to(root.resolve()) and target.is_file()


def audit(root: Path, areas: set[str] | None = None, final: bool = False) -> dict:
    known = inventory(root)
    selected = {key: area for key, area in known.items()
                if not areas or area.split("-", 1)[0] in areas}
    rows, differences, errors, claimed = {}, {}, [], set()

    def require_text(entry, field, where):
        if not isinstance(entry.get(field), str) or not entry[field].strip():
            errors.append(f"{where}: needs {field}")

    for path in sorted((root / "docs/port/audit").glob("*.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            errors.append(f"{path.name}: {error}")
            continue
        if not isinstance(document, dict):
            errors.append(f"{path.name}: expected an object")
            continue
        # Performance measurements in this folder are independent of the feature inventory.
        if path.name.startswith("gain-") and "features" not in document:
            continue
        for field in ("area", "package"):
            require_text(document, field, path.name)
        records = document.get("differences", [])
        if not isinstance(records, list):
            errors.append(f"{path.name}: differences must be an array")
            records = []
        for item in records:
            if not isinstance(item, dict) or not isinstance(item.get("id"), str):
                errors.append(f"{path.name}: difference needs an id")
                continue
            key = item["id"]
            if key in differences:
                errors.append(f"{key}: duplicate difference")
            for field in ("id", "openwhisper", "openwhisper-cpp", "reason", "source"):
                require_text(item, field, key)
            refs = item.get("features")
            if not isinstance(refs, list) or any(not isinstance(r, str) or r not in known for r in refs):
                errors.append(f"{key}: invalid feature references")
            differences[key] = dict(item, area=document.get("area", ""))
        for field in ("features", "checklist"):
            entries = document.get(field, {})
            if not isinstance(entries, dict):
                errors.append(f"{path.name}: {field} must be an object")
                continue
            for key, entry in entries.items():
                if key not in known:
                    errors.append(f"{key}: unknown inventory ID")
                    continue
                if key not in selected:
                    continue
                if key in claimed:
                    errors.append(f"{key}: duplicate audit entry")
                claimed.add(key)
                if not isinstance(entry, dict) or entry.get("status") not in STATES:
                    errors.append(f"{key}: invalid status")
                    continue
                state = entry["status"]
                rows[key] = entry
                if state in {"verified", "fixed"}:
                    for kind in ("cpp", "tests"):
                        paths = entry.get(kind)
                        if not isinstance(paths, list) or not paths or any(not present(root, p) for p in paths):
                            errors.append(f"{key}: missing {kind} evidence")
                    if state == "fixed":
                        require_text(entry, "note", key)
                elif state == "difference":
                    require_text(entry, "difference", key)
                elif state == "handoff":
                    require_text(entry, "to", key)
                    require_text(entry, "note", key)
                    if final:
                        errors.append(f"{key}: handoff remains")
                else:
                    require_text(entry, "reason", key)
    for key, row in rows.items():
        if row["status"] == "difference":
            ref = differences.get(row.get("difference"))
            if not ref or key not in (ref.get("features") or []):
                errors.append(f"{key}: missing matching difference")
    missing = sorted(selected.keys() - claimed)
    summary = {}
    for key, area in selected.items():
        summary.setdefault(area, Counter())[rows.get(key, {}).get("status", "unaudited")] += 1
    return {"areas": dict(sorted(summary.items())), "missing": missing, "errors": errors,
            "differences": [differences[k] for k in sorted(differences)]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--areas", help="Comma-separated area codes, such as a05,a14")
    parser.add_argument("--final", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    areas = set(args.areas.split(",")) if args.areas else None
    result = audit(args.root, areas, args.final)
    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=True))
    else:
        for area, counts in result["areas"].items():
            print(area, ", ".join(f"{n} {s}" for s, n in sorted(counts.items())))
        for error in result["errors"]:
            print(error, file=sys.stderr)
        if result["missing"]:
            print(f"{len(result['missing'])} inventory IDs need audit evidence; "
                  f"first: {', '.join(result['missing'][:10])}", file=sys.stderr)
    return int(bool(result["errors"] or result["missing"]))


if __name__ == "__main__":
    sys.exit(main())
