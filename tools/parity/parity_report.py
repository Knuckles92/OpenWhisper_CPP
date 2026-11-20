"""Aggregate every tests/*/PARITY.json against OpenWhisper's test files into a parity report.

    python -I -B tools/parity/parity_report.py [--out docs/port/parity-report.md]
    python -I -B tools/parity/parity_report.py --check      (validate only, write nothing)

PARITY.json (architecture 9.1; schema binding for every package):

    {"package": "M0.6",
     "files": {"tests/test_settings.py": {
        "test_x": {"status": "ported", "cpp": "tests/settings/test_store.cpp"},
        "test_y": {"status": "golden", "vectors": "tests/golden/services.setting_types.json"},
        "test_z": {"status": "dropped", "reason": "..."},
        "Class::test_w": {"status": "pending", "owner": "M1.11", "reason": "..."}}}}

The same function may appear in more than one PARITY.json (pending in an early package's file,
ported in the package that finishes it): the report keeps the most finished status, ported or
golden over dropped over pending, so a function is pending only when every entry for it is.
Every OpenWhisper test file in classification.json is listed; a file no PARITY.json names is
"unassigned". Exit status 1 when a PARITY.json breaks the schema.
"""
from __future__ import annotations

import argparse
import collections
import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
import feature_audit
import tolerance_report
STATUSES = ("ported", "golden", "dropped", "other-platform", "pending")
RANK = {"ported": 3, "golden": 3, "dropped": 2, "other-platform": 2, "pending": 1}
REQUIRED = {"ported": ("cpp",), "golden": ("vectors",), "dropped": ("reason",),
            "other-platform": ("reason",), "pending": ("owner", "reason")}


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key {key!r}")
        result[key] = value
    return result


def load_parity_files(repo: Path) -> tuple[list[tuple[Path, dict]], list[str]]:
    found, errors = [], []
    for path in sorted((repo / "tests").glob("*/PARITY.json")):
        relative = path.relative_to(repo).as_posix()
        try:
            document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)
        except (OSError, ValueError) as exc:
            errors.append(f"{relative}: not readable JSON ({exc})")
            continue
        found.append((path, document))
    return found, errors


def validate(repo: Path, relative: str, document: dict, known_files: set[str]) -> list[str]:
    errors = []
    if not isinstance(document, dict):
        return [f"{relative}: the top level must be an object"]
    if not isinstance(document.get("package"), str) or not document["package"]:
        errors.append(f"{relative}: \"package\" must name the work package")
    files = document.get("files")
    if not isinstance(files, dict):
        return errors + [f"{relative}: \"files\" must be an object"]
    for test_file, functions in files.items():
        where = f"{relative}: {test_file}"
        if not test_file.startswith("tests/") or Path(test_file).name not in known_files:
            errors.append(f"{where}: not an OpenWhisper test file in classification.json")
        if not isinstance(functions, dict):
            errors.append(f"{where}: must map test functions to entries")
            continue
        for name, entry in functions.items():
            at = f"{where}::{name}"
            if not name.split("::")[-1].startswith("test"):
                errors.append(f"{at}: not a test function name")
            if not isinstance(entry, dict) or entry.get("status") not in STATUSES:
                errors.append(f"{at}: status must be one of {', '.join(STATUSES)}")
                continue
            for key in REQUIRED[entry["status"]]:
                if not isinstance(entry.get(key), str) or not entry[key].strip():
                    errors.append(f"{at}: a {entry['status']} entry needs \"{key}\"")
            for key in ("cpp", "vectors"):
                if key in entry and isinstance(entry[key], str) \
                        and not (repo / entry[key]).is_file():
                    errors.append(f"{at}: {key} {entry[key]} does not exist")
    return errors


def merge(documents: list[tuple[str, dict]]) -> dict[str, dict[str, dict]]:
    """{test file name: {function: best entry, with "packages": [...]}}"""
    merged: dict[str, dict[str, dict]] = collections.defaultdict(dict)
    for _, document in documents:
        package = document.get("package", "?")
        for test_file, functions in document.get("files", {}).items():
            if not isinstance(functions, dict):
                continue
            name = Path(test_file).name
            for function, entry in functions.items():
                if not isinstance(entry, dict) or entry.get("status") not in RANK:
                    continue
                current = merged[name].get(function)
                candidate = dict(entry, packages=[package])
                if current is None:
                    merged[name][function] = candidate
                elif RANK[entry["status"]] > RANK[current["status"]]:
                    candidate["packages"] = current["packages"] + [package]
                    merged[name][function] = candidate
                else:
                    current["packages"].append(package)
    return merged


