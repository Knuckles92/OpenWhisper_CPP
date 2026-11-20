"""Curated inputs for ui_qt/snap/capture.py settle_ms (fv_ui_snap settleMs). Each case is
[refresh rate in Hz or None for no screen, captures through grim, Qt platform "cocoa"], posed
the way tests/test_snap_session.py's TestCapture does, with the module restored afterwards."""


def _settle_ms(module, rate, grim, cocoa):
    from types import SimpleNamespace

    from PyQt6.QtGui import QGuiApplication

    saved_grim = module.support.uses_grim
    saved_name = QGuiApplication.platformName
    module.support.uses_grim = lambda: grim
    QGuiApplication.platformName = staticmethod(lambda: "cocoa" if cocoa else "offscreen")
    try:
        screen = None if rate is None else SimpleNamespace(refreshRate=lambda: rate)
        return module.settle_ms(screen)
    finally:
        module.support.uses_grim = saved_grim
        QGuiApplication.platformName = saved_name


WRAPPERS = {"settle_ms": _settle_ms}

CASES = {
    "settle_ms": [
        [rate, grim, cocoa]
        for rate in (None, 0.0, 23.9, 24.0, 29.97, 30.0, 50.0, 58.82, 59.94, 60.0, 75.0, 90.0,
                     120.0, 144.0, 165.0, 240.0, 360.0)
        for grim in (False, True)
        for cocoa in (False, True)
    ],
}
