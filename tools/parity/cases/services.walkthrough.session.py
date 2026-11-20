"""Curated scripts for services/walkthrough/session.py (fv_walkthrough's session.h).

    _storyboard(ops, probes)

runs a WalkthroughSession started at 2026-10-21 14:32:05 in a scratch folder under out/parity
through `ops`, then reads its storyboard. Each op is a list:

    ["add", kept_at_s]          add_shot with an image that saves; the shot's number or None
    ["add_failing", kept_at_s]  add_shot with an image whose save writes half a file and fails
    ["remove", number]          remove_shot; True or False
    ["undo"]                    undo_last; the number or None
    ["move", number, step]      move_shot; True or False
    ["hold", at_s] / ["release", at_s] / ["close"]       None
    ["checkpoint", journal_id]  checkpoint_recording; None
    ["count"]                   shot_count

The result is {"ops": [each op's result], "shots": [[number, file name, kept_at_s]], "cuts_s",
"sections", "notes_s", "section_at": [section_at(probe) for each probe], "shot_count",
"lost_shots", "files": the folder's file names, sorted, "draft": draft.json's text or None}.
The writer is drained before the files are listed and the draft is read.
"""
from __future__ import annotations

import shutil
import tempfile
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_session"
STARTED = datetime(2026, 10, 21, 14, 32, 5)


class _Image:
    def __init__(self, works: bool):
        self.works = works

    def copy(self):
        return self

    def save(self, path, fmt):
        with open(path, "wb") as out:
            out.write(b"\x89PNG fake" if self.works else b"\x89PNG")
        return self.works


def _storyboard(module, ops, probes):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(dir=SCRATCH))
    folder = root / "capture"

    def make_folder(started_at):
        folder.mkdir(exist_ok=True)
        return folder

    session = module.WalkthroughSession(started_at=STARTED, make_folder=make_folder)
    try:
        results = []
        for op in ops:
            name = op[0]
            if name == "add":
                shot = session.add_shot(_Image(True), op[1])
                results.append(None if shot is None else shot.number)
            elif name == "add_failing":
                shot = session.add_shot(_Image(False), op[1])
                results.append(None if shot is None else shot.number)
            elif name == "remove":
                results.append(session.remove_shot(op[1]))
            elif name == "undo":
                results.append(session.undo_last())
            elif name == "move":
                results.append(session.move_shot(op[1], op[2]))
            elif name == "hold":
                results.append(session.hold_note(op[1]))
            elif name == "release":
                results.append(session.release_note(op[1]))
            elif name == "close":
                results.append(session.close())
            elif name == "checkpoint":
                results.append(session.checkpoint_recording(op[1]))
            elif name == "count":
                results.append(session.shot_count)
            else:
                raise ValueError(name)
        board = session.storyboard()
        if session._writer is not None:
            session._writer.shutdown(wait=True)
        draft = folder / "draft.json"
        return {
            "ops": results,
            "shots": [[shot.number, shot.path.name, shot.kept_at_s] for shot in board.shots],
            "cuts_s": list(board.cuts_s),
            "sections": [list(section) for section in board.sections],
            "notes_s": [list(note) for note in board.notes_s],
            "section_at": [board.section_at(probe) for probe in probes],
            "shot_count": session.shot_count,
            "lost_shots": session.lost_shots,
            "files": sorted(p.name for p in folder.iterdir()) if folder.exists() else [],
            "draft": draft.read_text(encoding="utf-8") if draft.exists() else None,
        }
    finally:
        session.end()
        if session._writer is not None:
            session._writer.shutdown(wait=True)
        shutil.rmtree(root)


WRAPPERS = {"_storyboard": _storyboard}


def _three():
    return [["add", 2.0], ["add", 4.0], ["add", 6.0]]


