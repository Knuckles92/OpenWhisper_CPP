"""Curated inputs for meeting/asr/offline.py (M7.12 meeting-asr).

Audio is described, never listed, as in meeting.capture.spool's cases: a signal is a list of
segments [kind, frames, amp, step] ("saw", "const", "zero"); "saw" is
((i * step) % 65536) * 2 * amp // 65536 - amp with i counted from the signal's first sample.

    offline_cut_ranges_signal(sample_count, rate, signal or None, target, max, quiet_window,
                              overlap, quiet_rms)       the ranges as [start, end] lists
    drop_overlapped_prefix_rows(rows, keep_from_s)     ids of the kept rows
    drop_mic_echo_rows(rows)                           ids of the kept rows
        rows are [segment_id, channel, start_s, end_s, text]
    offline_segment_id(meeting_id, channel, start_s, ordinal)
    session_pass(signal, rate, origin_s, target, max, overlap, channel, windows)
        transcribe_session_audio with a model that answers window i with windows[i] (a list of
        [start, end, text]; the last list repeats): the segments as [segment_id, start_s,
        end_s, text], each window's sample count, and the progress details.
"""

from types import SimpleNamespace

import numpy as np

from meeting.interfaces import TranscriptSegment


def _signal(segments):
    parts = []
    offset = 0
    for kind, frames, amp, step in segments:
        index = np.arange(offset, offset + frames, dtype=np.int64)
        if kind == "saw":
            values = ((index * step) % 65536) * 2 * amp // 65536 - amp
        elif kind == "const":
            values = np.full(frames, amp, dtype=np.int64)
        elif kind == "zero":
            values = np.zeros(frames, dtype=np.int64)
        else:
            raise ValueError(f"unknown segment {kind}")
        parts.append(values.astype(np.int16))
        offset += frames
    return np.concatenate(parts) if parts else np.zeros(0, dtype=np.int16)


def _cut_ranges(module, count, rate, signal, target, max_sec, quiet_window, overlap, quiet_rms):
    audio = None if signal is None else _signal(signal)
    return [list(pair) for pair in module.offline_cut_ranges(
        count, rate, audio, target_sec=target, max_sec=max_sec,
        quiet_window_s=quiet_window, overlap_s=overlap, quiet_rms=quiet_rms)]


def _segments(rows):
    return [TranscriptSegment(segment_id=r[0], meeting_id="m", chunk_id=None, channel=r[1],
                              start_s=r[2], end_s=r[3], text=r[4]) for r in rows]


def _overlap(module, rows, keep_from_s):
    return [seg.segment_id for seg in module.drop_overlapped_prefix(_segments(rows), keep_from_s)]


def _echo(module, rows):
    return [seg.segment_id for seg in module.drop_mic_echo(_segments(rows))]


class _Model:
    def __init__(self, windows):
        self.windows = windows
        self.sizes = []

    def transcribe(self, audio, **_options):
        index = min(len(self.sizes), len(self.windows) - 1)
        self.sizes.append(int(audio.size))
        segments = [SimpleNamespace(start=s, end=e, text=t) for s, e, t in self.windows[index]]
        return iter(segments), SimpleNamespace()


def _session(module, signal, rate, origin_s, target, max_sec, overlap, channel, windows):
    model = _Model(windows)
    details = []
    segments = module.transcribe_session_audio(
        model, _signal(signal), rate, meeting_id="m_golden", channel=channel, origin_s=origin_s,
        target_sec=target, max_sec=max_sec, overlap_s=overlap,
        progress_cb=lambda detail, current, total: details.append([detail, current, total]))
    return {
        "segments": [[s.segment_id, s.start_s, s.end_s, s.text] for s in segments],
        "windows": model.sizes,
        "progress": details,
    }


_LOUD = ["const", 16000, 5000, 0]
_QUIET = ["zero", 16000, 0, 0]

