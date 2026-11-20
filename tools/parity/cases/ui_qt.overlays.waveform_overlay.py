"""Curated inputs for ui_qt/overlays/waveform_overlay.py's layout methods (fv_uimath
overlay_layout).

The methods run unbound on a stub overlay whose widths are the case's: nothing is measured, so no
font and no QApplication is needed. A case is one dict of what the overlay is doing:
  state, hands_free, destination, language, choices   the badge and the chip
  controls                                            None, or the left edge of Snap and Done
  badge_width, chip_width                             what the font measured
  dot, clock, live                                    the live label's parts, and a live look
  label_widths                                        [with the clock, without it]
  layout ("header" or "footer"), words, height, caption
"layout" reports the top row (in use, band top, span), the header height, the preview margins,
the text rect and divider, and the status rect, for the overlay at "height" with its header room
as "header_row" left it. "header_row" runs _draw_preview_header against a recording painter and
reports the label and the small look's room. "small_look" runs _draw_small_look and reports the
clip, the composed transform and the width the look saw. "max_height" and "reposition" run
_effective_streaming_max_height and _reposition_near_anchor on a screen and anchor. "controls"
(M6.4, fv_ui_overlay walkthrough_controls) lays out a walkthrough's Snap and Done, and is the one
function that measures text, on an offscreen QApplication.
"""
import types

FLOAT_TOLERANCE = 1e-12

_WIDTH = 300
_LISTENING = ("recording", "streaming", "command_listening")


class _Rect:
    def __init__(self, x, y, w, h):
        self._r = (x, y, w, h)

    def left(self):
        return self._r[0]

    def top(self):
        return self._r[1]

    def x(self):
        return self._r[0]

    def y(self):
        return self._r[1]

    def width(self):
        return self._r[2]

    def height(self):
        return self._r[3]


def _screen_rect(rect):
    from PyQt6.QtCore import QRect

    return None if rect is None else QRect(*rect)


def _stub(module, params):
    """The overlay's own methods on an object with just the state they read."""
    from PyQt6.QtCore import QRectF

    cls = module.WaveformOverlay
    state = params.get("state", "recording")
    stub = types.SimpleNamespace(
        current_state=state,
        LISTENING_STATES=cls.LISTENING_STATES,
        STATE_COMMAND_LISTENING=cls.STATE_COMMAND_LISTENING,
        STATE_STREAMING=cls.STATE_STREAMING,
        _hands_free=params.get("hands_free", False),
        _recording_destination=params.get("destination", ""),
        _language=params.get("language", ""),
        _language_choices=tuple(params.get("choices", ())),
        _label_parts=(params.get("dot", True), True, params.get("clock", True)),
        _visual=object() if params.get("live", True) else None,
        _streaming_layout=params.get("layout", "header"),
        _streaming_preview_text="words" if params.get("words", False) else "",
        _base_height=params.get("base_height", 80),
        _header_room=QRectF(*params.get("room", (0, 0, 0, 0))),
        _caption="caption" if params.get("caption", False) else "",
        _listen_started=0.0,
        _embedded=params.get("embedded", False),
        _preview=params.get("preview", False),
        _cornered=params.get("cornered", False),
        _anchor_pos=None,
        overlay_width=_WIDTH,
        overlay_height=params.get("height", 80),
        width=lambda: _WIDTH,
        height=lambda: params.get("height", 80),
    )
    controls = params.get("controls")
    stub._controls_clickable = lambda: controls is not None
    stub._badge_width = lambda: float(params.get("badge_width", 60))
    stub._chip_width = lambda: float(params.get("chip_width", 30))
    stub._layout_controls = lambda: {
        "snap": (QRectF(controls, 9, 40, 18), "Snap"),
        "finish": (QRectF(controls + 45, 9, 40, 18), "Done"),
    } if controls is not None else {}
    stub._live_tone = lambda: "danger"
    for name in ("_badge_shown", "_chip_visible", "_top_row_in_use", "_band_top", "_top_row_span",
                 "_live_shares_top_row", "_header_height", "_preview_text_margins",
                 "_status_rect", "_draw_preview_header", "_draw_small_look",
                 "_effective_streaming_max_height", "_reposition_near_anchor"):
        setattr(stub, name, types.MethodType(getattr(cls, name), stub))
    return stub


