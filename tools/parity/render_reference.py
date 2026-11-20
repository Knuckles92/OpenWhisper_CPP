"""Render OpenWhisper widgets offscreen, styled as the app styles them, as reference screenshots.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/render_reference.py --builder a02_harness
        [--widget empty_state_harness ...] [--theme dark|light] [--style windows11]
        [--dpr 1] [--scale 100] [--dump-stylesheet] [--check] [--out tests/reference]

Builders are tools/parity/builders/<builder>.py, one per area, each exporting
BUILDERS = {name: fn(variant) -> (widget, (width, height), focus widget or None)}, with fixed
fake data. For every widget this writes, under the output folder:

    <widget>/<theme>-<style>-dpr<n>-s<scale>.png    the reference image
    <widget>/<theme>-<style>-dpr<n>-s<scale>.json   every pin of the render plus the widget tree

--dump-stylesheet also writes app_stylesheet/<theme>-s<scale>.qss (the resolved application
stylesheet, with url(<checkout>/ui_qt/assets/...) rewritten to url(:/ui_theme/assets/...), the
paths fv_ui_theme's resources use), its .sha1, and app_stylesheet/palette-<theme>.json (the
QPalette the theme manager set, per colour group). --check renders into out/parity/render-check
instead and fails when a pixel or stylesheet SHA-1 or a palette differs from the committed file.

Pins, in the order they are set: environment before QApplication (QT_QPA_PLATFORM=offscreen;
QT_QPA_FONTDIR, because offscreen rasterizes with FreeType from that folder and finds no fonts on
Windows otherwise; QT_SCALE_FACTOR, QT_ENABLE_HIGHDPI_SCALING=1 and QT_FONT_DPI=96 for the device
pixel ratio and logical DPI; no QT_SCREEN_SCALE_FACTORS or QT_STYLE_OVERRIDE); the base style
after QApplication exists (setting windows11 before it crashes the offscreen process on Qt
6.11.0); ThemeManager, WidgetStyleFilter, apply_ui_theme and apply_ui_font_scale, as
ui_qt/app.py's QtApplication does; a fixed size; show, the pinned focus, one processEvents and no
event loop, so no animation advances and no timer fires. Never move the mouse.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ow_isolation  # noqa: E402

REFERENCE_DIR = ow_isolation.REPO / "tests" / "reference"
CHECK_DIR = ow_isolation.SCRATCH / "render-check"
ASSET_PREFIX = ":/ui_theme/assets/"


def load_builders(name: str):
    path = TOOLS / "builders" / f"{name}.py"
    if not path.is_file():
        raise SystemExit(f"No builder file {path.relative_to(ow_isolation.REPO)}.")
    spec = importlib.util.spec_from_file_location(f"builders_{name}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.BUILDERS, set(getattr(module, 'TREE_ONLY', ()))


def rewrite_asset_urls(sheet: str, checkout: Path) -> str:
    """url(<checkout>/ui_qt/assets/x.svg) -> url(:/ui_theme/assets/x.svg). Fails when any
    absolute checkout path is left, since the sheet's SHA-1 could then never match fv_ui_theme's."""
    root = re.escape(checkout.as_posix())
    pattern = re.compile(r"url\((['\"]?)" + root + r"/ui_qt/assets/", re.IGNORECASE)
    rewritten = pattern.sub(lambda m: f"url({m.group(1)}{ASSET_PREFIX}", sheet)
    for form in (checkout.as_posix(), str(checkout)):
        if form.lower() in rewritten.lower():
            raise SystemExit(f"The stylesheet still names {form} after the asset rewrite.")
    return rewritten


def qt_base_class(widget) -> str:
    """The first C++ Qt class in the widget's chain (Button -> QPushButton)."""
    meta = widget.metaObject()
    while meta is not None and not meta.className().startswith("Q"):
        meta = meta.superClass()
    return meta.className() if meta else "QWidget"


