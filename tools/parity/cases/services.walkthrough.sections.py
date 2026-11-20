"""Curated inputs for services/walkthrough/sections.py (fv_walkthrough's sections.h).

Recordings are synthetic int16 signals described as data, so the C++ replay builds the same
samples without floating-point sine: {"rate": r, "channels": [segments of channel 0, of channel
1, ...]}, every channel the same length, each segment one of

    ["quiet", frames]                     zeros
    ["square", frames, amplitude, half]   +amplitude for `half` frames, then -amplitude, ...
    ["noise", frames, amplitude, seed]    x = (1103515245 * x + 12345) % 2**31 from x = seed,
                                          each sample x % (2 * amplitude + 1) - amplitude

counted from the segment's first frame. A mono signal is shaped (frames,), a multi-channel one
(frames, channels), as the recorder's WAV reads back.

    cut_frames(signal, marks_s)                         the cut frames
    _quietest(signal, low, high, target)                the cut frame
    _without(start, end, gaps)                          spans, as lists
    split_recording(signal, marks_s, leave_out_s)       per clip [file name or None,
    clip_stretches(signal, stretches_s)                 duration_s, frames in the file or None,
                                                        channels in the file or None]

The last two write the signal as a 16-bit WAV in a scratch folder under out/parity, call the
function there, read the clips back and remove the folder.
"""
from __future__ import annotations

import shutil
import tempfile
import wave
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
SCRATCH = REPO / "out" / "parity" / "walkthrough_sections"


