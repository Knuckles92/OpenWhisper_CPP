"""Curated scripts for ui_qt/widgets/walkthrough_panel.py's Qt-free parts (fv_ui_walkthrough's
walkthrough_column.h).

    _column(ops)

runs a fresh WalkthroughColumn through `ops`, each a list:

    ["hear", level]                   hear
    ["advance", dt, now]              advance
    ["speak", level, seconds, now]    hear then advance(TICK_S, now), round(seconds / TICK_S) times
    ["add", number, now, kept_at_s]   add_shot (the image is a stand-in: the column only keeps it)
    ["hold", on, at_s]                hold_note
    ["move", number, step]            move_shot; True or False
    ["remove", number]                remove_shot; True or False
    ["place", number]                 place

The result is {"ops": [each op's result, None for those that return nothing], "items": every
item as {"kind": "words" | "note", "ticks", "born", "started_s", "ended_s"} or {"kind": "shot",
"number", "born", "kept_at_s"}, "sections": the item kinds per section, "shots", "holding",
"speech_s"}.

    _rows_of(count, width)  _rows of a paragraph of `count` ticks
"""

FLOAT_TOLERANCE = 1e-12


def _item(module, item):
    if isinstance(item, module.ShotItem):
        return {"kind": "shot", "number": item.number, "born": item.born,
                "kept_at_s": item.kept_at_s}
    return {"kind": "note" if item.note else "words", "ticks": list(item.ticks),
            "born": list(item.born), "started_s": item.started_s, "ended_s": item.ended_s}


def _kind(module, item):
    if isinstance(item, module.ShotItem):
        return f"shot {item.number}"
    return "note" if item.note else "words"


def _column(module, ops):
    column = module.WalkthroughColumn()
    results = []
    for op in ops:
        name = op[0]
        if name == "hear":
            results.append(column.hear(op[1]))
        elif name == "advance":
            results.append(column.advance(op[1], op[2]))
        elif name == "speak":
            for _ in range(round(op[2] / module.TICK_S)):
                column.hear(op[1])
                column.advance(module.TICK_S, op[3])
            results.append(None)
        elif name == "add":
            results.append(column.add_shot(op[1], None, op[2], op[3]))
        elif name == "hold":
            results.append(column.hold_note(op[1], op[2]))
        elif name == "move":
            results.append(column.move_shot(op[1], op[2]))
        elif name == "remove":
            results.append(column.remove_shot(op[1]))
        elif name == "place":
            results.append(column.place(op[1]))
        else:
            raise ValueError(name)
    return {
        "ops": results,
        "items": [_item(module, item) for item in column.items],
        "sections": [[_kind(module, item) for item in section] for section in column.sections()],
        "shots": column.shots,
        "holding": column.holding,
        "speech_s": column.speech_s,
    }


def _rows_of(module, count, width):
    return module._rows(module.Paragraph(ticks=[0.5] * count), width)


WRAPPERS = {"_column": _column, "_rows_of": _rows_of}

LOUD = 0.05
ROOM = 0.0005
TICK = 0.075


def _board():
    """Words (4 ticks), shot 1, words (8), shot 2, then words being spoken (2)."""
    return [["speak", LOUD, 4 * TICK, 0.0], ["add", 1, 0.0, 2.0],
            ["speak", LOUD, 8 * TICK, 0.0], ["add", 2, 0.0, 4.0],
            ["speak", LOUD, 2 * TICK, 0.0]]


