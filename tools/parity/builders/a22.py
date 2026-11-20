"""Reference builders of the a22 Snap pieces (fv_ui_snap: M6.3, then M6.8 and M6.10).

snap_glyphs is a test canvas, not an OpenWhisper widget: it paints through OpenWhisper's own
functions every toolbar glyph at 20 px in the overlay text colour and at 40 px in the accent,
the four swatch rings, the hint pill and the size readout, and one of each mark on a mid-grey
480 x 320 image with the selection frame, so the C++ port of icons.py, chrome.py and
annotations.py is compared pixel for pixel. Nothing here runs an event loop.
"""

#: The fourteen distinct glyphs; "region" draws the "snap" glyph.
GLYPHS = ("arrow", "box", "ellipse", "redact", "pen", "highlighter", "text", "step", "undo",
          "redo", "keep", "drop", "snap", "clear")
CANVAS = (664, 482)
IMAGE = (480, 320)
IMAGE_AT = (12, 150)
HINT = "Drag to select"
BADGE = "1280 × 720"


def _marks():
    from PyQt6.QtCore import QPointF
    from PyQt6.QtGui import QColor

    from ui_qt.snap import annotations as a

    coral, amber, blue, green = (QColor(s.color) for s in a.SWATCHES)
    return [
        a.Arrow(QPointF(30, 120), QPointF(150, 40), coral),
        a.Box(QPointF(180, 30), QPointF(280, 100), amber, infill=True),
        a.Ellipse(QPointF(300, 30), QPointF(440, 110), blue),
        a.Stroke([QPointF(30, 160), QPointF(60, 140), QPointF(90, 180), QPointF(120, 150),
                  QPointF(150, 170)], green),
        a.Stroke([QPointF(180, 150), QPointF(300, 155)], amber, highlighter=True),
        a.Label(QPointF(30, 220), "Click here first", coral),
        a.Step(QPointF(200, 250), 3, blue),
        a.Step(QPointF(250, 250), 1, amber),
        a.Redaction(QPointF(300, 200), QPointF(360, 260)),
    ]


def snap_glyphs(variant):
    from PyQt6.QtCore import QPoint, QPointF, QRect, QRectF, Qt
    from PyQt6.QtGui import QColor, QGuiApplication, QImage, QPainter, QPen
    from PyQt6.QtWidgets import QWidget

    from ui_qt.snap import annotations, chrome, icons
    from ui_qt.utils.palette import current_palette

    class SnapGlyphs(QWidget):
        def paintEvent(self, event):
            palette = current_palette()
            font = QGuiApplication.font()
            painter = QPainter(self)
            background = palette.color("overlay-bg")
            background.setAlpha(255)
            painter.fillRect(QRect(0, 0, *CANVAS), background)
            text = palette.color("overlay-text")
            accent = palette.color("accent")
            for index, name in enumerate(GLYPHS):
                icons.paint_icon(painter, name, QRectF(12 + index * 28, 12, 20, 20), text)
                icons.paint_icon(painter, name, QRectF(12 + index * 46, 44, 40, 40), accent)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            for index, swatch in enumerate(annotations.SWATCHES):
                center = QPointF(30 + index * 32, 112)
                painter.setPen(QPen(text, 2))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawEllipse(center, 11, 11)
                painter.setPen(QPen(QColor(0, 0, 0, 60), 1))
                painter.setBrush(QColor(swatch.color))
                painter.drawEllipse(center, 8, 8)
            pill = chrome.HintPill(HINT, font, palette)
            pill.paint(painter, QRect(QPoint(150, 96), pill.size()))
            badge_font = chrome.chrome_font(font, 12)
            badge = QRect(QPoint(504, 160), chrome.badge_size(BADGE, badge_font))
            chrome.paint_badge(painter, badge, BADGE, badge_font, palette)
            image = QImage(*IMAGE, QImage.Format.Format_ARGB32_Premultiplied)
            image.fill(QColor(128, 128, 128))
            marks = QPainter(image)
            marks.setRenderHint(QPainter.RenderHint.Antialiasing)
            marks.setRenderHint(QPainter.RenderHint.TextAntialiasing)
            for mark in _marks():
                mark.paint(marks)
            chrome.paint_frame(marks, QRect(380, 200, 80, 90))
            marks.end()
            painter.drawImage(QPoint(*IMAGE_AT), image)
            painter.end()

    return SnapGlyphs(), CANVAS, None


#: snap_toolbar: both variants on one mid-grey canvas, each 12 px from the edges and each other.
TOOLBAR_MARGIN = 12
#: snap_overlay: the frozen screen of both phases, one above the other.
SNAP_IMAGE = (960, 600)
OVERLAY_HINT = "S toggles snapping"


