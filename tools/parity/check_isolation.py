"""Check that a harness run wrote nothing into the OpenWhisper checkout or the owner's
%LOCALAPPDATA%\\OpenWhisper (the a33 map's section 3 check, scripted).

    python -I -B tools/parity/check_isolation.py -- <command> [args...]
    python -I -B tools/parity/check_isolation.py --since 2026-11-14T12:00:00

The first form notes the time, runs the command, then lists every file and folder under the two
watched roots whose write or creation time is newer; the second checks against a given time
(ISO 8601, local time unless it has an offset, or Unix seconds). Only names and times are read,
never contents. Exit status: 0 when nothing changed and the command (if any) succeeded, 1 when
something under a watched root changed, otherwise the command's own status.

The watched roots are FIORIVOCE_OW_ROOT (or --root) and %LOCALAPPDATA%\\OpenWhisper, taken from
this process's environment, so run it from a shell whose LOCALAPPDATA is the real one. A change
the owner made by using OpenWhisper during the run is reported like any other: judge it by its
name and time.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import stat
import subprocess
import sys
import time
from pathlib import Path


def watched_roots(root_arg: str | None) -> list[Path]:
    roots = []
    root = root_arg or os.environ.get("FIORIVOCE_OW_ROOT")
    if root:
        roots.append(Path(root))
    # ow_isolation moves LOCALAPPDATA into scratch and keeps the real one here, so a check run
    # from an isolated process still watches the owner's folder.
    local = os.environ.get("FIORIVOCE_REAL_LOCALAPPDATA") or os.environ.get("LOCALAPPDATA")
    if local:
        roots.append(Path(local) / "OpenWhisper")
    return [r for r in roots if r.exists()]


def entry_times(st: os.stat_result) -> tuple[float, float]:
    created = getattr(st, "st_birthtime", None)
    if created is None:
        created = st.st_ctime if sys.platform == "win32" else st.st_mtime
    return st.st_mtime, created


def newer_than(roots: list[Path], since: float) -> list[tuple[float, Path]]:
    found: list[tuple[float, Path]] = []
    stack = list(roots)
    for root in roots:
        modified, created = entry_times(root.lstat())
        if max(modified, created) > since:
            found.append((max(modified, created), root))
    while stack:
        folder = stack.pop()
        try:
            entries = list(os.scandir(folder))
        except OSError as exc:
            print(f"cannot list {folder}: {exc.strerror}", file=sys.stderr)
            continue
        for entry in entries:
            try:
                st = entry.stat(follow_symlinks=False)
            except OSError:
                continue
            newest = max(entry_times(st))
            if newest > since:
                found.append((newest, Path(entry.path)))
            if stat.S_ISDIR(st.st_mode) and not entry.is_symlink():
                stack.append(Path(entry.path))
    return sorted(found)


def parse_since(text: str) -> float:
    try:
        return float(text)
    except ValueError:
        return dt.datetime.fromisoformat(text).timestamp()


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    command: list[str] = []
    if "--" in argv:
        split = argv.index("--")
        argv, command = argv[:split], argv[split + 1:]
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--since", help="ISO 8601 time or Unix seconds")
    parser.add_argument("--root", help="the OpenWhisper checkout (default FIORIVOCE_OW_ROOT)")
    args = parser.parse_args(argv)
    if bool(command) == bool(args.since):
        parser.error("give either --since or -- <command>")

    roots = watched_roots(args.root)
    if not roots:
        parser.error("no watched root exists: set FIORIVOCE_OW_ROOT or pass --root")
    status = 0
    if command:
        since = time.time()
        status = subprocess.run(command, check=False).returncode
    else:
        since = parse_since(args.since)

    changed = newer_than(roots, since)
    when = dt.datetime.fromtimestamp(since).isoformat(timespec="seconds")
    if changed:
        print(f"check_isolation: {len(changed)} entries changed since {when}:")
        for moment, path in changed:
            stamp = dt.datetime.fromtimestamp(moment).isoformat(timespec="seconds")
            print(f"  {stamp}  {path}")
        return 1
    print(f"check_isolation: nothing under {', '.join(str(r) for r in roots)} changed "
          f"since {when}")
    return status


if __name__ == "__main__":
    sys.exit(main())
