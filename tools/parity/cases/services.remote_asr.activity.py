"""Curated inputs for services/remote_asr/activity.py (M9.5 host-state): HostActivity driven by
a script of calls over a fake clock, and empty_snapshot.

script(max_events, steps) builds HostActivity(clock, max_events) with a clock at 1000.0 and runs
the steps in order. A step is a list: ["tick", seconds] moves the clock; ["reset"]; a recording
method and its arguments (["connected", id, name], ["disconnected", id, name], ["transcribed",
id, name, audio_s, host_s], ["previewed", id, name], ["failed", id, name, reason], ["paired",
id, name, via], ["switched", name, label], ["renamed", id, name]); ["snapshot", events] records
snapshot(events) (null for all). The result is the list of recorded snapshots. Clock steps are
binary fractions, so wall times read the same through a C++ clock's ticks.
"""


class _Clock:
    def __init__(self, now: float):
        self.now = now

    def __call__(self) -> float:
        return self.now


def _script(module, max_events, steps):
    clock = _Clock(1000.0)
    activity = module.HostActivity(clock=clock, max_events=max_events)
    snapshots = []
    for step in steps:
        op, args = step[0], step[1:]
        if op == "tick":
            clock.now += args[0]
        elif op == "snapshot":
            snapshots.append(activity.snapshot(events=args[0]))
        else:
            getattr(activity, op)(*args)
    return snapshots


def _each(steps):
    """The steps with a full snapshot after every one."""
    out = []
    for step in steps:
        out.append(step)
        out.append(["snapshot", None])
    return out


WRAPPERS = {"script": _script}

_HOST_MODE = [
    # test_host_mode.py's sequence, then a few more kinds.
    ["connected", "d1", "laptop"],
    ["tick", 5],
    ["transcribed", "d1", "laptop", 8.8, 0.4],
    ["previewed", "d1", "laptop"],
    ["tick", 5],
    ["transcribed", "d2", "desk", 2.0, 0.1],
    ["paired", "d3", "studio", "tailscale"],
    ["switched", "desk", "Parakeet TDT 0.6B v3"],
    ["tick", 0.5],
    ["failed", "d2", "desk", "the engine fell over"],
    ["renamed", "d2", "Kitchen desk"],
    ["renamed", "nobody", "ghost"],
    ["disconnected", "d1", "laptop"],
]

_CONNECTIONS = [
    ["connected", "d1", "laptop"],
    ["connected", "d1", "laptop"],
    ["disconnected", "d1", "laptop"],
    ["disconnected", "d1", "laptop"],
    ["disconnected", "d1", "laptop"],
    ["connected", "d2", ""],
    ["connected", "d2", "desk"],
    ["disconnected", "d2", "desk"],
]

_RESET = [
    ["connected", "d1", "laptop"],
    ["transcribed", "d1", "laptop", 3.0, 0.2],
    ["tick", 1000],
    ["reset"],
    ["disconnected", "d1", "laptop"],
    ["paired", "d2", "desk", "code"],
    ["connected", "d2", "desk"],
    ["tick", 0.25],
    ["reset"],
    ["transcribed", "d2", "desk", 1.5, 0.75],
]

_ROUNDING = [
    ["transcribed", "d1", "laptop", 2.675, 0.12345],
    ["transcribed", "d1", "", 1.005, 0.0005],
    ["transcribed", "d1", "laptop", -1.234, -0.5],
    ["transcribed", "d2", "desk", 0.0, 0.0],
    ["tick", 2.75],
    ["transcribed", "d2", "desk", 123.456789, 9.87654321],
    ["previewed", "d3", "pad"],
]

_REASONS = [
    ["failed", "d1", "laptop", "Engine\n   exploded " + "x" * 400],
    ["failed", "d1", "laptop", ""],
    ["failed", "d1", "laptop", "  tabs\tand\r\nnewlines and em spaces  "],
    ["failed", "d1", "laptop", "x" * 160],
    ["failed", "d1", "laptop", "x" * 161],
    ["failed", "d1", "laptop", "x" * 158 + " " + "y" * 10],
    ["failed", "d1", "laptop", "é" * 200],
    ["failed", "d1", "laptop", "\U0001f600" * 170],
    ["failed", "d2", "", "no name"],
]

_SNAPSHOT_LIMITS = [
    ["connected", "d1", "laptop"],
    ["switched", "laptop", "A"],
    ["switched", "laptop", "B"],
    ["snapshot", 0],
    ["snapshot", 1],
    ["snapshot", 2],
    ["snapshot", -1],
    ["snapshot", 100],
]


def _many(count):
    steps = [["connected", "d1", "laptop"]]
    for index in range(count):
        steps.append(["switched", "laptop", f"model {index}"])
        if index % 7 == 0:
            steps.append(["tick", 0.5])
    steps.append(["disconnected", "d1", "laptop"])
    steps.append(["snapshot", None])
    steps.append(["snapshot", 3])
    return steps


CASES = {
    "empty_snapshot": [[], [0.0], [1000.5], [1763200000.25]],
    "script": [
        [60, _each(_HOST_MODE)],
        [60, _each(_CONNECTIONS)],
        [60, _each(_RESET)],
        [60, _each(_ROUNDING)],
        [60, _each(_REASONS)],
        [60, _SNAPSHOT_LIMITS],
        [60, _many(70)],
        [3, _each(_HOST_MODE)],
        [0, _each(_CONNECTIONS)],
    ],
}