class _Patched:
    """Replaces module attributes for one call and puts them back."""

    def __init__(self, target, **values):
        self.target, self.values, self.saved = target, values, {}

    def __enter__(self):
        for name, value in self.values.items():
            self.saved[name] = getattr(self.target, name)
            setattr(self.target, name, value)

    def __exit__(self, *exc):
        for name, value in self.saved.items():
            setattr(self.target, name, value)


def _rect_list(rect):
    return [rect.x(), rect.y(), rect.width(), rect.height()]


def _layout(module, params):
    stub = _stub(module, params)
    widths = params.get("label_widths", [40.0, 12.0])
    with _Patched(module.live_recording,
                  wordless_label_width=lambda elapsed, *, dot, clock: widths[0] if clock else widths[1]):
        above, below = stub._preview_text_margins()
        height = params.get("height", 80)
        footer = params.get("layout", "header") == "footer"
        band = height - stub._base_height if footer else stub._band_top()
        divider = band if footer else stub._header_height()
        left, right = stub._top_row_span()
        status = stub._status_rect()
        words = params.get("words", False)
        return {
            "in_use": stub._top_row_in_use(),
            "band_top": stub._band_top(),
            "span": [left, right],
            "shares": stub._live_shares_top_row(),
            "header_height": stub._header_height(),
            "margins": [above, below],
            "text_rect": [module._TEXT_MARGIN, above, _WIDTH - 2 * module._TEXT_MARGIN,
                          max(20, height - above - below)],
            "divider": divider if words else None,
            "status": _rect_list(status),
        }


def _header_row(module, params):
    stub = _stub(module, params)
    widths = params.get("label_widths", [40.0, 12.0])
    drawn = []
    looks = []
    live = module.live_recording
    stub._draw_small_look = lambda painter, area: looks.append(_rect_list(area))
    with _Patched(live,
                  wordless_label_width=lambda elapsed, *, dot, clock: widths[0] if clock else widths[1],
                  draw_live_label=lambda painter, rect, *args, clock, **kwargs:
                  drawn.append([_rect_list(rect), clock])):
        stub._draw_preview_header(None, "overlay-text")
    room = stub._header_room
    return {"label": drawn[0] if drawn else None, "room": _rect_list(room),
            "look": looks[0] if looks else None}


class _TransformRecorder:
    def __init__(self):
        self.tx = self.ty = 0.0
        self.sx = self.sy = 1.0
        self.clip = None
        self.stack = []

    def save(self):
        self.stack.append((self.tx, self.ty, self.sx, self.sy))

    def restore(self):
        self.tx, self.ty, self.sx, self.sy = self.stack.pop()

    def setClipRect(self, rect):
        self.clip = _rect_list(rect)

    def translate(self, dx, dy):
        self.tx += self.sx * dx
        self.ty += self.sy * dy

    def scale(self, sx, sy):
        self.sx *= sx
        self.sy *= sy


def _small_look(module, small, area, live):
    from PyQt6.QtCore import QRectF

    stub = _stub(module, {"live": live})
    seen = []

    class _Look:
        band_width = None

        def draw(self, painter, grow):
            seen.append([painter.tx, painter.ty, painter.sx, painter.sy, self.band_width])

    look = _Look()
    look.small = small
    stub._visual = look if live else None
    stub._draw_particles = lambda painter: seen.append([painter.tx, painter.ty, painter.sx,
                                                        painter.sy, None])
    recorder = _TransformRecorder()
    stub._draw_small_look(recorder, QRectF(*area))
    return {"clip": recorder.clip, "draw": seen[0], "band_width_after": look.band_width}


def _screen(params):
    from PyQt6.QtCore import QPoint

    available = _screen_rect(params.get("available"))
    return available, QPoint(*params["anchor"]) if params.get("anchor") else None


