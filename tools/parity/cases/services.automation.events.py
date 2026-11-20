"""Curated scripts for services/automation/events.py (M5.3 automation-core).

    run(buffer_size, seq_base, steps)   a fresh EventBus(buffer_size=..., seq_base=...), then
                                        each step in turn; the list of what each step answered

Steps:
    ["publish", type, data, replay]     the event as [seq, type, v, data], or "TypeError"
    ["publish_lone", type, where]       publishes data holding a lone surrogate, which a vector
                                        file cannot hold: as a value ({"text": "hi " + U+D83D}),
                                        a key ({U+DC00: "key"}) or a list item ({"list": ["ok",
                                        U+DC00]}); "published" or "TypeError"
    ["publish_many", type, count]       publishes count events {"n": i}; answers the last seq
    ["clear"]                           answers None
    ["since", seq, types]               {"events": [seq, ...], "gap": reason or None}
    ["recent", limit, types]            [seq, ...]
    ["last_seq"]                        the last number given out

Event ids and times are random and wall-clock, so the vectors leave them out; the C++ replay
checks their shape. types None follows every type.
"""

_LONE = {
    "value": lambda: {"text": "hi " + chr(0xD83D)},
    "key": lambda: {chr(0xDC00): "key"},
    "list": lambda: {"list": ["ok", chr(0xDC00)]},
}


def _publish(bus, out, event_type, data, replay=True):
    try:
        event = bus.publish(event_type, data, replay=replay)
    except TypeError:
        out.append("TypeError")
        return None
    return event


def _run(module, buffer_size, seq_base, steps):
    bus = module.EventBus(buffer_size=buffer_size, seq_base=seq_base)
    out = []
    try:
        for step in steps:
            op = step[0]
            if op == "publish":
                event = _publish(bus, out, step[1], step[2], step[3])
                if event is not None:
                    data = event.to_dict()
                    out.append([data["seq"], data["type"], data["v"], data["data"]])
            elif op == "publish_lone":
                if _publish(bus, out, step[1], _LONE[step[2]]()) is not None:
                    out.append("published")
            elif op == "publish_many":
                last = None
                for n in range(step[2]):
                    last = bus.publish(step[1], {"n": n}).seq
                out.append(last)
            elif op == "clear":
                bus.clear()
                out.append(None)
            elif op == "since":
                events, gap = bus.since(step[1], types=step[2])
                out.append({"events": [e.seq for e in events], "gap": gap})
            elif op == "recent":
                out.append([e.seq for e in bus.recent(step[1], types=step[2])])
            elif op == "last_seq":
                out.append(bus.last_seq())
            else:
                raise ValueError(op)
    finally:
        bus.close()
    return out


WRAPPERS = {"run": _run}

_A, _B, _C = "recording.started", "channel.message", "meeting.segments"

CASES = {
    "run": [
        # Numbering, replay and the live-only events.
        [500, 0, [["last_seq"], ["since", 0, None], ["recent", 50, None],
                  ["publish", _A, {"x": 1}, True], ["publish", _A, None, True],
                  ["publish", "transcription.preview", {"text": "hel"}, False],
                  ["publish", _B, {"text": "café \U0001f600"}, True],
                  ["last_seq"], ["since", 0, None], ["since", 1, None], ["since", 3, None],
                  ["since", 4, None], ["since", 5, None], ["since", 900, None],
                  ["since", -1, None], ["recent", 2, None], ["recent", 0, None],
                  ["recent", -3, None], ["recent", 50, [_B]], ["recent", 50, []],
                  ["recent", 50, ["", _A]], ["since", 0, []], ["since", 0, [_B, "nope"]]]],
        # Text that does not encode as UTF-8 is refused before it is numbered.
        [500, 0, [["publish_lone", _B, "value"], ["publish_lone", _B, "key"],
                  ["publish_lone", _B, "list"], ["last_seq"],
                  ["publish", _B, {"text": "hi \U0001f600"}, True], ["recent", 50, None]]],
        # Eviction past the default 500.
        [500, 0, [["publish_many", _A, 505], ["since", 0, None], ["since", 4, None],
                  ["since", 5, None], ["since", 6, None], ["recent", 3, None],
                  ["since", 504, [_A]], ["since", 4, [_B]], ["last_seq"]]],
        # A small buffer, per-type gaps.
        [3, 0, [["publish_many", _A, 5], ["since", 3, None], ["since", 2, None],
                ["since", 1, None], ["since", 5, None], ["since", 6, None]]],
        [5, 0, [["publish", _B, {"text": "seen"}, True], ["publish_many", _C, 6],
                ["since", 1, [_B]], ["since", 1, [_C]], ["since", 1, None],
                ["since", 0, [_B]], ["since", 0, None]]],
        [5, 0, [["publish", _B, {"text": "first"}, True],
                ["publish", _B, {"text": "second"}, True], ["publish_many", _C, 5],
                ["since", 1, [_B]], ["since", 2, [_B]], ["since", 7, [_B]]]],
        # Clear: the buffer goes, numbering goes on, per-type gaps remain.
        [500, 0, [["publish", _B, {"text": "seen"}, True],
                  ["publish", "history.saved", {"record_id": "1"}, True], ["clear"],
                  ["recent", 50, None], ["since", 1, [_B]], ["since", 1, ["history.saved"]],
                  ["since", 2, None], ["since", 0, None], ["since", 0, [_A]],
                  ["publish", _B, {"text": "unseen"}, True], ["clear"], ["since", 1, [_B]],
                  ["since", 3, [_B]], ["last_seq"], ["publish", _A, {}, True],
                  ["since", 3, None], ["since", 3, [_A]]]],
        # A run numbering from a base: positions from another run.
        [500, 1000, [["last_seq"], ["since", 0, None], ["since", 1000, None],
                     ["publish_many", _A, 3], ["since", 0, None], ["since", 999, None],
                     ["since", 1000, None], ["since", 1001, None], ["since", 1003, None],
                     ["since", 1004, None], ["since", 5, None], ["since", 2000, None]]],
        [3, 2000, [["publish_many", _C, 5], ["publish", _B, {"text": "hello"}, True],
                   ["since", 0, [_B]], ["since", 0, None], ["since", 1003, [_B]],
                   ["since", 1003, None], ["since", 2003, [_B]], ["since", 2003, None]]],
        # Buffer sizes below one keep one event; a negative base starts at zero.
        [0, -5, [["publish_many", _A, 2], ["recent", 10, None], ["since", 0, None],
                 ["since", 1, None], ["last_seq"]]],
    ],
}
