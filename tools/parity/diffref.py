"""Attach inventory ids to a difference record that already exists in some docs/port/audit file.

    diffref.py <difference-record-id> <ID> [<ID> ...]

Finds the audit file holding the record and appends the ids to its "features" list (ids already
there are left alone). One locked load-change-write, so concurrent workers do not lose each
other's edits. Run from anywhere.
"""
import fcntl
import json
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
# One lock for every writer on this machine: workers sharing a checkout edit the same files.
LOCK = Path(tempfile.gettempdir()) / "openwhisper-cpp-audit-files.lock"


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    record_id, ids = sys.argv[1], sys.argv[2:]
    with open(LOCK, "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for path in sorted((REPO / "docs/port/audit").glob("*.json")):
            document = json.loads(path.read_text(encoding="utf-8"))
            for record in document.get("differences", []) if isinstance(document, dict) else []:
                if isinstance(record, dict) and record.get("id") == record_id:
                    added = [i for i in ids if i not in record["features"]]
                    record["features"] += added
                    path.write_text(json.dumps(document, indent=1, ensure_ascii=False) + "\n",
                                    encoding="utf-8")
                    print(f"diffref: {path.name}: {record_id} now also covers {', '.join(added) or '(nothing new)'}")
                    return 0
    print(f"diffref: no difference record named {record_id}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
