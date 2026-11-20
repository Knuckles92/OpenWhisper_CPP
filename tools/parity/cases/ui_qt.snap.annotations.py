"""Curated inputs for ui_qt/snap/annotations.py (fv_ui_snap annotations). Marks are described as
[kind, ...]: ["arrow", start, end, width], ["box" | "ellipse", start, end, width],
["redaction", start, end], ["stroke", points, highlighter], ["step", center, number],
["label", text] (meaningful only: its rect needs fonts). Points are [x, y]; rectangles come back
as [x, y, width, height] floats. Labels and steps are built without the application font, which
nothing here reads."""

CORAL = "#FF4F3A"


def _point(values):
    from PyQt6.QtCore import QPointF

    return QPointF(*values)


def _xy(point):
    return [point.x(), point.y()]


def _rect_list(rect):
    return [rect.x(), rect.y(), rect.width(), rect.height()]


def _mark(module, spec):
    from PyQt6.QtGui import QColor, QFont

    kind, *rest = spec
    color = QColor(CORAL)
    if kind == "arrow":
        return module.Arrow(_point(rest[0]), _point(rest[1]), color, rest[2])
    if kind == "box":
        return module.Box(_point(rest[0]), _point(rest[1]), color, rest[2])
    if kind == "ellipse":
        return module.Ellipse(_point(rest[0]), _point(rest[1]), color, rest[2])
    if kind == "redaction":
        return module.Redaction(_point(rest[0]), _point(rest[1]))
    if kind == "stroke":
        return module.Stroke([_point(p) for p in rest[0]], color, highlighter=rest[1])
    if kind == "step":
        return module.Step(_point(rest[0]), rest[1], color)
    if kind == "label":
        return module.Label(_point([0, 0]), rest[0], color, QFont())
    raise ValueError(kind)


def _bounds(module, *spec):
    return _rect_list(_mark(module, spec).bounds())


def _meaningful(module, *spec):
    return _mark(module, spec).meaningful()


def _extend_draft(module, tool, start, moves):
    """A draft of tool at start, extended by [point, constrained, bounds or None] moves; the
    result is the drag's end, or a stroke's points."""
    from PyQt6.QtCore import QRectF
    from PyQt6.QtGui import QColor

    draft = module.new_draft(module.Tool(tool), _point(start), QColor(CORAL))
    for point, constrained, bounds in moves:
        module.extend_draft(draft, _point(point), constrained=constrained,
                            bounds=None if bounds is None else QRectF(*bounds))
    if isinstance(draft, module.Stroke):
        return [_xy(p) for p in draft.points]
    return _xy(draft.end)


def _elements(path):
    return [[path.elementAt(i).x, path.elementAt(i).y, path.elementAt(i).type.value]
            for i in range(path.elementCount())]


def _arrow_shapes(module, start, end, width):
    shaft, head = module.arrow_shapes(_point(start), _point(end), width)
    return {"shaft": _elements(shaft), "head": [_xy(head[i]) for i in range(head.size())],
            "current": _xy(shaft.currentPosition())}


def _smooth_path(module, points):
    return _elements(module.smooth_path([_point(p) for p in points]))


def _ink_for(module, color):
    from PyQt6.QtGui import QColor

    return module.ink_for(QColor(color)).name()


def _next_step_number(module, operations):
    """next_step_number after each operation: "step" adds a step with that number, "mark" an
    arrow, "undo" and "redo" walk the history."""
    from PyQt6.QtGui import QColor

    stack = module.AnnotationStack()
    numbers = []
    for operation in operations:
        if operation == "step":
            stack.add(module.Step(_point([10, 10]), stack.next_step_number(), QColor(CORAL)))
        elif operation == "mark":
            stack.add(module.Arrow(_point([0, 0]), _point([50, 0]), QColor(CORAL)))
        elif operation == "undo":
            stack.undo()
        elif operation == "redo":
            stack.redo()
        numbers.append(stack.next_step_number())
    return numbers


WRAPPERS = {
    "bounds": _bounds,
    "meaningful": _meaningful,
    "extend_draft": _extend_draft,
    "arrow_shapes": _arrow_shapes,
    "smooth_path": _smooth_path,
    "ink_for": _ink_for,
    "next_step_number": _next_step_number,
}

