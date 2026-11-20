"""Curated inputs for services/recorder.py (fv_recorder's post_roll.h and Recorder, fv_dsp's
block_level_db).

block_level_db takes the dtype ("int16" or "float32") and the samples; floats are float32
values exact in binary so the C++ replay feeds the same block. post_roll_gate drives one
PostRollGate the way the recorder does: every level of `before` is observed as a block of
`frames` frames, then arm() at the stop press, then every level of `after`; it reports the
frozen threshold, the fallback, the indexes of `after` at which observe() returned True (at
most one) and describe(). `adaptive` stands for config.POST_ROLL_ADAPTIVE. Levels include
halves, which Python's round() takes to the even bin. format_start_error is the user-facing
start failure.
"""
from __future__ import annotations

import math


def _block_level_db(module, dtype: str, samples: list):
    import numpy as np

    return module.block_level_db(np.asarray(samples, dtype=np.dtype(dtype)))


def _post_roll_gate(module, rate: int, before: list, after: list, frames: int,
                    adaptive: bool = True):
    from config import config

    saved = config.POST_ROLL_ADAPTIVE
    config.POST_ROLL_ADAPTIVE = adaptive
    try:
        gate = module.PostRollGate(rate)
        for level in before:
            gate.observe(level, frames)
        gate.arm()
        ended_at = [index for index, level in enumerate(after) if gate.observe(level, frames)]
        return {
            "threshold_db": gate.threshold_db,
            "floor_db": gate.floor_db,
            "speech_db": gate.speech_db,
            "fallback": gate.fallback,
            "ended_at": ended_at,
            "describe": gate.describe(),
            "frames_before_stop": gate.frames_before_stop,
            "frames_after_stop": gate.frames_after_stop,
        }
    finally:
        config.POST_ROLL_ADAPTIVE = saved


def _format_start_error(module, message: str):
    return module.AudioRecorder._format_start_error(Exception(message))


WRAPPERS = {
    "block_level_db": _block_level_db,
    "post_roll_gate": _post_roll_gate,
    "format_start_error": _format_start_error,
}

_SINE = [int(32767 * math.sin(2 * math.pi * 1000 * i / 44100)) for i in range(1024)]


def _dictation(floor, speech, blocks, gap=None):
    """Lead-in quiet, then speech with a short quiet gap every 22 blocks (the shape of
    tests/test_recorder_postroll.py's _dictation)."""
    gap = floor if gap is None else gap
    levels = []
    for i in range(blocks):
        if i < 10 or i % 22 in (0, 1, 2):
            levels.append(gap if i >= 10 else floor)
        else:
            levels.append(speech)
    return levels


CASES = {
    "block_level_db": [
        ["int16", []],
        ["int16", [0] * 1024],
        ["int16", _SINE],
        ["int16", [-32768] * 512],
        ["int16", [16384] * 64],
        ["int16", [1] * 10],
        ["int16", [-32768, 0, 0, 0]],
        ["int16", [100, -100, 200, -200]],
        ["int16", [3, -7, 11, -13, 17, -19, 23, -29]],
        ["float32", []],
        ["float32", [0.0] * 1024],
        ["float32", [0.5] * 1024],
        ["float32", [1.0] * 512],
        ["float32", [0.25, -0.25, 0.125, -0.125]],
        ["float32", [2.0 ** -20] * 16],
    ],
    "post_roll_gate": [
        # Speech then quiet: ends after the quiet window.
        [44100, _dictation(-60.0, -25.0, 172), [-25.0] * 6 + [-60.0] * 60, 1024],
        # A pause one block short of the window, then more speech.
        [44100, _dictation(-60.0, -25.0, 129), [-60.0] * 12 + [-28.0] * 8 + [-60.0] * 40, 1024],
        # Continued speech never ends.
        [44100, _dictation(-60.0, -25.0, 129), [-25.0] * 80, 1024],
        # Nobody spoke.
        [44100, [-55.0] * 150, [-70.0] * 60, 1024],
        # A loud room: speech only 10 dB over the floor.
        [44100, _dictation(-35.0, -25.0, 172), [-60.0] * 60, 1024],
        # The switch off.
        [44100, _dictation(-60.0, -25.0, 172), [-60.0] * 60, 1024, False],
        # Too short for a floor, and just long enough with a lead-in.
        [44100, [-60.0] * 4 + [-25.0] * 16, [-60.0] * 60, 1024],
        [44100, [-60.0] * 6 + [-25.0] * 18, [-60.0] * 60, 1024],
        # Stopped before any audio.
        [44100, [], [-90.0] * 60, 1024],
        # A near digital silence floor: the speech headroom sets the threshold.
        [44100, _dictation(-95.0, -25.0, 215), [-70.0, -95.0] * 30, 1024],
        [44100, _dictation(-95.0, -25.0, 215), [-95.0] * 4 + [-40.0] * 10 + [-95.0] * 40, 1024],
        # Percentiles over the whole recording.
        [44100, [-60.0] * 30 + [-20.0] * 70, [-90.0], 1024],
        # Halves round to the even bin.
        [44100, [-60.5] * 30 + [-20.5] * 70, [-48.5, -49.5] * 20, 1024],
        [44100, [-59.5] * 30 + [-21.5] * 70, [-47.5] * 20, 1024],
        # Floors and ceilings of the histogram.
        [44100, [-150.0] * 30 + [5.0] * 70, [-130.0] * 30, 1024],
        # Other rates and block sizes.
        [16000, [-50.0] * 20 + [-10.0] * 30, [-45.0] * 30, 512],
        [48000, [-70.0] * 40 + [-30.0] * 60, [-62.0] * 20 + [-30.0] + [-62.0] * 20, 480],
        [44100, [-62.0] * 25 + [-41.0] * 25, [-50.0] * 40, 1024],
    ],
    "format_start_error": [
        ["Error querying device -1"],
        ["Error opening InputStream: Device unavailable [PaErrorCode -9985]"],
        ["No Default Input Device Available"],
        ["Invalid device [PaErrorCode -9996]"],
        ["There is no such device"],
        ["PortAudio not initialized [PaErrorCode -10000]"],
        ["Invalid sample rate [PaErrorCode -9997]"],
        ["Permission denied"],
        [""],
    ],
}