CASES = {
    "_storyboard": [
        # The vectors OpenWhisper's own tests pin.
        [_three(), [1.0, 3.0, 5.0, 7.0]],
        [_three() + [["move", 3, -1], ["move", 3, -1]], [1.0, 3.0, 5.0, 7.0]],
        [_three() + [["move", 3, 1], ["move", 1, -1], ["move", 9, 1]], [1.0]],
        [_three() + [["remove", 2], ["count"], ["remove", 2]], [1.0, 3.0, 5.0, 7.0]],
        [_three() + [["move", 1, 1], ["remove", 2]], [1.0, 3.0, 5.0, 7.0]],
        [_three() + [["move", 3, -1], ["undo"], ["undo"]], [1.0, 3.0, 5.0, 7.0]],
        [_three() + [["undo"], ["undo"]], [1.0, 3.0]],
        [_three() + [["undo"], ["add", 8.0]], [1.0, 3.0, 5.0, 9.0]],
        [_three() + [["close"], ["remove", 1], ["move", 2, -1], ["undo"], ["count"]], [1.0]],
        [_three() + [["checkpoint", "0" * 32], ["remove", 2]], [1.0]],
        [_three() + [["move", 2, -1]], [1.0, 3.0, 7.0]],
        [_three() + [["hold", 2.5], ["release", 3.5], ["release", 3.9], ["hold", 7.0], ["close"],
                     ["hold", 8.0]], [2.5, 7.0]],
        # Twenty more.
        [[], [0.0, 100.0]],
        [[["hold", 1.0], ["release", 2.0]], [1.5]],
        [[["checkpoint", ""], ["hold", 1.0], ["release", 2.0], ["hold", 3.0]], [0.0]],
        [[["checkpoint", "ab" * 16], ["hold", -4.0], ["release", 0.0], ["hold", 2.0],
          ["release", 2.0], ["hold", 5.0], ["release", 4.0]], [0.0]],
        [[["add", -3.0], ["add", 0.0], ["add", 1.5]], [0.0, 1.0, 2.0]],
        [[["add", 1.0], ["add", 1.0], ["add", 1.0]], [0.5, 1.0, 1.5]],
        [_three() + [["add", 8.0], ["move", 1, 1], ["move", 1, 1], ["move", 1, 1], ["move", 1, 1]],
         [1.0, 3.0, 5.0, 7.0, 9.0]],
        [_three() + [["add", 8.0], ["move", 4, -1], ["move", 4, -1], ["move", 4, -1],
                     ["move", 4, -1]], [1.0, 3.0, 5.0, 7.0, 9.0]],
        [_three() + [["add", 8.0], ["move", 2, 2], ["move", 1, -2], ["move", 4, -3]],
         [1.0, 3.0, 5.0, 7.0, 9.0]],
        [_three() + [["add", 8.0], ["move", 1, 1], ["move", 3, -1], ["remove", 2], ["remove", 3]],
         [1.0, 3.0, 5.0, 7.0, 9.0]],
        [_three() + [["add", 8.0], ["remove", 1], ["remove", 3], ["undo"], ["undo"], ["undo"]],
         [1.0, 9.0]],
        [_three() + [["remove", 3], ["add", 8.0], ["move", 4, -1], ["undo"]], [1.0, 5.0, 7.0]],
        [_three() + [["move", 2, 1], ["move", 1, 2], ["add", 8.0], ["add", 10.0], ["move", 5, -4]],
         [1.0, 3.0, 5.0, 7.0, 9.0, 11.0]],
        [[["add", 1.0], ["add_failing", 2.0], ["add", 3.0]], [0.5, 1.5, 2.5, 3.5]],
        [[["add_failing", 1.0], ["add_failing", 2.0]], [0.5, 1.5, 2.5]],
        [[["add", 1.0], ["add_failing", 2.0], ["add", 3.0], ["move", 3, -1], ["remove", 1]],
         [0.5, 1.5, 2.5, 3.5]],
        [[["checkpoint", "f" * 32], ["add", 2.0], ["hold", 2.5], ["add", 3.0], ["release", 3.5],
          ["add", 4.0], ["remove", 2], ["hold", 5.0], ["close"]], [2.0, 2.75, 3.25, 4.5]],
        [[["add", 2.0], ["close"], ["add", 3.0], ["checkpoint", "c" * 32]], [1.0, 2.5]],
        [_three() + [["remove", 1], ["remove", 2], ["remove", 3], ["add", 8.0], ["undo"],
                     ["count"]], [0.0, 3.0, 9.0]],
        [_three() + [["move", 1, 1], ["move", 1, 1], ["remove", 2], ["remove", 3]], [1.0, 5.0, 7.0]],
    ],
}