def render(rows: list[dict], merged: dict[str, dict[str, dict]], packages: list[str]) -> str:
    by_area = collections.defaultdict(list)
    for row in rows:
        by_area[row["area"]].append(row)

    def counts(row):
        functions = merged.get(row["file"], {})
        c = collections.Counter(e["status"] for e in functions.values())
        c["unlisted"] = max(0, row["tests"] - len(functions))
        return c, functions

    lines = ["# Parity report", "",
             "Generated by `tools/parity/parity_report.py` from every `tests/*/PARITY.json` and "
             "`tools/parity/classification.json`. Do not edit by hand.", "",
             f"PARITY.json files: {len(packages)}"
             + (f" ({', '.join(packages)})" if packages else ""), ""]
    total = collections.Counter()
    file_states = collections.Counter()
    for row in rows:
        c, functions = counts(row)
        total.update({k: v for k, v in c.items()})
        total["tests"] += row["tests"]
        file_states["assigned" if functions else "unassigned"] += 1
    lines += ["| Test files | Assigned | Unassigned | Test functions | Ported | Golden | Dropped "
              "| Other platform | Pending | Not listed |", "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
              f"| {len(rows)} | {file_states['assigned']} | {file_states['unassigned']} "
              f"| {total['tests']} | {total['ported']} | {total['golden']} | {total['dropped']} "
              f"| {total['other-platform']} | {total['pending']} | {total['unlisted']} |", ""]

    excluded = total["dropped"] + total["other-platform"]
    kept = total["tests"] - excluded
    mapped = total["ported"] + total["golden"]
    if kept:
        lines += [f"Mapped kept test functions: {mapped:,} of {kept:,} ({mapped / kept:.1%}). "
                  f"Other-platform functions: {total['other-platform']}.", ""]
    lines += ["## By area", "", "| Area | Files | Assigned | Functions | Ported | Golden | Dropped "
              "| Other platform | Pending | Not listed |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for area in sorted(by_area):
        area_rows = by_area[area]
        c = collections.Counter()
        assigned = 0
        for row in area_rows:
            row_counts, functions = counts(row)
            c.update(row_counts)
            assigned += bool(functions)
        lines.append(f"| {area} | {len(area_rows)} | {assigned} "
                     f"| {sum(r['tests'] for r in area_rows)} | {c['ported']} | {c['golden']} "
                     f"| {c['dropped']} | {c['other-platform']} | {c['pending']} | {c['unlisted']} |")
    lines.append("")

    lines += ["## By test file", ""]
    for area in sorted(by_area):
        lines += [f"### {area}", "", "| Test file | Kind | Functions | State | Ported | Golden "
                  "| Dropped | Other platform | Pending | Not listed | Packages |",
                  "|---|---|---:|---|---:|---:|---:|---:|---:|---:|---|"]
        for row in sorted(by_area[area], key=lambda r: r["file"]):
            c, functions = counts(row)
            if not functions:
                state = "unassigned"
            elif c["pending"] or c["unlisted"]:
                state = "in progress"
            else:
                state = "done"
            owners = sorted({p for e in functions.values() for p in e["packages"]})
            lines.append(f"| {row['file']} | {row['kind']} | {row['tests']} | {state} "
                         f"| {c['ported']} | {c['golden']} | {c['dropped']} | {c['other-platform']} "
                         f"| {c['pending']} | {c['unlisted']} | {', '.join(owners)} |")
        lines.append("")

    pending = [(name, function, entry) for name, functions in sorted(merged.items())
               for function, entry in sorted(functions.items()) if entry["status"] == "pending"]
    lines += ["## Pending functions", ""]
    if pending:
        lines += ["| Function | Owner | Reason |", "|---|---|---|"]
        lines += [f"| {name}::{function} | {entry['owner']} | {entry['reason']} |"
                  for name, function, entry in pending]
    else:
        lines.append("None.")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=REPO)
    parser.add_argument("--classification", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", action="store_true", help="validate only")
    parser.add_argument("--strict", action="store_true", help="require all kept tests and feature audits")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    classification = args.classification or root / "tools" / "parity" / "classification.json"
    rows = json.loads(classification.read_text(encoding="utf-8"))
    known = {row["file"] for row in rows}

    found, errors = load_parity_files(root)
    documents = []
    for path, document in found:
        relative = path.relative_to(root).as_posix()
        errors += validate(root, relative, document, known)
        documents.append((relative, document))
    result = feature_audit.audit(root, final=args.strict)
    errors.extend(result["errors"])
    merged = merge(documents)
    if args.strict:
        for row in rows:
            entries = merged.get(row["file"], {})
            if not entries:
                errors.append(f"{row['file']}: unassigned")
            elif len(entries) != row["tests"] or any(e["status"] == "pending" for e in entries.values()):
                errors.append(f"{row['file']}: incomplete test mapping")
        if result["missing"]:
            errors.append(f"{len(result['missing'])} feature audits missing, including {result['missing'][0]}")
    for error in errors:
        print(f"parity: {error}", file=sys.stderr)
    if args.check:
        print(f"parity_report: {len(documents)} PARITY.json files, {len(errors)} problems")
        return 1 if errors else 0

    packages = [d.get("package", "?") for _, d in documents]
    report = render(rows, merged, packages)
    report += "\n## Features by area\n\n| Area | Inventory | Audited | Handoffs |\n|---|---:|---:|---:|\n"
    for area, counts in result["areas"].items():
        total = sum(counts.values())
        report += f"| {area} | {total} | {total - counts['unaudited']} | {counts['handoff']} |\n"
    report += "\n" + tolerance_report.render(root)
    out = args.out or root / "docs" / "port" / "parity-report.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(report.encode("utf-8"))
    print(f"parity_report: {len(rows)} test files, {len(documents)} PARITY.json files -> {out}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