def snap_toolbar(variant):
    """The overlay toolbar (Arrow active, Coral, undo enabled, infill off) above the live one
    (no tool, Amber, snapping and Keep marks on, nothing to put down) with its delay-chip slot
    left empty: the chip is live drawing's."""
    from PyQt6 import sip
    from PyQt6.QtGui import QColor, QPainter
    from PyQt6.QtWidgets import QWidget

    from ui_qt.snap.annotations import Tool
    from ui_qt.snap.toolbar import SnapToolbar
    from ui_qt.utils.palette import current_palette

    class ToolbarCanvas(QWidget):
        def paintEvent(self, event):
            painter = QPainter(self)
            painter.fillRect(self.rect(), QColor(96, 96, 96))
            painter.end()

    canvas = ToolbarCanvas()
    overlay = SnapToolbar(canvas, current_palette())
    overlay.set_state(Tool.ARROW, 0, True, False)
    overlay.set_box_infill(False)
    live = SnapToolbar(canvas, current_palette(), live=True)
    sip.delete(live.capture_delay)
    live.capture_delay = None
    live.set_state(None, 1, False, False)
    live.set_snap_enabled(True)
    live.set_keep_marks(True)
    live.set_drop_enabled(False)
    overlay.move(TOOLBAR_MARGIN, TOOLBAR_MARGIN)
    live.move(TOOLBAR_MARGIN, 2 * TOOLBAR_MARGIN + overlay.height())
    width = max(overlay.width(), live.width()) + 2 * TOOLBAR_MARGIN
    height = overlay.height() + live.height() + 3 * TOOLBAR_MARGIN
    return canvas, (width, height), None


def _frozen_screen():
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QColor, QImage, QLinearGradient, QPainter

    image = QImage(*SNAP_IMAGE, QImage.Format.Format_RGB32)
    painter = QPainter(image)
    gradient = QLinearGradient(0, 0, *SNAP_IMAGE)
    gradient.setColorAt(0.0, QColor("#2d5b8a"))
    gradient.setColorAt(0.55, QColor("#d9c7a8"))
    gradient.setColorAt(1.0, QColor("#3c5a3c"))
    painter.fillRect(image.rect(), gradient)
    painter.fillRect(QRect(80, 60, 360, 240), QColor("#f4efe9"))
    painter.fillRect(QRect(520, 300, 320, 200), QColor("#1f2933"))
    painter.end()
    return image


def _mouse(widget, kind, point, held):
    from PyQt6.QtCore import QEvent, QPointF, Qt
    from PyQt6.QtGui import QMouseEvent
    from PyQt6.QtWidgets import QApplication

    at = QPointF(*point)
    button = Qt.MouseButton.NoButton if kind == QEvent.Type.MouseMove else Qt.MouseButton.LeftButton
    buttons = Qt.MouseButton.LeftButton if held else Qt.MouseButton.NoButton
    QApplication.sendEvent(widget, QMouseEvent(kind, at, at, button, buttons,
                                               Qt.KeyboardModifier.NoModifier))


def _drag(widget, points):
    from PyQt6.QtCore import QEvent

    _mouse(widget, QEvent.Type.MouseButtonPress, points[0], True)
    for point in points[1:]:
        _mouse(widget, QEvent.Type.MouseMove, point, True)
    _mouse(widget, QEvent.Type.MouseButtonRelease, points[-1], False)


