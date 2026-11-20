"""Curated inputs for meeting/capture/health.py (M7.8 meeting-capture).

Each case is one CaptureSignalMonitor driven by a timed trace, every time explicit:

    ["reset", now]                                   reset_stream(now=now)
    ["observe", value, samples, rate, signal, now]   observe(np.full(samples, value) at rate)
    ["snapshot", now]                                the snapshot dict is collected

A monitor built without a reset has its first generation's start at the real monotonic time, so
the traces that skip the reset snapshot only after a block has arrived.
"""

import numpy as np

from meeting.interfaces import CaptureBlock


def _trace(module, steps, signal_rms=None):
    monitor = (module.CaptureSignalMonitor() if signal_rms is None
               else module.CaptureSignalMonitor(signal_rms=signal_rms))
    out = []
    for step in steps:
        name = step[0]
        if name == "reset":
            monitor.reset_stream(now=step[1])
        elif name == "observe":
            _, value, samples, rate, signal, now = step
            block = CaptureBlock("mic", np.full(samples, value, np.int16), rate, now)
            monitor.observe(block, signal_enabled=signal, now=now)
        elif name == "snapshot":
            out.append(monitor.snapshot(now=step[1]))
        else:
            raise ValueError(f"unknown step {name}")
    return out


def _steady(value, start, count, every=0.25, samples=4000, rate=16000, signal=True):
    return [["observe", value, samples, rate, signal, start + i * every] for i in range(count)]


CASES = {
    "trace": [
        [[["reset", 0.0], ["observe", 0, 4000, 16000, True, 0.25], ["snapshot", 0.5]]],
        [[["reset", 0.0], ["observe", 50, 4000, 16000, True, 0.25], ["snapshot", 0.3],
          *_steady(5000, 0.5, 3), ["snapshot", 1.1]]],
        [[["reset", 0.0],
          *[["observe", v, 4000, 16000, True, t]
            for t, v in ((0.25, 6000), (0.5, 0), (0.75, 6000), (1.0, 6000), (1.25, 6000))],
          ["snapshot", 1.3], *_steady(6000, 1.5, 8), ["snapshot", 3.3]]],
        [[*_steady(3000, 1.0, 3), ["snapshot", 1.1]]],
        [[["reset", 0.0], ["snapshot", 3.9], ["snapshot", 4.0], *_steady(1000, 4.1, 3),
          ["snapshot", 4.2], ["snapshot", 7.6], ["reset", 8.0], ["snapshot", 8.1]]],
        [[["reset", 0.0], ["observe", 5000, 4000, 16000, False, 0.3], ["snapshot", 0.4]]],
        # 1024-frame blocks at 44.1 kHz: a window closes on the 11th block (11264 >= 11025).
        [[["reset", 100.0], *_steady(400, 100.0, 40, every=1024 / 44100, samples=1024,
                                     rate=44100), ["snapshot", 101.0], ["snapshot", 103.9],
          ["snapshot", 104.0], ["snapshot", 102.99]]],
        # Exactly at the threshold, and one below it.
        [[["reset", 0.0], *_steady(300, 0.1, 6), ["snapshot", 2.0], ["reset", 2.0],
          *_steady(299, 2.1, 6), ["snapshot", 4.0]]],
        # A rate change restarts the window; paused blocks clear the run of signal windows.
        [[["reset", 0.0], ["observe", 5000, 4000, 16000, True, 0.25],
          ["observe", 5000, 6000, 48000, True, 0.5], ["observe", 5000, 6000, 48000, True, 0.6],
          ["observe", 5000, 6000, 48000, True, 0.7], ["snapshot", 0.8],
          ["observe", 5000, 6000, 48000, False, 0.9], ["observe", 5000, 12000, 48000, True, 3.0],
          ["snapshot", 3.1], ["observe", 5000, 12000, 48000, True, 3.2], ["snapshot", 3.3]]],
        # The report interval: signal every window, pulses at most every 2 s.
        [[["reset", 0.0], *_steady(9000, 0.25, 40), ["snapshot", 10.1], ["snapshot", 13.0],
          ["snapshot", 13.2]]],
        # Empty blocks are ignored, an alternating level never makes two windows in a row.
        [[["reset", 0.0], ["observe", 9000, 0, 16000, True, 0.1],
          *[["observe", 9000 if i % 2 == 0 else 10, 4000, 16000, True, 0.2 + 0.25 * i]
            for i in range(12)], ["snapshot", 3.5], ["snapshot", 6.3]]],
        # Rate 0 counts as 1 Hz: every block closes a window.
        [[["reset", 0.0], ["observe", 400, 3, 0, True, 1.0], ["observe", 400, 3, 0, True, 2.0],
          ["snapshot", 2.5]]],
    ],
    "trace_with_threshold": [
        [[["reset", 0.0], *_steady(100, 0.25, 3), ["snapshot", 1.0]], 50.0],
        [[["reset", 0.0], *_steady(100, 0.25, 3), ["snapshot", 1.0]], 100.0],
        [[["reset", 0.0], *_steady(100, 0.25, 3), ["snapshot", 1.0]], 100.5],
    ],
}

WRAPPERS = {
    "trace": lambda module, steps: _trace(module, steps),
    "trace_with_threshold": lambda module, steps, rms: _trace(module, steps, rms),
}