def _channel(segments):
    samples = []
    for segment in segments:
        kind, frames = segment[0], segment[1]
        if kind == "quiet":
            samples += [0] * frames
        elif kind == "square":
            amplitude, half = segment[2], segment[3]
            samples += [amplitude if (i // half) % 2 == 0 else -amplitude for i in range(frames)]
        elif kind == "noise":
            amplitude, x = segment[2], segment[3]
            for _ in range(frames):
                x = (1103515245 * x + 12345) % 2**31
                samples.append(x % (2 * amplitude + 1) - amplitude)
        else:
            raise ValueError(kind)
    return samples


def _samples(signal):
    channels = [np.array(_channel(c), dtype=np.int16) for c in signal["channels"]]
    if len(channels) == 1:
        return channels[0]
    return np.column_stack(channels)


def _cut_frames(module, signal, marks):
    return module.cut_frames(_samples(signal), signal["rate"], marks)


def _quietest(module, signal, low, high, target):
    return module._quietest(_samples(signal), signal["rate"], low, high, target)


def _without(module, start, end, gaps):
    return [list(span) for span in module._without(start, end, [tuple(g) for g in gaps])]


def _write(path, signal):
    samples = _samples(signal)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(len(signal["channels"]))
        out.setsampwidth(2)
        out.setframerate(signal["rate"])
        out.writeframes(samples.astype("<i2").tobytes())


def _clips(clips):
    result = []
    for clip in clips:
        if clip.path is None:
            result.append([None, clip.duration_s, None, None])
            continue
        with wave.open(str(clip.path), "rb") as read:
            result.append([clip.path.name, clip.duration_s, read.getnframes(),
                           read.getnchannels()])
    return result


def _in_scratch(call):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(dir=SCRATCH))
    try:
        return call(folder)
    finally:
        shutil.rmtree(folder)


def _split_recording(module, signal, marks, leave_out):
    def call(folder):
        source = folder / "recording.wav"
        _write(source, signal)
        out = folder / "clips"
        out.mkdir()
        return _clips(module.split_recording(source, marks, out,
                                             leave_out_s=[tuple(s) for s in leave_out]))
    return _in_scratch(call)


def _clip_stretches(module, signal, stretches):
    def call(folder):
        source = folder / "recording.wav"
        _write(source, signal)
        out = folder / "clips"
        out.mkdir()
        return _clips(module.clip_stretches(source, [tuple(s) for s in stretches], out))
    return _in_scratch(call)


WRAPPERS = {
    "cut_frames": _cut_frames,
    "_quietest": _quietest,
    "_without": _without,
    "split_recording": _split_recording,
    "clip_stretches": _clip_stretches,
}

INF = float("inf")


def _mono(rate, *segments):
    return {"rate": rate, "channels": [list(segments)]}


# Speech, a pause, speech: 1.5 s of square tone, 0.3 s quiet, 1.5 s of tone, at 16 kHz.
SPEECH_PAUSE = _mono(16000, ["square", 24000, 8000, 18], ["quiet", 4800], ["square", 24000, 8000, 18])
STEADY = _mono(16000, ["square", 64000, 8000, 18])
# A softer stretch (0.6 amplitude, more than half the energy of the louder tone).
SOFTER = _mono(16000, ["square", 22400, 8000, 18], ["square", 3200, 6000, 18],
               ["square", 22400, 8000, 18])
NOISY_PAUSE = _mono(16000, ["noise", 20000, 9000, 7], ["noise", 3200, 300, 11],
                    ["noise", 20000, 9000, 13])
TWO_PAUSES = _mono(16000, ["square", 24000, 8000, 18], ["quiet", 3200], ["square", 24000, 8000, 18],
                   ["quiet", 3200], ["square", 24000, 8000, 18])
STEREO = {"rate": 16000, "channels": [
    [["square", 16000, 8000, 18], ["quiet", 4800], ["square", 16000, 8000, 18]],
    [["noise", 16000, 3000, 5], ["quiet", 4800], ["noise", 16000, 3000, 9]],
]}
# The left channel pauses where the right one speaks: the sum decides.
STEREO_CROSSED = {"rate": 16000, "channels": [
    [["square", 16000, 8000, 18], ["quiet", 4000], ["square", 16000, 8000, 18]],
    [["square", 14000, 1000, 18], ["quiet", 8000], ["square", 14000, 1000, 18]],
]}
AT_44K = _mono(44100, ["noise", 66150, 7000, 3], ["quiet", 13230], ["noise", 66150, 7000, 4])
SHORT = _mono(16000, ["square", 1000, 8000, 18])
QUIET = _mono(16000, ["quiet", 32000])
# Equal energy everywhere in reach: ties go to the window nearest the mark.
EVEN = _mono(16000, ["quiet", 48000])

CASES = {
    "cut_frames": [
        [SPEECH_PAUSE, [1.4]],
        [SPEECH_PAUSE, [1.8]],
        [SPEECH_PAUSE, [0.5]],
        [STEADY, [1.5]],
        [SOFTER, [1.2]],
        [SOFTER, [1.45]],
        [NOISY_PAUSE, [1.0]],
        [NOISY_PAUSE, [1.5]],
        [TWO_PAUSES, [1.2, 3.0]],
        [TWO_PAUSES, [1.6, 1.62]],
        [TWO_PAUSES, [1.0, 1.1]],
        [TWO_PAUSES, [1.6, 1.6, 1.6]],
        [TWO_PAUSES, [3.4, 1.0]],
        [STEADY, [5.0, 6.0]],
        [SHORT, [5.0, 6.0]],
        [SHORT, [0.01]],
        [STEADY, []],
        [STEREO, [0.9]],
        [STEREO, [1.3]],
        [STEREO_CROSSED, [0.95]],
        [AT_44K, [1.4]],
        [AT_44K, [1.55, 1.6]],
        [QUIET, [1.0]],
        [EVEN, [1.0, 1.03125]],
        [SPEECH_PAUSE, [0.0]],
        [SPEECH_PAUSE, [-1.0]],
        [SPEECH_PAUSE, [1.50003125]],
        [SPEECH_PAUSE, [1.000003125]],
    ],
    "_quietest": [
        [SPEECH_PAUSE, 12000, 36000, 24000],
        [SPEECH_PAUSE, 12000, 24000, 18000],
        [SPEECH_PAUSE, 24000, 25000, 24000],
        [SPEECH_PAUSE, 24000, 25600, 25000],
        [SPEECH_PAUSE, 26000, 20000, 23000],
        [STEADY, 0, 64000, 32000],
        [NOISY_PAUSE, 8000, 32000, 16000],
        [EVEN, 0, 48000, 16000],
        [EVEN, 0, 48000, 16001],
        [STEREO, 2400, 26400, 14400],
        [STEREO_CROSSED, 3200, 27200, 15200],
        [AT_44K, 28665, 94815, 61740],
    ],
    "_without": [
        [0, 100, []],
        [0, 100, [[10, 20]]],
        [0, 100, [[0, 20], [80, 100]]],
        [0, 100, [[-10, 5], [50, 200]]],
        [30, 60, [[10, 20], [25, 35], [40, 45], [55, 70], [90, 95]]],
        [30, 60, [[10, 30], [60, 70]]],
        [30, 60, [[20, 70]]],
        [30, 30, [[10, 20]]],
        [0, 100, [[40, 30]]],
    ],
    "split_recording": [
        [TWO_PAUSES, [1.6, 3.3], []],
        [STEADY, [4.0 - 0.125], []],
        [STEADY, [0.0], []],
        [STEADY, [], []],
        [STEADY, [], [[2.0, INF]]],
        [STEADY, [3.0], [[2.0, 3.5]]],
        [STEADY, [2.0], [[0.5, 1.0], [1.5, 2.5], [3.0, INF]]],
        [STEADY, [1.0, 1.1, 2.0], []],
        [STEREO, [0.9], []],
        [AT_44K, [1.5], [[0.25, 0.75]]],
        [SPEECH_PAUSE, [1.4], [[0.0, 0.1]]],
        [SPEECH_PAUSE, [5.0], []],
    ],
    "clip_stretches": [
        [STEADY, [[1.0, 2.0]]],
        [STEADY, [[2.0, INF]]],
        [STEADY, [[1.0, 1.125]]],
        [STEADY, [[0.5, 1.5], [3.0, 3.9]]],
        [STEADY, [[-1.0, 0.5], [3.5, 9.0]]],
        [STEREO, [[0.2, 0.9]]],
        [AT_44K, [[0.1, 0.6]]],
        [STEADY, [[3.0, 2.0]]],
    ],
}