ARROWS = [
    [[0, 100], [200, 100]], [[200, 100], [0, 100]], [[100, 0], [100, 200]],
    [[100, 200], [100, 0]], [[40, 120], [200, 60]], [[200, 60], [40, 140]],
    [[5, 5], [5, 5]], [[0, 0], [3, 2]], [[0, 0], [6, 0]], [[10, 10], [14, 13]],
    [[0, 0], [30, 0]], [[0, 0], [0.5, 0.5]], [[300, 300], [100, 280]], [[0, 0], [100, 100]],
    [[0, 0], [-100, 100]], [[0, 0], [100, -100]], [[0, 0], [-100, -100]], [[0, 0], [100, 20]],
    [[0, 0], [100, -20]], [[0, 0], [20, 100]], [[0, 0], [-20, 100]], [[0, 0], [98, 20]],
    [[0, 0], [1000, 30]], [[12.5, 7.25], [13.5, 7.25]],
]
STROKES = [
    [[30, 100]], [[30, 100], [160, 104]], [[30, 100], [60, 40], [90, 120], [140, 50]],
    [[0, 0], [10, 0], [10, 10], [0, 10], [0, 0]], [[1.5, 2.5], [3.25, 4.75], [6.0, 2.0]],
]
BOX_SPANS = [
    [[30, 30], [120, 90]], [[120, 90], [30, 30]], [[0, 0], [3, 3]], [[0, 0], [2.9, 40]],
    [[0, 0], [40, 2]], [[0, 0], [4, 4.5]], [[0, 0], [4.25, 4.25]], [[10.5, 20.25], [80.75, 50.5]],
    [[5, 5], [5, 5]], [[0, 0], [3, 5.2]],
]

CASES = {
    "bounds": (
        [["arrow", start, end, 4.0] for start, end in ARROWS]
        + [["arrow", [3.5, 16.5], [16.5, 3.5], 1.8]]
        + [[kind, start, end, 3.0] for kind in ("box", "ellipse") for start, end in BOX_SPANS]
        + [["redaction", start, end] for start, end in BOX_SPANS]
        + [["redaction", [80.75, 50.75], [30.25, 20.25]]]
        + [["stroke", points, marker] for points in STROKES for marker in (False, True)]
        + [["step", [100, 100], 7], ["step", [12.5, 3.25], 100]]
    ),
    "meaningful": (
        [["arrow", start, end, 4.0] for start, end in ARROWS]
        + [[kind, start, end, 3.0] for kind in ("box", "ellipse") for start, end in BOX_SPANS]
        + [["redaction", start, end] for start, end in BOX_SPANS]
        + [["stroke", points, False] for points in STROKES]
        + [["step", [1, 1], 1]]
        + [["label", text] for text in ("", "   ", "Click here", " x ", "\t\n", " ")]
    ),
    "extend_draft": [
        [tool, [100, 100], [[end, True, None]]]
        for tool in ("box", "ellipse")
        for end in ([160, 125], [40, 125], [160, 75], [40, 75], [100, 160], [130, 100])
    ] + [
        [tool, [100, 100], [[end, True, None], [end, False, None]]]
        for tool in ("box", "ellipse") for end in ([160, 125], [40, 75])
    ] + [
        [tool, [110, 100], [[[160, 150], True, [0, 0, 120, 120]]]]
        for tool in ("box", "ellipse", "arrow")
    ] + [
        ["arrow", [100, 100], [[end, True, None]]]
        for end in ([150, 110], [110, 150], [150, 140], [50, 90], [90, 50], [60, 140],
                    [150, 100], [100, 100], [150, 121], [150, 120.7106781], [101, 300])
    ] + [
        ["arrow", [10, 10], [[[200, 300], True, [0, 0, 120, 120]]]],
        ["arrow", [10, 10], [[[-50, 30], True, [0, 0, 120, 120]]]],
        ["box", [10, 10], [[[-50, 30], True, [5, 5, 120, 120]]]],
        ["box", [10, 10], [[[-50, -80], True, [5, 5, 120, 120]]]],
        ["redact", [10, 10], [[[50, 30], True, [0, 0, 20, 20]]]],
        ["pen", [0, 0], [[[0.5, 0.5], False, None], [[10, 0], False, None],
                         [[11.4, 0], False, None], [[11.5, 0], True, None]]],
        ["highlighter", [0, 0], [[[1.06, 1.06], False, None], [[1.07, 1.07], False, None]]],
    ],
    "arrow_shapes": [[start, end, 4.0] for start, end in ARROWS] + [
        [[3.5, 16.5], [16.5, 3.5], 1.8], [[0, 0], [100, 0], 18.0], [[0, 0], [8, 0], 4.0],
    ],
    "smooth_path": [[points] for points in STROKES]
    + [[[[3, 13], [5.5, 7.5], [8.5, 12.5], [11.5, 6.5], [14, 12], [17, 6.5]]]],
    "ink_for": [
        ["#FF4F3A"], ["#F5A524"], ["#3A7BFD"], ["#1FA463"], ["#000000"], ["#FFFFFF"],
        ["#808080"], ["#959595"], ["#969696"], ["#9A9A9A"], ["#A0A0A0"], ["#C0C0C0"],
        ["#181A20"], ["#FFFF00"], ["#00FF00"], ["#0A0A0A"], ["#0B0B0B"],
    ],
    "next_step_number": [
        [["step", "mark", "step"]],
        [["step", "mark", "step", "undo", "redo"]],
        [["undo", "redo", "step", "step", "undo", "undo", "undo", "redo", "step", "redo"]],
        [["mark", "mark", "undo", "step", "redo"]],
    ],
}
