"""Write one audit entry into a docs/port/audit file, in one locked load-change-write.

    auditput.py <audit-file-name.json> <features|checklist> <ID> '<entry as JSON>'
    auditput.py <audit-file-name.json> difference '<difference record as JSON>'

Creates the file (area and package taken from --area/--package) when it does not exist. An
entry already there for the id is replaced. Refuses an id another audit file already holds.
"""
import argparse
import fcntl
import json
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
# One lock for every writer on this machine: workers sharing a checkout edit the same files.
LOCK = Path(tempfile.gettempdir()) / "fiorivoce-audit-files.lock"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("kind", choices=["features", "checklist", "difference"])
    ap.add_argument("rest", nargs="+")
    ap.add_argument("--area")
    ap.add_argument("--package", default="M10 sweep")
    args = ap.parse_args()
    directory = REPO / "docs/port/audit"
    path = directory / Path(args.file).name
    with open(LOCK, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if path.exists():
            document = json.loads(path.read_text(encoding="utf-8"))
        else:
            if not args.area:
                print("auditput: a new file needs --area")
                return 2
            document = {"area": args.area, "package": args.package, "features": {}, "differences": []}
        if args.kind == "difference":
            record = json.loads(args.rest[0])
            for field in ("id", "features", "openwhisper", "fiorivoce", "reason", "source"):
                if field not in record:
                    print(f"auditput: the record needs {field}")
                    return 2
            for other in directory.glob("*.json"):
                if other == path:
                    continue
                held = json.loads(other.read_text(encoding="utf-8")).get("differences", [])
                if any(isinstance(r, dict) and r.get("id") == record["id"] for r in held):
                    print(f"auditput: {other.name} already has a record named {record['id']} (use diffref.py)")
                    return 1
            records = document.setdefault("differences", [])
            records[:] = [r for r in records if r.get("id") != record["id"]] + [record]
        else:
            if len(args.rest) != 2:
                print("auditput: needs an ID and an entry")
                return 2
            key, entry = args.rest[0], json.loads(args.rest[1])
            for other in directory.glob("*.json"):
                if other == path:
                    continue
                held = json.loads(other.read_text(encoding="utf-8"))
                if isinstance(held, dict) and any(key in held.get(f, {}) for f in ("features", "checklist")):
                    print(f"auditput: {key} is already recorded in {other.name}")
                    return 1
            for kind in ("cpp", "tests"):
                for item in entry.get(kind, []):
                    if not (REPO / item).is_file():
                        print(f"auditput: {item} does not exist")
                        return 1
            document.setdefault(args.kind, {})[key] = entry
        path.write_text(json.dumps(document, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"auditput: {path.name}: wrote {args.rest[0][:60]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