def dump_tree(widget) -> dict:
    """Class, name, dynamic properties, geometry, text and local stylesheet, recursively. This is
    the schema fv::screenshot::dump_tree writes for a C++ widget, so a port's tree is compared
    with it before any pixel is."""
    from PyQt6.QtWidgets import QWidget

    node = {"class": widget.metaObject().className(), "qtClass": qt_base_class(widget),
            "objectName": widget.objectName(),
            "geometry": [widget.x(), widget.y(), widget.width(), widget.height()],
            "visible": widget.isVisible(), "hasFocus": widget.hasFocus(),
            "properties": {bytes(n).decode(): str(widget.property(bytes(n).decode()))
                           for n in widget.dynamicPropertyNames()
                           if not bytes(n).startswith(b"_q_")}}
    for attr in ("text", "isChecked", "isDefault", "alignment", "wordWrap"):
        fn = getattr(widget, attr, None)
        if callable(fn):
            try:
                value = fn()
            except TypeError:
                continue
            node[attr] = value if isinstance(value, (bool, str)) else int(value.value) \
                if hasattr(value, "value") else str(value)
    if widget.styleSheet():
        node["styleSheet"] = widget.styleSheet()
    node["font"] = widget.font().toString()
    node["children"] = [dump_tree(c) for c in widget.children() if isinstance(c, QWidget)
                        and not c.isWindow()]
    return node


def dump_palette(app) -> dict:
    """{group: {role: "#aarrggbb"}} for the Active, Inactive and Disabled groups: ThemeManager's
    build_qpalette gives the Disabled group its own text colours, which unstyled disabled text
    is drawn with."""
    from PyQt6.QtGui import QColor, QPalette

    pal = app.palette()
    groups = (QPalette.ColorGroup.Active, QPalette.ColorGroup.Inactive,
              QPalette.ColorGroup.Disabled)
    return {group.name: {role.name: pal.color(group, role).name(QColor.NameFormat.HexArgb)
                         for role in QPalette.ColorRole if role.name != "NColorRoles"}
            for group in groups}