def _max_height(module, params):
    from PyQt6.QtCore import QPoint

    stub = _stub(module, params)
    available = _screen_rect(params.get("available"))
    stub._max_height = params.get("configured_max", 400)
    stub._streaming_max_height = params.get("configured_max", 400)
    stub.parentWidget = lambda: types.SimpleNamespace(height=lambda: params.get("parent_height", 0))
    stub._placed = lambda: params.get("placed", False)
    stub.screen = lambda: (types.SimpleNamespace(availableGeometry=lambda: available)
                           if params.get("has_screen", True) else None)
    stub.y = lambda: params.get("y", 0)
    anchor = QPoint(*params["anchor"]) if params.get("anchor") else None
    stub._anchor_pos = anchor
    stub._available_geometry_for_anchor = lambda: available
    return stub._effective_streaming_max_height()


def _reposition(module, params):
    from PyQt6.QtCore import QPoint

    stub = _stub(module, params)
    available = _screen_rect(params.get("available"))
    moved = []
    stub.move = lambda *args: moved.append(list(args) if len(args) == 2 else [args[0].x(), args[0].y()])
    anchor = QPoint(*params["anchor"]) if params.get("anchor") else None
    stub._anchor_pos = anchor
    stub.overlay_height = params.get("height", 80)
    stub.height = lambda: params.get("height", 80)
    stub.parentWidget = lambda: types.SimpleNamespace(width=lambda: params.get("parent", [0, 0])[0],
                                                      height=lambda: params.get("parent", [0, 0])[1])
    stub._placed = lambda: params.get("placed", False)
    stub.screen = lambda: (types.SimpleNamespace(availableGeometry=lambda: available)
                           if params.get("has_screen", True) else None)
    stub.x = lambda: params.get("at", [0, 0])[0]
    stub.y = lambda: params.get("at", [0, 0])[1]
    stub._available_geometry_for_anchor = lambda: available
    stub._reposition_near_anchor()
    return moved[-1] if moved else None


_APP = []


