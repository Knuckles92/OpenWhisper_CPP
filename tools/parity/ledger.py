"""Add or replace entries in a tests/<dir>/PARITY.json without reformatting the rest of it.

    python -I -B tools/parity/ledger.py <tests/dir/PARITY.json> <tests/test_x.py> <status>
              [--cpp F] [--reason R] [--note N] [--owner O]
              (--all-unlisted | FUNCTION [FUNCTION ...])

The OpenWhisper checkout is OPENWHISPER_CPP_OW_ROOT (read only; function names are checked against
its test file).

status: ported (needs --cpp), dropped / other-platform (need --reason), pending (needs --owner
and --reason). --all-unlisted takes every function of the upstream file that no PARITY.json
lists yet. An entry already in this file for the same function is replaced. The edit is textual
(only the touched lines change) and is refused when the result is not valid ledger JSON.
Read the file again before any hand edit afterwards.
"""
from __future__ import annotations

import argparse
import ast
import json
import os
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
OW = Path(os.environ.get("OPENWHISPER_CPP_OW_ROOT", ""))
sys.path.insert(0, str(TOOLS))
import parity_report as pr  # noqa: E402


def upstream_functions(test_file: str) -> list[str]:
    tree = ast.parse((OW / test_file).read_text(encoding="utf-8"))
    out = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name.startswith("test"):
            out.append(node.name)
        elif isinstance(node, ast.ClassDef) and node.name.startswith("Test"):
            out += [f"{node.name}::{sub.name}" for sub in node.body
                    if isinstance(sub, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and sub.name.startswith("test")]
    return out


def matching_brace(text: str, start: int) -> int:
    """Index of the brace closing the object that opens at text[start] == '{'."""
    depth, i, in_string = 0, start, False
    while i < len(text):
        c = text[i]
        if in_string:
            if c == "\\":
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced braces")


def find_key(text: str, key: str, lo: int, hi: int) -> int:
    """Index of the '{' that is the value of "key" at the top level of text[lo:hi], or -1."""
    needle = json.dumps(key)
    depth, i, in_string = 0, lo, False
    while i < hi:
        c = text[i]
        if in_string:
            if c == "\\":
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            if depth == 1 and text.startswith(needle, i):
                j = i + len(needle)
                while text[j] in " \t\r\n":
                    j += 1
                if text[j] == ":":
                    j += 1
                    while text[j] in " \t\r\n":
                        j += 1
                    if text[j] == "{":
                        return j
            in_string = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return -1


def line_indent(text: str, index: int) -> str:
    start = text.rfind("\n", 0, index) + 1
    end = start
    while text[end] in " \t":
        end += 1
    return text[start:end]


def insert_member(text: str, open_brace: int, member: str, child_indent: str) -> str:
    """Append `member` (no trailing comma) as the last member of the object at open_brace."""
    close = matching_brace(text, open_brace)
    body = text[open_brace + 1:close]
    close_indent = line_indent(text, close) if "\n" in body or body.strip() == "" else ""
    if body.strip():
        end = open_brace + 1 + len(body.rstrip())
        return text[:end] + ",\n" + child_indent + member + "\n" + close_indent + text[close:]
    return text[:open_brace + 1] + "\n" + child_indent + member + "\n" + line_indent(text, open_brace) + text[close:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("parity")
    ap.add_argument("test_file")
    ap.add_argument("status", choices=pr.STATUSES)
    ap.add_argument("--cpp")
    ap.add_argument("--reason")
    ap.add_argument("--note")
    ap.add_argument("--owner")
    ap.add_argument("--all-unlisted", action="store_true")
    ap.add_argument("functions", nargs="*")
    args = ap.parse_args()

    path = (REPO / args.parity).resolve()
    if not os.environ.get("OPENWHISPER_CPP_OW_ROOT") or not (OW / "tests").is_dir():
        print("ledger: set OPENWHISPER_CPP_OW_ROOT to the OpenWhisper checkout")
        return 2
    if not (OW / args.test_file).is_file():
        print(f"ledger: {args.test_file} is not an OpenWhisper test file")
        return 2
    known = upstream_functions(args.test_file)
    if args.all_unlisted:
        found, _ = pr.load_parity_files(REPO)
        merged = pr.merge([(p.relative_to(REPO).as_posix(), d) for p, d in found])
        listed = set(merged.get(Path(args.test_file).name, {}))
        functions = [f for f in known if f not in listed]
    else:
        functions = args.functions
        unknown = [f for f in functions if f not in known]
        if unknown:
            print(f"ledger: not in {args.test_file}: {', '.join(unknown)}")
            print("        (names are test_name or Class::test_name exactly as in the Python file)")
            return 2
    if not functions:
        print("ledger: nothing to add")
        return 0

    entry: dict[str, str] = {"status": args.status}
    for key in ("cpp", "owner", "reason", "note"):
        value = getattr(args, key)
        if value:
            entry[key] = value
    for key in pr.REQUIRED[args.status]:
        if key not in entry:
            print(f"ledger: a {args.status} entry needs --{key}")
            return 2
    if "cpp" in entry and not (REPO / entry["cpp"]).is_file():
        print(f"ledger: {entry['cpp']} does not exist")
        return 2
    value = json.dumps(entry, ensure_ascii=False)

    text = path.read_text(encoding="utf-8")
    files_at = find_key(text, "files", 0, len(text))
    if files_at < 0:
        print("ledger: no \"files\" object")
        return 2
    step = line_indent(text, files_at)                       # indent of the "files" line
    file_indent = step * 2 if step else "  "
    block_at = find_key(text, args.test_file, files_at, matching_brace(text, files_at) + 1)
    if block_at < 0:
        text = insert_member(text, files_at, json.dumps(args.test_file) + ": {\n" + file_indent + "}", file_indent)
        files_at = find_key(text, "files", 0, len(text))
        block_at = find_key(text, args.test_file, files_at, matching_brace(text, files_at) + 1)
    function_indent = line_indent(text, block_at) + (step or " ")
    replaced = added = 0
    for function in functions:
        block_end = matching_brace(text, block_at)
        at = find_key(text, function, block_at, block_end + 1)
        if at >= 0:
            text = text[:at] + value + text[matching_brace(text, at) + 1:]
            replaced += 1
        else:
            text = insert_member(text, block_at, json.dumps(function) + ": " + value, function_indent)
            added += 1

    try:
        document = json.loads(text, object_pairs_hook=pr.unique_keys)
    except ValueError as exc:
        print(f"ledger: refused, the result would not be valid JSON ({exc})")
        return 1
    rows = json.loads((REPO / "tools/parity/classification.json").read_text(encoding="utf-8"))
    errors = pr.validate(REPO, args.parity, document, {row["file"] for row in rows})
    if errors:
        print("ledger: refused:")
        for error in errors[:10]:
            print("  " + error)
        return 1
    path.write_text(text, encoding="utf-8")
    print(f"ledger: {args.parity}: {added} added, {replaced} replaced as {args.status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