def color_scheme(app) -> str:
    """The colour scheme in effect after apply_ui_theme asked for the theme's one: windows11 draws
    the controls the stylesheet leaves to it in that scheme's colours. The offscreen platform does
    not take the request, so it reads "Unknown" there; recorded so the C++ side can match it."""
    return app.styleHints().colorScheme().name


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def sha1(text: str) -> str:
    return hashlib.sha1(text.encode("utf-8")).hexdigest()


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--builder", help="builders/<name>.py, for example a02_harness")
    p.add_argument("--widget", action="append", default=[],
                   help="one of the builder's widgets (repeatable; default all)")
    p.add_argument("--theme", default="dark", choices=["dark", "light"])
    p.add_argument("--style", default="windows11")
    p.add_argument("--dpr", default="1")
    p.add_argument("--scale", type=int, default=100)
    p.add_argument("--fontdir", default=os.path.join(os.environ.get("WINDIR", r"C:\Windows"),
                                                      "Fonts"))
    p.add_argument("--dump-stylesheet", action="store_true")
    p.add_argument("--check", action="store_true",
                   help="render into out/parity/render-check and compare with the committed")
    p.add_argument("--out", type=Path, default=REFERENCE_DIR)
    p.add_argument("--allow-other-commit", action="store_true")
    a = p.parse_args(argv)
    if not a.builder and not a.dump_stylesheet:
        p.error("name --builder, --dump-stylesheet or both")

    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout, a.allow_other_commit)
    builders, tree_only = load_builders(a.builder) if a.builder else ({}, set())
    unknown = [w for w in a.widget if w not in builders]
    if unknown:
        raise SystemExit(f"{a.builder} has no widget {', '.join(unknown)}.")
    selected = a.widget or list(builders)

    data = ow_isolation.isolate()
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    os.environ["QT_QPA_FONTDIR"] = a.fontdir
    os.environ["QT_ENABLE_HIGHDPI_SCALING"] = "1"
    os.environ["QT_SCALE_FACTOR"] = a.dpr
    os.environ["QT_FONT_DPI"] = "96"
    os.environ.pop("QT_SCREEN_SCALE_FACTORS", None)
    os.environ.pop("QT_STYLE_OVERRIDE", None)

    from PyQt6.QtCore import PYQT_VERSION_STR, QT_VERSION_STR, QCoreApplication
    from PyQt6.QtWidgets import QApplication

    reference = ow_isolation.reference()
    if PYQT_VERSION_STR != reference["pyqt"]:
        raise SystemExit(f"PyQt6 {PYQT_VERSION_STR} is not the pinned {reference['pyqt']}.")
    ow_isolation.after_import_guards(data)
    app = QApplication([])
    app.setStyle(a.style)
    base_style = app.style().metaObject().className()

    from services.settings import UiTheme
    from ui_qt.utils.font_scale import WidgetStyleFilter, apply_ui_font_scale, apply_ui_theme
    from ui_qt.utils.theme_manager import ThemeManager

    manager = ThemeManager(a.theme)
    style_filter = WidgetStyleFilter(app)
    app.installEventFilter(style_filter)
    apply_ui_theme(UiTheme.LIGHT if a.theme == "light" else UiTheme.DARK, app=app,
                   theme_manager=manager)
    apply_ui_font_scale(a.scale, app=app, theme_manager=manager)

    sheet = rewrite_asset_urls(app.styleSheet(), checkout)
    out = CHECK_DIR if a.check else a.out
    failures = []

    if a.dump_stylesheet:
        stem = f"{a.theme}-s{a.scale}"
        folder = out / "app_stylesheet"
        write_text(folder / f"{stem}.qss", sheet)
        write_text(folder / f"{stem}.sha1", sha1(sheet) + "\n")
        write_text(folder / f"palette-{a.theme}.json",
                   json.dumps(dump_palette(app), indent=1) + "\n")
        print(f"app_stylesheet/{stem}.qss: {len(sheet.encode('utf-8'))} bytes, sha1 {sha1(sheet)}")
        if a.check:
            committed = REFERENCE_DIR / "app_stylesheet" / f"{stem}.sha1"
            if not committed.is_file() or committed.read_text().strip() != sha1(sheet):
                failures.append(f"app_stylesheet/{stem}: SHA-1 differs from the committed file")
            palette = f"palette-{a.theme}.json"
            committed_palette = REFERENCE_DIR / "app_stylesheet" / palette
            if not committed_palette.is_file() or committed_palette.read_bytes() != \
                    (folder / palette).read_bytes():
                failures.append(f"app_stylesheet/{palette}: differs from the committed file")

    variant = {"theme": a.theme, "style": a.style, "dpr": a.dpr, "scale": a.scale}
    variant_name = f"{a.theme}-{a.style}-dpr{a.dpr}-s{a.scale}"
    for name in selected:
        widget, (w, h), focus = builders[name](variant)
        widget.setFixedSize(w, h)
        widget.show()
        if focus is not None:
            focus.setFocus()
        QCoreApplication.processEvents()
        image = widget.grab().toImage()
        png = out / name / f"{variant_name}.png"
        png.parent.mkdir(parents=True, exist_ok=True)
        image.save(str(png))
        bits = image.constBits()
        bits.setsize(image.sizeInBytes())
        pins = {
            "widget": name, "variant": variant_name, "builder": a.builder,
            "ow_commit": commit, "logical_size": [w, h],
            "image_size": [image.width(), image.height()],
            "device_pixel_ratio": image.devicePixelRatio(),
            "qt": QT_VERSION_STR, "pyqt": PYQT_VERSION_STR, "platform": app.platformName(),
            "base_style": base_style, "style": app.style().metaObject().className(),
            "theme": a.theme, "color_scheme": color_scheme(app), "font_scale_percent": a.scale,
            "app_font": app.font().toString(),
            "font_resolved": widget.fontInfo().family(),
            "qt_qpa_fontdir": a.fontdir,
            "stylesheet_sha1": sha1(sheet),
            "pixels_sha1": hashlib.sha1(bytes(bits)).hexdigest(),
            "tree": dump_tree(widget),
        }
        write_text(png.with_suffix(".json"), json.dumps(pins, indent=1) + "\n")
        print(f"{name}/{png.name}: {pins['image_size']} dpr={pins['device_pixel_ratio']} "
              f"style={base_style} font={pins['font_resolved']} px={pins['pixels_sha1'][:10]}")
        widget.close()
        if a.check:
            committed = REFERENCE_DIR / name / f"{variant_name}.json"
            if not committed.is_file():
                failures.append(f"{name}/{variant_name}: no committed reference")
            else:
                reference_pins = json.loads(committed.read_text(encoding="utf-8"))
                expected = reference_pins["pixels_sha1"]
                if name in tree_only:
                    if reference_pins["tree"] != pins["tree"]:
                        failures.append(f"{name}/{variant_name}: widget tree differs from its reference")
                elif expected != pins["pixels_sha1"]:
                    failures.append(f"{name}/{variant_name}: pixel SHA-1 {pins['pixels_sha1']} "
                                    f"differs from the committed {expected}")
    for failure in failures:
        print(f"MISMATCH {failure}")
    if a.check and not failures:
        print("render check: identical to the committed references")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
