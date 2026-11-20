"""Rebrand the vendored dashboard bundle (webui/) by exact text substitution.

OpenWhisper's webui source and its committed dist are copied into webui/ unchanged; this script
replaces the product name in them using rebrand_map.json, so no Node runs at build or run time.
A rule is an exact literal, its replacement and the number of times the literal occurs in its
file. A count that differs refuses the whole run before any file is written: a minified name
containing the product name could otherwise be broken silently.

    rebrand.py                    apply the map (a second run changes nothing)
    rebrand.py --check            no product name left, replacements present, dist.sha256 matches
    rebrand.py --manifest         write webui/dist.sha256 (after a Node rebuild and a new apply)
    rebrand.py --report           per-file replacement counts
    rebrand.py --against <dir>    the tree equals <dir> (OpenWhisper's webui) with the map applied
    rebrand.py --against-commit <git dir> [--commit <sha>]
                                  the same against webui/ as committed at the reference commit
                                  (tools/parity/reference.json), read with `git cat-file` only
    rebrand.py --self-test        undo the map on a temporary copy, apply it, compare

Run it as `python -I -B tools/webui/rebrand.py`. Exit status 0 means success, 1 a problem.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MAP_FILE = HERE / "rebrand_map.json"
REFERENCE_FILE = ROOT / "tools" / "parity" / "reference.json"

# Files of FioriVoce's own that sit in webui/ beside the vendored ones.
OURS = frozenset({"SOURCE", "README.md", "dist.sha256", ".gitattributes", ".gitignore"})
# Folders of OpenWhisper's webui that are not vendored. They are skipped on both sides, so the
# owner's `npm ci` in webui/ (node_modules/) changes no result.
NOT_VENDORED = frozenset({"node_modules", "mocks", "benchmarks"})
BRAND = re.compile(rb"open[ ._-]?whisper", re.IGNORECASE)


@dataclass(frozen=True)
class Rule:
    file: str
    literal: bytes
    replacement: bytes
    count: int


@dataclass(frozen=True)
class Allowed:
    file: str
    count: int


def load_map(path: Path = MAP_FILE) -> tuple[list[Rule], list[Allowed]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    rules = [Rule(r["file"], r["literal"].encode(), r["replacement"].encode(), r["count"])
             for r in data["replacements"]]
    allowed = [Allowed(a["file"], a["count"]) for a in data.get("allowed", [])]
    problems = []
    for rule in rules:
        if not rule.literal or rule.count < 1 or rule.literal == rule.replacement:
            problems.append(f"{rule.file}: empty literal, count below 1 or no change")
        if b"\n" in rule.literal:
            problems.append(f"{rule.file}: a literal spans lines (files mix LF and CRLF)")
        for other in rules:
            if other is not rule and other.file == rule.file and (
                    other.literal in rule.literal or other.literal in rule.replacement):
                problems.append(f"{rule.file}: rules overlap, their order would matter")
    if problems:
        raise ValueError("rebrand_map.json is inconsistent:\n  " + "\n  ".join(problems))
    return rules, allowed


def apply_rules(rules: list[Rule], files: dict[str, bytes]) -> tuple[dict[str, bytes], list[str], list[str]]:
    """Returns (new contents, a line per rule, problems). A rule whose literal is gone and whose
    replacement is present the expected number of times counts as already applied."""
    out = dict(files)
    lines, problems = [], []
    for rule in rules:
        if rule.file not in files:
            problems.append(f"{rule.file}: file is missing")
            continue
        # Counted on the text as the earlier rules left it, so a rule whose occurrence an earlier
        # replacement consumed is reported instead of replacing fewer than its count.
        data = out[rule.file]
        found = data.count(rule.literal)
        if found == rule.count:
            out[rule.file] = out[rule.file].replace(rule.literal, rule.replacement)
            lines.append(f"{rule.file}: replaced {found} x {rule.literal.decode()!r}")
        elif found == 0 and data.count(rule.replacement) >= rule.count:
            lines.append(f"{rule.file}: already replaced {rule.literal.decode()!r}")
        else:
            problems.append(f"{rule.file}: {rule.literal.decode()!r} occurs {found} times, "
                            f"expected {rule.count}")
    return out, lines, problems


def read_tree(webui: Path) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    if not webui.is_dir():
        return files
    for path in sorted(webui.iterdir()):
        if path.is_file():
            files[path.name] = path.read_bytes()
        elif path.is_dir() and path.name not in NOT_VENDORED:
            files.update({p.relative_to(webui).as_posix(): p.read_bytes()
                          for p in sorted(path.rglob("*")) if p.is_file()})
    return files


def vendored(files: dict[str, bytes]) -> dict[str, bytes]:
    return {name: data for name, data in files.items() if name not in OURS}


def apply(webui: Path, rules: list[Rule]) -> tuple[list[str], list[str]]:
    """Returns (the files written, problems). Nothing is written when there is a problem."""
    files = read_tree(webui)
    new, _, problems = apply_rules(rules, files)
    if problems:
        return [], problems
    changed = [name for name in new if new[name] != files[name]]
    for name in changed:
        (webui / name).write_bytes(new[name])
    return changed, []


def leftovers(files: dict[str, bytes], allowed: list[Allowed]) -> list[str]:
    allowance = {a.file: a.count for a in allowed}
    problems = []
    for name, data in vendored(files).items():
        found = len(BRAND.findall(data))
        if found != allowance.get(name, 0):
            problems.append(f"{name}: {found} occurrences of the old product name, "
                            f"{allowance.get(name, 0)} allowed")
    return problems


def manifest_text(files: dict[str, bytes]) -> str:
    return "".join(f"{hashlib.sha256(data).hexdigest()}  {name[5:]}\n"
                   for name, data in sorted(files.items()) if name.startswith("dist/"))


def check_manifest(webui: Path, files: dict[str, bytes]) -> list[str]:
    path = webui / "dist.sha256"
    if not path.is_file():
        return ["dist.sha256 is missing"]
    expected = manifest_text(files)
    actual = path.read_bytes().decode("utf-8", errors="replace")
    if expected == actual:
        return []

    def entries(text: str) -> tuple[dict[str, str], list[str]]:
        found, bad = {}, []
        for number, line in enumerate(text.splitlines(), 1):
            digest, sep, name = line.partition("  ")
            if not sep or not name or not re.fullmatch(r"[0-9a-f]{64}", digest):
                bad.append(f"dist.sha256 line {number} is not `<sha256>  <path>`")
            else:
                found[name] = digest
        return found, bad

    listed, problems = entries(actual)
    current, _ = entries(expected)
    problems += [f"dist/{name}: not in the manifest" for name in sorted(current.keys() - listed.keys())]
    problems += [f"dist/{name}: listed but not in dist" for name in sorted(listed.keys() - current.keys())]
    problems += [f"dist/{name}: hash differs" for name in sorted(listed.keys() & current.keys())
                 if listed[name] != current[name]]
    return problems or ["dist.sha256 is not in the sorted LF format"]


def check(webui: Path, rules: list[Rule], allowed: list[Allowed]) -> list[str]:
    files = read_tree(webui)
    problems = leftovers(files, allowed)
    for rule in rules:
        data = files.get(rule.file)
        if data is None or data.count(rule.replacement) < rule.count or rule.literal in data:
            problems.append(f"{rule.file}: {rule.replacement.decode()!r} is not in place "
                            f"{rule.count} times")
    return problems + check_manifest(webui, files)


def report(rules: list[Rule]) -> str:
    totals: dict[str, int] = {}
    for rule in rules:
        totals[rule.file] = totals.get(rule.file, 0) + rule.count
    rows = [f"{name}: {count}" for name, count in sorted(totals.items())]
    return "\n".join(rows + [f"{len(totals)} files, {sum(totals.values())} replacements"])


def compare(webui: Path, files: dict[str, bytes], rules: list[Rule]) -> list[str]:
    """`files` (OpenWhisper's webui, minus what is not vendored) with the map applied is the
    vendored part of `webui` byte for byte."""
    expected, _, problems = apply_rules(rules, files)
    ours = vendored(read_tree(webui))
    problems += [f"{name}: not in the original" for name in sorted(ours.keys() - expected.keys())]
    problems += [f"{name}: missing" for name in sorted(expected.keys() - ours.keys())]
    for name in sorted(ours.keys() & expected.keys()):
        if ours[name] == expected[name]:
            continue
        if ours[name].replace(b"\r\n", b"\n") == expected[name].replace(b"\r\n", b"\n"):
            problems.append(f"{name}: differs in line endings only")
        else:
            problems.append(f"{name}: differs beyond the map")
    return problems


def against(webui: Path, original: Path, rules: list[Rule]) -> list[str]:
    """Compares with a checkout's webui folder (read only). A working copy can differ from its
    commit in line endings (an editor or core.autocrlf), which --against-commit does not see."""
    if not original.is_dir():
        return [f"{original}: not a folder"]
    return compare(webui, read_tree(original), rules)


def reference_commit() -> str:
    return json.loads(REFERENCE_FILE.read_text(encoding="utf-8"))["ow_commit"]


def committed_webui(git_dir: Path, commit: str) -> dict[str, bytes]:
    """webui/ as committed in OpenWhisper's repository. Only `git ls-tree` and `git cat-file`
    run, neither of which writes to the repository or its index."""
    def git(*args: str, data: bytes | None = None) -> bytes:
        result = subprocess.run(["git", "--git-dir", str(git_dir), *args], input=data,
                                capture_output=True, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"git {args[0]}: {result.stderr.decode(errors='replace').strip()}")
        return result.stdout

    names, shas = [], []
    for entry in git("ls-tree", "-r", "-z", commit, "--", "webui/").split(b"\0"):
        if not entry:
            continue
        meta, path = entry.split(b"\t", 1)
        mode, kind, sha = meta.decode().split()
        name = path.decode("utf-8")[len("webui/"):]
        if kind != "blob" or name.split("/", 1)[0] in NOT_VENDORED:
            continue
        names.append(name)
        shas.append(sha)
    if not names:
        raise RuntimeError(f"commit {commit} has no webui/ folder")
    out = git("cat-file", "--batch", data="".join(f"{sha}\n" for sha in shas).encode())
    files, pos = {}, 0
    for name in names:
        header_end = out.index(b"\n", pos)
        size = int(out[pos:header_end].split()[2])
        files[name] = out[header_end + 1:header_end + 1 + size]
        pos = header_end + 1 + size + 1
    return files


def against_commit(webui: Path, git_dir: Path, commit: str, rules: list[Rule]) -> list[str]:
    try:
        files = committed_webui(git_dir, commit)
    except (RuntimeError, OSError, ValueError, IndexError) as error:
        return [str(error)]
    return compare(webui, files, rules)


def undo(rules: list[Rule], files: dict[str, bytes]) -> dict[str, bytes]:
    out = dict(files)
    for rule in rules:
        out[rule.file] = out[rule.file].replace(rule.replacement, rule.literal)
    return out


def materialize(files: dict[str, bytes], where: Path) -> Path:
    for name, data in files.items():
        target = where / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return where


def self_test(webui: Path, rules: list[Rule]) -> list[str]:
    """On temporary copies: undo the map, apply it, apply it again, and apply it with one count
    off. The first run must reproduce the shipped bytes; the others must change nothing."""
    shipped = read_tree(webui)
    names = {rule.file for rule in rules}
    problems = []
    with tempfile.TemporaryDirectory(prefix="fv-rebrand-") as tmp:
        copy = materialize(undo(rules, shipped), Path(tmp) / "undone")
        if not leftovers(read_tree(copy), []):
            problems.append("undoing the map left no old product name: the map matches nothing")
        changed, refused = apply(copy, rules)
        if refused or set(changed) != names:
            problems.append(f"first run: {len(changed)} files changed, refused: {refused}")
        if read_tree(copy) != shipped:
            problems.append("the map applied to the undone copy is not the shipped tree")
        changed, refused = apply(copy, rules)
        if changed or refused:
            problems.append(f"a second run changed {len(changed)} files, refused: {refused}")

        first = rules[0]
        skewed = undo(rules, shipped)
        skewed[first.file] += first.literal * (first.count + 1)
        copy = materialize(skewed, Path(tmp) / "skewed")
        changed, refused = apply(copy, rules)
        if changed or not refused or read_tree(copy) != skewed:
            problems.append("a count that differs did not stop the run before it wrote")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    parser.add_argument("--map", type=Path, default=MAP_FILE)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--manifest", action="store_true")
    mode.add_argument("--report", action="store_true")
    mode.add_argument("--against", type=Path, metavar="DIR")
    mode.add_argument("--against-commit", type=Path, metavar="GIT_DIR")
    mode.add_argument("--self-test", action="store_true")
    parser.add_argument("--commit", help="with --against-commit; default: reference.json's ow_commit")
    args = parser.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")  # literals hold typographic characters
    webui = args.root / "webui"
    try:
        rules, allowed = load_map(args.map)
    except (ValueError, OSError, KeyError) as error:
        print(error)
        return 1

    if args.check:
        problems = check(webui, rules, allowed)
        ok = "rebrand: no old product name left, every replacement in place, dist.sha256 matches"
    elif args.manifest:
        text = manifest_text(read_tree(webui))
        (webui / "dist.sha256").write_bytes(text.encode("utf-8"))
        print(f"rebrand: wrote dist.sha256, {text.count(chr(10))} files")
        return 0
    elif args.report:
        print(report(rules))
        return 0
    elif args.against:
        problems = against(webui, args.against, rules)
        ok = "rebrand: the tree is the original with exactly the map's replacements"
    elif args.against_commit:
        commit = args.commit or reference_commit()
        problems = against_commit(webui, args.against_commit, commit, rules)
        ok = f"rebrand: the tree is webui/ of {commit} with exactly the map's replacements"
    elif args.self_test:
        problems = self_test(webui, rules)
        ok = "rebrand: self-test passed"
    else:
        changed, problems = apply(webui, rules)
        ok = f"rebrand: {len(changed)} files changed"
    if problems:
        print("rebrand: failed")
        for problem in problems:
            print("  " + problem)
        return 1
    print(ok)
    return 0


if __name__ == "__main__":
    sys.exit(main())