def snap_overlay(variant):
    """Two overlays on the same frozen screen, the select phase above the annotate phase. Above,
    a selection being dragged shows its size; below, a kept selection carries one mark of every
    tool and the toolbar. Both are children of one canvas here, so one image holds both."""
    from PyQt6.QtCore import QCoreApplication, QEvent, Qt
    from PyQt6.QtTest import QTest
    from PyQt6.QtWidgets import QWidget

    from ui_qt.snap.annotations import Tool
    from ui_qt.snap.overlay import SnapOverlay

    canvas = QWidget()
    selecting = SnapOverlay(_frozen_screen(), None, hint=OVERLAY_HINT)
    annotating = SnapOverlay(_frozen_screen(), None, hint=OVERLAY_HINT)
    for index, overlay in enumerate((selecting, annotating)):
        overlay.setParent(canvas)
        overlay.move(0, index * SNAP_IMAGE[1])
        overlay.show()
        # OpenWhisper_CPP's name for the hotkey-window marker.
        overlay.setProperty("openwhisperHotkeyWindow", None)
        overlay.setProperty("openwhisperCppHotkeyWindow", True)

    _mouse(selecting, QEvent.Type.MouseButtonPress, (200, 150), True)
    _mouse(selecting, QEvent.Type.MouseMove, (520, 390), True)

    _drag(annotating, [(120, 80), (840, 520)])
    marks = [
        (Tool.ARROW, 0, [(160, 120), (230, 160), (300, 200)]),
        (Tool.BOX, 1, [(340, 110), (460, 190)]),
        (Tool.ELLIPSE, 2, [(500, 110), (640, 190)]),
        (Tool.PEN, 3, [(160, 260), (200, 300), (240, 250), (280, 310), (300, 280)]),
        (Tool.HIGHLIGHTER, 1, [(340, 280), (410, 281), (480, 282)]),
    ]
    for tool, color, points in marks:
        annotating.set_tool(tool)
        annotating.set_color(color)
        _drag(annotating, points)
    annotating.set_tool(Tool.TEXT)
    annotating.set_color(0)
    _drag(annotating, [(520, 280)])
    QTest.keyClicks(annotating.label_editor, "Click here")
    QTest.keyClick(annotating.label_editor, Qt.Key.Key_Return)
    annotating.set_tool(Tool.STEP)
    annotating.set_color(2)
    _drag(annotating, [(200, 420)])
    _drag(annotating, [(260, 420)])
    annotating.set_tool(Tool.REDACT)
    _drag(annotating, [(600, 380), (720, 460)])
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    return canvas, (SNAP_IMAGE[0], 2 * SNAP_IMAGE[1]), None


#: snap_live_toolbar and capture_delay: what the delay chip shows, on the toolbar's mid grey.
LIVE_MARGIN = 12
LIVE_DELAY = 3
#: The counting-down chip: 1.2 s left of 3 s, 40 % of the ring.
COUNTDOWN = (1.2, 3.0)
#: The picker opens 7 px under its chip (_DelayPicker.open).
PICKER_GAP = 7


def _grey_canvas(name):
    from PyQt6.QtGui import QColor, QPainter
    from PyQt6.QtWidgets import QWidget

    def paint(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(96, 96, 96))
        painter.end()

    return type(name, (QWidget,), {"paintEvent": paint})()


def snap_live_toolbar(variant):
    """The live toolbar as Auto Mode shows it while drawing: the Pen in hand in Coral, snapping
    and Keep marks on, something to put down, and the delay chip at 3 s."""
    from ui_qt.snap.annotations import Tool
    from ui_qt.snap.toolbar import SnapToolbar
    from ui_qt.utils.palette import current_palette

    canvas = _grey_canvas("LiveToolbarCanvas")
    toolbar = SnapToolbar(canvas, current_palette(), live=True)
    toolbar.capture_delay.set_seconds(LIVE_DELAY)
    toolbar.set_state(Tool.PEN, 0, False, False)
    toolbar.set_snap_enabled(True)
    toolbar.set_box_infill(False)
    toolbar.set_keep_marks(True)
    toolbar.set_drop_enabled(True)
    toolbar.move(LIVE_MARGIN, LIVE_MARGIN)
    size = (toolbar.width() + 2 * LIVE_MARGIN, toolbar.height() + 2 * LIVE_MARGIN)
    return canvas, size, None


def capture_delay(variant):
    """Two delay chips side by side, one idle at 3 s and one counting down at 40 %, and under the
    first its preset picker with 3 s current. The picker is a child of the canvas here, at the
    place open() would put it, so one image holds all three; its pointer watch is not started."""
    from ui_qt.widgets.capture_delay import CaptureDelay, _DelayPicker

    canvas = _grey_canvas("CaptureDelayCanvas")
    idle = CaptureDelay(canvas)
    idle.set_seconds(LIVE_DELAY)
    counting = CaptureDelay(canvas)
    counting.set_seconds(LIVE_DELAY)
    counting.set_countdown(*COUNTDOWN)
    idle.move(LIVE_MARGIN, LIVE_MARGIN)
    counting.move(2 * LIVE_MARGIN + idle.width(), LIVE_MARGIN)
    picker = _DelayPicker(idle)
    picker.setParent(canvas)
    picker.move(LIVE_MARGIN, LIVE_MARGIN + idle.height() + PICKER_GAP)
    picker.show()
    width = max(counting.x() + counting.width(), picker.x() + picker.width()) + LIVE_MARGIN
    height = picker.y() + picker.height() + LIVE_MARGIN
    return canvas, (width, height), None


# render_reference.py writes one folder per builder name: tests/reference/<name>/.
BUILDERS = {
    "snap_glyphs": snap_glyphs,
    "snap_toolbar": snap_toolbar,
    "snap_overlay": snap_overlay,
    "snap_live_toolbar": snap_live_toolbar,
    "capture_delay": capture_delay,
}
