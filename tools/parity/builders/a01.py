"""Reference builders of the a01 shell chrome (fv_ui_shell, M0.11).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data.
OpenWhisper paints or labels its product name in these widgets; the builders put OpenWhisper_CPP's in
its place, so the references show what the port must draw (the rest is OpenWhisper's code as it
is). Nothing here runs an event loop: render_reference.py processes events once.
"""

import inspect

PRODUCT = "OpenWhisper_CPP"


def title_bar(variant):
    from ui_qt.main_window import CustomTitleBar

    bar = CustomTitleBar()
    bar.title_label.setText(PRODUCT)
    for name in ("File", "View", "Help"):
        bar.menu_bar.addMenu(name)
    # The bar's natural height: 32 at 100 %, taller once a larger font grows the menu bar.
    bar.menu_bar.ensurePolished()
    bar._sync_height()
    return bar, (680, bar.height()), None


class _FooterOwner:
    """What MainWindow._build_footer reads of its window: the classic (not compositor-managed)
    window, its footer styles and shortcut, and slots that do nothing here. A whole MainWindow
    left PyQt crashing at exit once its footer had been rendered on its own."""

    _compositor_managed = False

    def __init__(self, main_window_class):
        from types import SimpleNamespace

        for name in ("_FOOTER_BAR_STYLE", "_TRAY_BUTTON_STYLE", "_STATS_BUTTON_STYLE",
                     "_SETTINGS_BUTTON_STYLE", "_QUIT_BUTTON_STYLE", "QUIT_SHORTCUT"):
            setattr(self, name, getattr(main_window_class, name))
        self.stats_requested = SimpleNamespace(emit=lambda: None)

    def minimize_to_tray(self):
        pass

    def open_settings(self):
        pass

    def request_quit(self):
        pass


def footer_bar(variant):
    from PyQt6.QtWidgets import QVBoxLayout, QWidget

    from ui_qt.main_window import MainWindow

    host = QWidget()
    owner = _FooterOwner(MainWindow)
    MainWindow._build_footer(owner, QVBoxLayout(host))
    footer = owner.footer
    # Grabbed on its own, as a window of its own; the tray is available (MainWindow's default).
    footer.setParent(None)
    return footer, (680, 48), None


def _rebranded_loading_screen():
    """LoadingScreen as OpenWhisper defines it, with the painted product name rebranded. The
    module's source is read, never written."""
    import ui_qt.loading_screen as module

    source = inspect.getsource(module).replace('"OpenWhisper"', f'"{PRODUCT}"')
    namespace = {"__name__": "builders_a01_loading_screen"}
    exec(compile(source, module.__file__, "exec"), namespace)
    return namespace["LoadingScreen"]


def loading_screen(variant):
    screen = _rebranded_loading_screen()()
    # A fixed frame: pulse 0.5, the timer never consulted.
    screen._glow_phase = lambda: 0.0
    screen._glow_timer.stop()
    screen.update_status("Initializing components...")
    screen.update_progress("Preparing startup...")
    return screen, (450, 300), None


BUILDERS = {
    "title_bar": title_bar,
    "footer_bar": footer_bar,
    "loading_screen": loading_screen,
}
