"""Curated inputs for services/recording_journal.py (fv_recorder's journal.h).

The functions touch the disk, so each wrapper works in a scratch folder under out/parity and
returns names and text only:

- recovery_directory: the recovery folder's name for an output file name.
- session_json: the session.json a RecordingJournal writes when datetime.now() is `created`
  (the module's datetime is swapped for one whose now() returns it; the journal is then
  discarded).
- _recovered_wav_path: the recovered WAV's file name for a session created at `created` in a
  session folder named `session`.
- _session_format: session.json's text to (rate, channels, sample_width), or the error.
"""
from __future__ import annotations

import datetime as _datetime
import shutil
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "recording_journal"


def _scratch() -> Path:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(dir=SCRATCH))


def _recovery_directory(module, output_name: str):
    folder = _scratch()
    try:
        return module.recovery_directory(str(folder / output_name)).name
    finally:
        shutil.rmtree(folder)


def _session_json(module, rate: int, channels: int, width: int, created: str):
    folder = _scratch()
    fixed = _datetime.datetime.fromisoformat(created)

    class _Fixed(_datetime.datetime):
        @classmethod
        def now(cls, tz=None):
            return fixed

    saved = module.datetime
    module.datetime = _Fixed
    try:
        journal = module.RecordingJournal(str(folder / "recorded_audio.wav"), rate, channels,
                                          width, lambda message: None)
        try:
            return (journal.directory / "session.json").read_text(encoding="utf-8")
        finally:
            journal.close(discard=True)
    finally:
        module.datetime = saved
        shutil.rmtree(folder)


def _recovered_wav_path(module, created: str, session: str):
    folder = _scratch()
    try:
        return module._recovered_wav_path({"created": created}, str(folder / "recordings"),
                                          folder / session).name
    finally:
        shutil.rmtree(folder)


def _session_format(module, text: str):
    folder = _scratch()
    try:
        (folder / "session.json").write_text(text, encoding="utf-8")
        _meta, audio_format = module._session_format(folder)
        return list(audio_format)
    finally:
        shutil.rmtree(folder)


WRAPPERS = {
    "recovery_directory": _recovery_directory,
    "session_json": _session_json,
    "_recovered_wav_path": _recovered_wav_path,
    "_session_format": _session_format,
}

_SESSION = "0123456789abcdef0123456789abcdef"

CASES = {
    "recovery_directory": [
        ["recorded_audio.wav"],
        ["capture.wav"],
        ["dictation.take.wav"],
        ["noextension"],
        [".hidden"],
    ],
    "session_json": [
        [44100, 1, 2, "2026-11-14T09:30:00"],
        [44100, 1, 2, "2026-11-14T09:30:00.123456"],
        [16000, 2, 2, "1999-12-31T23:59:59.000001"],
        [48000, 1, 4, "2026-01-02T03:04:05.500000"],
    ],
    "_recovered_wav_path": [
        ["2026-11-14T09:30:00", _SESSION],
        ["2026-11-14T09:30:00.123456", _SESSION],
        ["1999-12-31T23:59:59.999999", "abc"],
        ["2026-01-02T03:04:05+02:00", _SESSION],
        ["2026-01-02", _SESSION],
        ["not a time", _SESSION],
    ],
    "_session_format": [
        ['{"version": 1, "rate": 44100, "channels": 1, "sample_width": 2, "created": "x"}'],
        ['{"rate": 8000, "channels": 2, "sample_width": 1}'],
        ['{"rate": 384000, "channels": 1, "sample_width": 4}'],
        ['{"rate": 7999, "channels": 1, "sample_width": 2}'],
        ['{"rate": 384001, "channels": 1, "sample_width": 2}'],
        ['{"rate": "44100", "channels": 1, "sample_width": 2}'],
        ['{"rate": true, "channels": 1, "sample_width": 2}'],
        ['{"rate": 44100, "channels": 0, "sample_width": 2}'],
        ['{"rate": 44100, "channels": 3, "sample_width": 2}'],
        ['{"rate": 44100, "channels": true, "sample_width": 2}'],
        ['{"rate": 44100, "channels": 1, "sample_width": 0}'],
        ['{"rate": 44100, "channels": 1, "sample_width": 5}'],
        ['{"rate": 44100, "channels": 1, "sample_width": 3}'],
        ['{"rate": 44100, "channels": 1}'],
        ['{"rate": 44100, "channels": "1", "sample_width": 2}'],
        ['not json'],
    ],
}
