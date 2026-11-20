"""Curated inputs for meeting/clock.py (M7.8 meeting-capture).

MeetingClock reads time.monotonic() only; each case swaps the module's `time` for a scripted
monotonic value and runs a list of steps on one clock, collecting what the queries return:

    ["advance", seconds]          the monotonic clock moves
    ["start"] | ["pause"] | ["resume"] | ["resume_from_recovery", elapsed_s]
    ["now_s"] | ["meeting_time", t_mono] | ["paused_total_s"] | ["is_paused"] | ["is_running"]

The monotonic clock starts at 10000.0 (the value OpenWhisper's own test fixture uses). The
replay drives MeetingClock over a FakeClock whose steady time starts there.
"""

from types import SimpleNamespace

START_MONO = 10_000.0


def _script(module, steps):
    ticks = SimpleNamespace(value=START_MONO)
    saved = module.time
    module.time = SimpleNamespace(monotonic=lambda: ticks.value)
    try:
        clock = module.MeetingClock()
        out = []
        for step in steps:
            name, args = step[0], step[1:]
            if name == "advance":
                ticks.value += args[0]
            elif name in ("start", "pause", "resume"):
                getattr(clock, name)()
            elif name == "resume_from_recovery":
                clock.resume_from_recovery(args[0])
            elif name == "now_s":
                out.append(clock.now_s())
            elif name == "meeting_time":
                out.append(clock.meeting_time(args[0]))
            elif name == "paused_total_s":
                out.append(clock.paused_total_s())
            elif name in ("is_paused", "is_running"):
                out.append(getattr(clock, name))
            else:
                raise ValueError(f"unknown step {name}")
        return out
    finally:
        module.time = saved


CASES = {
    "script": [
        [[["now_s"], ["is_running"], ["is_paused"], ["meeting_time", 10_000.0],
          ["paused_total_s"]]],
        [[["start"], ["advance", 0.05], ["now_s"], ["is_running"], ["advance", 1.25], ["now_s"],
          ["meeting_time", 10_000.5], ["meeting_time", 9_000.0], ["meeting_time", 20_000.0]]],
        [[["start"], ["advance", 0.05], ["pause"], ["now_s"], ["advance", 0.08], ["now_s"],
          ["meeting_time", 10_000.13], ["meeting_time", 10_000.01], ["is_paused"],
          ["paused_total_s"]]],
        [[["start"], ["advance", 0.05], ["now_s"], ["pause"], ["advance", 0.1], ["resume"],
          ["now_s"], ["paused_total_s"], ["advance", 2.0], ["now_s"],
          ["meeting_time", 10_000.1]]],
        [[["start"], ["resume"], ["pause"], ["pause"], ["resume"], ["resume"], ["is_running"],
          ["is_paused"], ["paused_total_s"], ["now_s"]]],
        [[["resume_from_recovery", 120.0], ["now_s"], ["advance", 0.05], ["now_s"],
          ["is_running"], ["meeting_time", 9_870.0]]],
        [[["pause"], ["is_paused"], ["resume"], ["advance", 3.0], ["now_s"], ["start"],
          ["advance", 7.5], ["pause"], ["advance", 2.25], ["now_s"], ["resume"],
          ["advance", 0.125], ["pause"], ["advance", 4.0], ["paused_total_s"], ["resume"],
          ["now_s"], ["paused_total_s"], ["meeting_time", 10_005.0]]],
        [[["start"], ["advance", 30.0], ["pause"], ["advance", 5.0],
          ["resume_from_recovery", 12.5], ["is_paused"], ["paused_total_s"], ["now_s"],
          ["advance", 1.0], ["now_s"], ["start"], ["now_s"], ["advance", 0.3], ["now_s"]]],
        [[["start"], ["advance", 3600.0], ["pause"], ["advance", 0.000001], ["resume"],
          ["advance", 0.1], ["now_s"], ["paused_total_s"]]],
    ],
}

WRAPPERS = {"script": _script}

# The replay's steady clock counts integer nanoseconds where Python sums float seconds.
FLOAT_TOLERANCE = 1e-9

PINS = {
    "monotonic_start": START_MONO,
    "note": "each case is one MeetingClock driven by scripted monotonic time; started_at_iso is "
            "wall time and is not recorded",
}