CASES = {
    "offline_cut_ranges_signal": [
        [90 * 16000, 16000, None, 15.0, 25.0, 0.7, 1.0, 300.0],
        [90 * 16000, 16000, None, 15.0, 25.0, 0.7, 0.0, 300.0],
        [0, 16000, None, 15.0, 25.0, 0.7, 0.0, 300.0],
        [-5, 16000, None, 15.0, 25.0, 0.7, 0.0, 300.0],
        [10, 0, None, 15.0, 25.0, 0.7, 0.0, 300.0],
        [40 * 16000, 16000, [["const", 16 * 16000, 5000, 0], ["zero", 16000, 0, 0],
                             ["const", 23 * 16000, 5000, 0]], 15.0, 25.0, 0.7, 1.0, 300.0],
        [40 * 16000, 16000, [["const", 16 * 16000, 5000, 0], ["zero", 16000, 0, 0],
                             ["const", 23 * 16000, 5000, 0]], 15.0, 25.0, 0.7, 0.0, 300.0],
        [60 * 16000, 16000, [["saw", 17 * 16000, 6000, 641], ["zero", 12000, 0, 0],
                             ["saw", 20 * 16000, 6000, 641], ["zero", 16000, 0, 0],
                             ["saw", 20 * 16000, 6000, 641]], 15.0, 25.0, 0.7, 0.0, 300.0],
        # Audio shorter than the sample count: the tail is hard-cut.
        [50 * 16000, 16000, [["const", 10 * 16000, 5000, 0]], 15.0, 25.0, 0.7, 0.0, 300.0],
        # An overlap at least as long as the window still advances.
        [5 * 16000, 16000, None, 1.0, 1.0, 0.7, 2.0, 300.0],
        [3 * 16000, 16000, None, 1.0, 1.0, 0.7, 0.5, 300.0],
        [2 * 16000, 16000, [["const", 2 * 16000, 1000, 0]], 1.0, 1.0, 0.7, 0.0, 300.0],
        [100, 16000, None, 0.0, 0.0, 0.7, 0.0, 300.0],
        [48000 * 30, 48000, [["saw", 48000 * 16, 9000, 997], ["zero", 48000, 0, 0],
                             ["saw", 48000 * 13, 9000, 997]], 15.0, 25.0, 0.7, 0.0, 300.0],
        [16000 * 30, 16000, [["const", 16000 * 16, 299, 0], ["const", 16000 * 14, 301, 0]],
         15.0, 25.0, 0.7, 0.0, 300.0],
        [16000 * 30, 16000, [["const", 16000 * 30, 301, 0]], 15.0, 25.0, 0.7, 0.0, 300.0],
    ],
    "drop_overlapped_prefix_rows": [
        [[["a", "mic", 9.2, 9.8, "only overlap"], ["b", "mic", 10.5, 12.0, "new"]], 10.0],
        [[["a", "mic", 9.2, 9.8, "only overlap"], ["b", "mic", 9.5, 12.0, "spans"]], 10.0],
        [[["a", "mic", 9.0, 10.0, "ends at the edge"], ["b", "mic", 9.0, 10.000001, "past"]],
         10.0],
        [[], 1.0],
    ],
    "drop_mic_echo_rows": [
        [[["lb1", "loopback", 10.0, 14.0, "The vendor ships the parts on Monday."],
          ["mic_echo", "mic", 10.4, 14.3, "vendor ships the parts on Monday"],
          ["mic_real", "mic", 15.0, 17.0, "Great, I will tell the warehouse."],
          ["mic_short", "mic", 10.5, 11.0, "Monday."]]],
        [[["lb1", "loopback", 10.0, 12.0, "ship the parts on Monday"],
          ["mic1", "mic", 40.0, 42.0, "ship the parts on Monday"]]],
        [[["mic1", "mic", 0.0, 1.0, "nothing to compare with here"]]],
        # Exactly 60 % shared (3 of 5) is an echo; 2 of 4 is not.
        [[["lb", "loopback", 0.0, 3.0, "alpha beta gamma"],
          ["m5", "mic", 0.5, 2.5, "alpha beta gamma delta epsilon"],
          ["m4", "mic", 0.5, 2.5, "alpha beta delta epsilon"]]],
        # The slack: a loopback segment ending one second before the mic one starts is too
        # far; one ending just after the slack edge overlaps.
        [[["lb", "loopback", 0.0, 4.0, "one two three"],
          ["far", "mic", 5.0, 6.0, "one two three"],
          ["near", "mic", 4.999, 6.0, "one two three"]]],
        # Repeated words count each time; case and punctuation are ignored; \w is Unicode.
        [[["lb", "loopback", 0.0, 2.0, "Ça va, très bien"],
          ["m", "mic", 0.0, 2.0, "ça VA très très bien!"],
          ["other", "other", 0.0, 2.0, "ça va très bien"]]],
        [[["lb", "loopback", 0.0, 2.0, "yes yes"], ["m", "mic", 0.0, 2.0, "yes yes yes no"]]],
    ],
    "offline_segment_id": [
        ["m1", "mic", 1.25, 0],
        ["m1", "mic", 1.25, 1],
        ["m1", "loopback", 0.0, 0],
        ["m1", "mic", 1.0005, 0],
        ["m1", "mic", 1.0015, 0],
        ["m1", "mic", -0.0001, 3],
        ["m_ü", "mic", 3600.4375, 12],
        ["m1", "mic", 1e15, 0],
        ["m1", "mic", float("nan"), 0],
        ["m1", "mic", float("inf"), 0],
    ],
    "session_pass": [
        [[["const", 3 * 16000, 1000, 0]], 16000, 100.0, 1.0, 1.0, 0.0, "loopback",
         [[[0.0, 0.1, ""], [0.25, 0.75, " said "]]]],
        [[["const", 2 * 16000, 1000, 0]], 16000, 0.0, 1.0, 1.0, 0.0, "mic",
         [[[0.0, 0.5, "first"]], [[0.0, 0.4, "second"], [0.5, 1.0, "third"]]]],
        # Overlapping windows: the later window owns the overlap.
        [[["const", 4 * 16000, 1000, 0]], 16000, 10.0, 2.0, 2.0, 0.5, "mic",
         [[[0.0, 0.5, "a"], [1.6, 1.9, "tail"]], [[0.0, 0.4, "overlap only"],
                                                   [0.2, 0.9, "spans"]],
          [[0.1, 0.3, "late"]]]],
        [[["saw", 40 * 16000, 6000, 641]], 16000, 0.0, 15.0, 25.0, 0.0, "custom",
         [[[0.0, 1.0, "x"]]]],
        [[["const", 30 * 44100, 1000, 0]], 44100, 5.0, 15.0, 25.0, 0.0, "mic",
         [[[0.0, 1.0, "resampled"]]]],
    ],
}

WRAPPERS = {
    "offline_cut_ranges_signal": _cut_ranges,
    "drop_overlapped_prefix_rows": _overlap,
    "drop_mic_echo_rows": _echo,
    "session_pass": _session,
}

PINS = {
    "signal_note": "segments [kind, frames, amp, step]; saw = ((i * step) % 65536) * 2 * amp "
                   "// 65536 - amp with i counted from the signal's first sample",
}

FLOAT_TOLERANCE = 1e-9