CASES = {
    "_column": [
        # OpenWhisper's own scripts (tests/test_walkthrough_panel.py).
        [[["speak", ROOM, 1.5, 0.0], ["speak", LOUD, 1.5, 0.0]]],
        [[["speak", 0.01, 0.3, 0.0], ["speak", 0.1, 0.3, 0.0]]],
        [[["speak", LOUD, 0.5, 0.0], ["add", 1, 1.0, 0.0]]],
        [[["add", 1, 0.0, 0.0], ["add", 2, 0.1, 0.0]]],
        [_board() + [["place", 1], ["place", 2], ["place", 9]]],
        [_board() + [["move", 2, -1], ["place", 2], ["move", 1, 1], ["move", 2, -1]]],
        [_board() + [["remove", 1], ["remove", 2], ["speak", LOUD, TICK, 0.0], ["remove", 2]]],
        [[["speak", LOUD, 2 * TICK, 0.0], ["hold", True, 1.0], ["speak", LOUD, 3 * TICK, 0.0],
          ["hold", False, 2.0], ["speak", LOUD, TICK, 0.0]]],
        [[["hold", True, 1.0], ["hold", False, 1.5]]],
        [[["hold", True, 1.0], ["speak", LOUD, 2 * TICK, 0.0], ["add", 1, 0.0, 3.0],
          ["speak", LOUD, TICK, 0.0]]],
        [[["speak", LOUD, 2 * TICK, 0.0], ["add", 1, 0.0, 1.0], ["hold", True, 1.0],
          ["speak", LOUD, TICK, 0.0], ["hold", False, 2.0], ["remove", 1]]],
        # Levels and timing: the carry across uneven frames, the gate, the loudest of a window.
        [[["hear", 0.001], ["hear", 0.2], ["hear", 0.003], ["advance", 0.016, 1.0],
          ["advance", 0.016, 1.016], ["advance", 0.016, 1.032], ["advance", 0.016, 1.048],
          ["advance", 0.016, 1.064], ["hear", 0.5], ["advance", 0.1, 1.164],
          ["advance", 0.2, 1.364]]],
        [[["hear", 0.00125], ["advance", 0.075, 0.0], ["hear", 0.0013], ["advance", 0.075, 0.1],
          ["hear", 1.0], ["advance", 0.075, 0.2], ["hear", 2.0], ["advance", 0.075, 0.3],
          ["hear", 0.0], ["advance", 0.075, 0.4], ["hear", -1.0], ["advance", 0.075, 0.5]]],
        [[["hear", 0.05], ["advance", 0.3, 5.0], ["hear", 0.05], ["advance", 0.0749, 5.1],
          ["advance", 0.0002, 5.2]]],
        # Hold for note: pressed twice, let go twice, around screenshots, moved and removed.
        [[["hold", True, 0.5], ["hold", True, 0.6], ["speak", LOUD, TICK, 0.0],
          ["hold", False, 1.0], ["hold", False, 1.1]]],
        [[["hold", False, 0.5], ["speak", LOUD, TICK, 0.0]]],
        [[["hold", True, 1.0], ["add", 1, 0.0, 2.0], ["hold", False, 3.0]]],
        [[["hold", True, 1.0], ["speak", LOUD, TICK, 0.0], ["add", 1, 0.0, 2.0],
          ["add", 2, 0.0, 2.5], ["speak", LOUD, TICK, 0.0], ["hold", False, 3.0],
          ["move", 2, -1], ["remove", 1]]],
        [[["speak", LOUD, TICK, 0.0], ["add", 1, 0.0, 1.0], ["speak", LOUD, 2 * TICK, 0.0],
          ["hold", True, 2.0], ["speak", LOUD, TICK, 0.0], ["hold", False, 3.0],
          ["add", 2, 0.0, 4.0], ["speak", LOUD, 3 * TICK, 0.0], ["move", 2, -1],
          ["move", 1, -1], ["remove", 2]]],
        # Moves at the edges, unknown numbers, and a board of five.
        [[["add", 1, 0.0, 1.0], ["move", 1, -1], ["move", 1, 1], ["move", 7, 1],
          ["remove", 7]]],
        [[["speak", LOUD, TICK, 0.0], ["add", 1, 0.0, 1.0], ["speak", LOUD, 2 * TICK, 0.0],
          ["add", 2, 0.0, 2.0], ["speak", LOUD, 3 * TICK, 0.0], ["add", 3, 0.0, 3.0],
          ["add", 4, 0.0, 4.0], ["speak", LOUD, 5 * TICK, 0.0], ["add", 5, 0.0, 5.0],
          ["move", 1, 2], ["move", 5, -4], ["move", 3, -1], ["remove", 4], ["remove", 2],
          ["place", 5], ["place", 3], ["place", 1]]],
        [[["speak", LOUD, TICK, 0.0], ["add", 1, 0.0, 1.0], ["speak", LOUD, 2 * TICK, 0.0],
          ["add", 2, 0.0, 2.0], ["remove", 2], ["remove", 1], ["speak", LOUD, TICK, 0.0]]],
    ],
    "ease_out": [[t] for t in (-1.0, 0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0, 1.5, 0.3333)],
    "ease_out_back": [[t] for t in (-1.0, 0.0, 0.1, 0.25, 0.5, 0.6, 0.75, 0.9, 1.0, 2.0)],
    "_per_row": [[w] for w in (-10.0, 0.0, 1.0, 2.0, 2.5, 6.0, 9.9, 10.0, 172.0, 176.0, 236.0,
                               252.0, 1e6)],
    "_rows_of": [[0, 172.0], [1, 172.0], [43, 172.0], [44, 172.0], [87, 172.0], [88, 172.0],
                 [89, 172.0], [5, 0.0], [62, 236.0], [63, 236.0], [200, 252.0]],
}