def _app():
    # QFontMetrics needs a QGuiApplication. As render_reference.py: offscreen finds no fonts on
    # Windows without the font folder, and the font DPI is pinned.
    import os

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    os.environ.setdefault("QT_QPA_FONTDIR",
                          os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"))
    os.environ.setdefault("QT_FONT_DPI", "96")
    from PyQt6.QtWidgets import QApplication
    if not _APP:
        # Held for the process: a collected QApplication takes every widget down with it.
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


def _controls(module, params):
    """_layout_controls (Snap and Done along the badge row) on a stub with the row's other
    contents: "width" (the overlay's), "chip" and "chip_width", "badge" and "badge_width". The
    words are measured for real, in the controls' font. Returns the measured words ([Snap, Done])
    and the controls in Python's order as [key, [x, y, w, h], label]."""
    _app()
    from PyQt6.QtGui import QFont, QFontMetrics

    cls = module.WaveformOverlay
    width = params.get("width", _WIDTH)
    stub = types.SimpleNamespace(
        width=lambda: width,
        _chip_visible=lambda: params.get("chip", False),
        _chip_width=lambda: float(params.get("chip_width", 30)),
        _badge_shown=lambda: params.get("badge", False),
        _badge_width=lambda: float(params.get("badge_width", 60)),
        _control_width=cls._control_width,
    )
    stub._controls_width = types.MethodType(cls._controls_width, stub)
    stub._layout_controls = types.MethodType(cls._layout_controls, stub)
    metrics = QFontMetrics(QFont("Segoe UI", 8, QFont.Weight.DemiBold))
    return {
        "labels": [metrics.horizontalAdvance(label) for label in module._CONTROL_LABELS.values()],
        "controls": [[key, _rect_list(rect), label]
                     for key, (rect, label) in stub._layout_controls().items()],
    }


def _tables(module):
    return {name: getattr(module, name) for name in (
        "_BADGE_HEIGHT", "_BADGE_MARGIN", "_LOOK_HEIGHT", "_HEADER_HEIGHT", "_HEADER_LOOK_HEIGHT",
        "_HEADER_LOOK_MIN_WIDTH", "_TEXT_MARGIN", "_TOP_ROW", "_LIVE_ROW", "_PARK_MARGIN")}


WRAPPERS = {
    "layout": _layout,
    "header_row": _header_row,
    "small_look": _small_look,
    "max_height": _max_height,
    "reposition": _reposition,
    "tables": _tables,
    "controls": _controls,
}

_SCREEN = [0, 0, 1920, 1040]
_SMALL_SCREEN = [100, 50, 800, 480]
_BADGE = {"hands_free": True, "badge_width": 83}
_CHIP = {"language": "en", "choices": ["en", "es"], "chip_width": 30}
_CONTROLS = {"destination": "Walkthrough", "badge_width": 90, "controls": 150, "language": "en",
             "choices": ["en", "es"], "chip_width": 30}

_STATES = ("recording", "streaming", "command_listening", "processing", "copied", "idle")


def _layout_cases():
    cases = []
    for state in _STATES:
        for extra in ({}, _BADGE, _CHIP, {**_BADGE, **_CHIP}, _CONTROLS,
                      {"destination": "Walkthrough", "badge_width": 40}):
            for layout in ("header", "footer"):
                for words in (False, True):
                    for height in (80, 107, 140, 260):
                        cases.append([{**extra, "state": state, "layout": layout, "words": words,
                                       "height": height, "room": [90, 6, 170.5, 24]}])
    for dot, clock, live in ((True, True, True), (True, False, True), (False, True, True),
                             (False, False, True), (True, True, False)):
        for widths in ([40.0, 12.0], [30.0, 10.0], [200.0, 8.0]):
            for extra in (_BADGE, {**_BADGE, **_CHIP}, _CONTROLS,
                          {**_CONTROLS, "badge_width": 140}, {"controls": 20},
                          {"language": "en", "choices": ["en", "es"], "chip_width": 250}):
                cases.append([{**extra, "state": "streaming", "dot": dot, "clock": clock,
                               "live": live, "label_widths": widths, "layout": "header",
                               "words": True, "height": 150}])
    cases.append([{"state": "streaming", "layout": "footer", "words": True, "base_height": 80,
                   "height": 260, "hands_free": True, "badge_width": 83}])
    # A header room off the pixel grid: the status row is QRectF.toRect() of it, which rounds the
    # size after the corner (90.3 + 170.3 must not round to a 195 px row).
    for room in ([90.3, 6, 170.3, 24], [90.3, 6.25, 170.3, 23.6], [12.7, 6, 40.2, 24],
                 [101.45, 5.5, 60.6, 24], [97.6, 6.5, 121.4, 24]):
        cases.append([{"state": "streaming", "layout": "header", "words": True, "height": 150,
                       "room": room}])
    return cases


def _header_cases():
    cases = []
    for extra in ({}, _BADGE, {**_BADGE, **_CHIP}, _CONTROLS, {**_CONTROLS, "badge_width": 140},
                  {"controls": 20}, {"language": "en", "choices": ["en", "es"], "chip_width": 250}):
        for dot, clock, live in ((True, True, True), (True, False, True), (False, True, True),
                                 (False, False, True), (True, True, False)):
            for widths in ([40.0, 12.0], [30.0, 10.0], [200.0, 8.0], [90.0, 12.0]):
                for caption in (False, True):
                    cases.append([{**extra, "state": "streaming", "dot": dot, "clock": clock,
                                   "live": live, "label_widths": widths, "caption": caption}])
    return cases


def _small_cases():
    cases = []
    for small in ("squash", "shrink", "fit", "other"):
        for area in ([100, 6, 120.5, 24], [60, 6, 40, 24], [90.25, 34, 170, 24], [12, 6, 276, 24]):
            cases.append([small, area, True])
    for area in ([100, 6, 120.5, 24], [60, 6, 40, 24], [12, 6, 276, 24]):
        cases.append(["fit", area, False])
    return cases


def _screens():
    for available in (_SCREEN, _SMALL_SCREEN):
        yield available


def _max_height_cases():
    cases = []
    for available in (_SCREEN, _SMALL_SCREEN):
        for anchor in ([500, 400], [500, 20], [500, 1000], [available[0] + 5, available[1] + 5],
                       [available[0] + available[2] - 2, available[1] + available[3] - 2]):
            cases.append([{"available": available, "anchor": anchor}])
            cases.append([{"available": available, "anchor": anchor, "configured_max": 120}])
    cases += [
        [{"available": None, "anchor": [10, 10]}],
        [{"available": _SCREEN, "anchor": None}],
        [{"embedded": True, "parent_height": 900}],
        [{"embedded": True, "parent_height": 100}],
        [{"embedded": True, "parent_height": 5000, "configured_max": 400}],
        [{"embedded": True, "parent_height": 0}],
    ]
    for available in (_SCREEN, _SMALL_SCREEN):
        for y in (0, 300, 800, 1000, 1030):
            cases.append([{"placed": True, "available": available, "y": y}])
            cases.append([{"placed": True, "available": available, "y": y, "configured_max": 90}])
    cases.append([{"placed": True, "has_screen": False, "available": _SCREEN, "anchor": [400, 400]}])
    return cases


def _reposition_cases():
    cases = []
    for available in (_SCREEN, _SMALL_SCREEN):
        for anchor in ([500, 400], [available[0] + available[2] - 30, 500],
                       [500, available[1] + available[3] - 30], [0, 0], [-40, -40]):
            for height in (80, 107, 260, 400):
                cases.append([{"available": available, "anchor": anchor, "height": height}])
    cases += [
        [{"available": None, "anchor": [100, 200]}],
        [{"available": _SCREEN, "anchor": None}],
        [{"preview": True, "available": _SCREEN, "anchor": [100, 100]}],
        [{"embedded": True, "parent": [1000, 800], "height": 80}],
        [{"embedded": True, "parent": [1000, 800], "height": 400}],
        [{"embedded": True, "parent": [200, 100], "height": 80}],
    ]
    for available in (_SCREEN, _SMALL_SCREEN):
        for at in ([50, 50], [1800, 1000], [-30, -30], [available[0] + 700, available[1] + 450]):
            for height in (80, 260):
                cases.append([{"placed": True, "available": available, "at": at, "height": height}])
    cases += [
        [{"placed": True, "has_screen": False, "available": _SCREEN, "anchor": [400, 400],
          "at": [10, 10]}],
        [{"cornered": True, "available": _SCREEN, "anchor": [400, 400]}],
        [{"cornered": True, "available": _SMALL_SCREEN, "anchor": [400, 400]}],
        [{"cornered": True, "available": None, "anchor": [400, 400]}],
        [{"cornered": True, "available": None, "anchor": None}],
    ]
    return cases


def _controls_cases():
    cases = []
    for width in (300, 260, 340):
        for chip in ({}, {"chip": True, "chip_width": 30}, {"chip": True, "chip_width": 44}):
            for badge in ({}, {"badge": True, "badge_width": 40}, {"badge": True, "badge_width": 90},
                          {"badge": True, "badge_width": 118}, {"badge": True, "badge_width": 140},
                          {"badge": True, "badge_width": 180}, {"badge": True, "badge_width": 260}):
                cases.append([{"width": width, **chip, **badge}])
    # Either side of the fit: labelled Snap and Done need exactly the room between the badge and
    # the chip.
    for badge_width in (118.0, 119.0, 120.0, 120.5, 121.0, 121.25, 122.0, 123.0, 124.0):
        cases.append([{"chip": True, "chip_width": 30, "badge": True, "badge_width": badge_width}])
    return cases


CASES = {
    "layout": _layout_cases(),
    "header_row": _header_cases(),
    "small_look": _small_cases(),
    "max_height": _max_height_cases(),
    "reposition": _reposition_cases(),
    "tables": [[]],
    "controls": _controls_cases(),
}
