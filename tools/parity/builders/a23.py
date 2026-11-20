"""Reference builders of the a23 walkthrough UI (fv_ui_walkthrough: M6.5, then M6.9, M6.12, M6.15
and M6.16).

walkthrough_ready/<state> is the corner card that says a walkthrough is ready, shown at the size
adjustSize gives it for a stand-in result in a folder that does not exist: copied (a 3-screenshot
write-up of 1:12 with its thumbnail, tests/fixtures/walkthrough/shot-1.png); write-up-failed (as
spoken, the reason in full under the title and shot-2.png as the thumbnail); copy-failed (no first
screenshot, so no thumbnail). Settings' copy format and Auto-paste are the defaults, which the
card reads through settings_manager.load_all_settings. The card is placed by the offscreen
screen, which the C++ test's offscreen screen matches.

walkthrough_panel/<state> is the live column at the size it takes on a tall screen, after one
scripted walkthrough: words, screenshot 1 (tests/fixtures/walkthrough/shot-1.png) at 0:09, more
words, a note held from 0:20 to 0:26, words, screenshot 2 (shot-2.png) at 0:48, and words being
spoken, read at 1:15 of recording. The column paints from time.monotonic() and its recording
clock, so both are pinned: the module's time is replaced, and the QElapsedTimer is a stand-in
that reads the same milliseconds. Ticks are written into the column directly, TICK_S apart, from
a fixed level script, so no frame timing decides how many there are. Four frames of half a
second bring the column to full opacity first. The C++ test (tests/ui_walkthrough/
test_reference_screenshots.cpp) runs the same script on a FakeClock.

States: recording; paused (set_paused at 1:15); holding (a second note held from 1:10, words in
it); writing (finish at 1:15, read 1 s on: lines, "Writing it up...", only Cancel); done
(complete with 2 screenshots and 75 s at 1:17, read 1 s on); hover (recording, the pointer on
screenshot 1, its tools showing).

walkthrough_snap_menu is the Snap menu with every kind of screenshot, Region current and the
shortcut Ctrl+Alt+S, never opened beside a column (so its pointer watch never runs).

walkthrough_review/<state> is the storyboard at 760 x 720 on a copy of
tests/fixtures/walkthrough/2026-10-20_14-32-05 (three sections: shot-1.png kept at 0:09,
shot-2.png at 0:48 left out, closing words without a screenshot), with no host callbacks:
spoken (as finished); write-up (written up twice, so the version picker reads "Version 2 of 2";
the write-ups are tests/fixtures/walkthrough/write-ups/); draft (draft.json and audio.wav beside
the manifest: the interrupted banner above the rows, Recover off without a recover callback);
dirty (written up, shown as spoken, the first row's words edited: the save bar offers to redo the
write-up). The copies live in one temporary folder removed when the process ends.

write_up_prompt_dialog is the "Redo with a new prompt" dialog on the Bug report style's words,
for a walkthrough already written up, with Settings' styles the starters; the focus is in its
prompt, every word selected.

walkthrough_tab/<state> is the Walkthrough tab at 1100 x 800 with the shortcuts Ctrl+Alt+W and
Ctrl+Alt+S, today pinned to 2026-10-21, no agent scan (the agents read "Checking…"), and Settings
held in memory with only the AI write-up switch and the closed walkthrough set: empty (three
walkthroughs, the newest a draft the tab has closed, so the stage lists all three as recent: the
fixture written up, a copy of it as spoken started 2026-10-19 11:02:40, and the draft); recording
(a walkthrough recording, the stage showing its shortcuts); storyboard (the fixture walkthrough
open); write-up-off (the same with the write-up off, its four rows dimmed); prompt (the prompt
popover opened beside the column, at the size it takes, the focus in the prompt).

past_walkthroughs is the side panel's Walkthroughs page at 380 x 760 over the same three
walkthroughs, today 2026-10-21. new_style_dialog is the New style dialog at its own size, the
focus in its name.
"""
from datetime import date
from pathlib import Path
from types import SimpleNamespace

FIXTURES = Path(__file__).resolve().parents[3] / "tests" / "fixtures" / "walkthrough"
#: The column on a screen tall enough for MAX_HEIGHT: 296 + 2 * 14 by 820 + 2 * 14.
SIZE = (324, 848)
#: The pinned time.monotonic() when the walkthrough begins, in seconds.
T0 = 1000.0
#: Microphone levels, in turn, one per tick written.
LEVELS = (0.05, 0.02, 0.08, 0.03, 0.1, 0.012, 0.06, 0.04, 0.2, 0.025, 0.07, 0.009)


class _Elapsed:
    """The column's QElapsedTimer, reading the pinned milliseconds."""

    def __init__(self):
        self.ms = 0

    def start(self):
        pass

    def elapsed(self):
        return self.ms


class _Script:
    def __init__(self):
        from ui_qt.widgets import walkthrough_panel

        self.module = walkthrough_panel
        self.clock = SimpleNamespace(t=T0)
        walkthrough_panel.time = SimpleNamespace(monotonic=lambda: self.clock.t)
        self.elapsed = _Elapsed()
        self.panel = walkthrough_panel.WalkthroughPanel()
        self.panel._clock = self.elapsed
        self.written = 0

    def at(self, ms):
        self.clock.t = T0 + ms / 1000.0
        self.elapsed.ms = ms

    def speak(self, count):
        column = self.panel.column
        for _ in range(count):
            column.hear(LEVELS[self.written % len(LEVELS)])
            column.advance(self.module.TICK_S, self.clock.t)
            self.written += 1

    def shot(self, number, kept_at_s):
        from PyQt6.QtGui import QImage

        image = QImage(str(FIXTURES / f"shot-{number}.png"))
        if image.isNull():
            raise SystemExit(f"tests/fixtures/walkthrough/shot-{number}.png is missing")
        self.at(int(kept_at_s * 1000))
        self.panel.add_shot(number, image, kept_at_s)


def _recorded(hold_at_end=False):
    script = _Script()
    panel = script.panel
    script.at(0)
    panel.begin()
    panel.setFixedSize(*SIZE)
    for step in range(1, 5):
        script.at(500 * step)
        panel._advance()
    script.speak(24)
    script.shot(1, 9.0)
    script.speak(30)
    script.at(20_000)
    panel._hold(True)
    script.speak(12)
    script.at(26_000)
    panel._hold(False)
    script.speak(10)
    script.shot(2, 48.0)
    script.speak(20)
    if hold_at_end:
        script.at(70_000)
        panel._hold(True)
        script.speak(8)
    script.at(75_000)
    return script


def recording(variant):
    return _recorded().panel, SIZE, None


def paused(variant):
    script = _recorded()
    script.panel.set_paused(True)
    return script.panel, SIZE, None


def holding(variant):
    return _recorded(hold_at_end=True).panel, SIZE, None


def writing(variant):
    script = _recorded()
    script.panel.finish()
    script.at(76_000)
    return script.panel, SIZE, None


def done(variant):
    script = _recorded()
    script.panel.finish()
    script.at(77_000)
    script.panel.complete(SimpleNamespace(shots=2, duration_s=75.0))
    script.at(78_000)
    return script.panel, SIZE, None


def hover(variant):
    script = _recorded()
    panel = script.panel
    panel.grab()  # draws the screenshots where the pointer finds them
    panel._set_hover(panel._shot_rects[1].center())
    return panel, SIZE, None


def _ready_card(**fields):
    """A WalkthroughReadyCard presented for a stand-in result, and the size it took."""
    from pathlib import Path as _Path
    from unittest.mock import patch

    from ui_qt.widgets import walkthrough_ready

    folder = _Path("C:/captures/2026-10-21 14-32-05")
    values = dict(name="Walkthrough", shots=1, duration_s=0, first_shot=None, write_up_error="",
                  folder=folder, pdf=folder / "walkthrough.pdf", docx=folder / "walkthrough.docx")
    copied = fields.pop("copied", True)
    values.update(fields)
    card = walkthrough_ready.WalkthroughReadyCard()
    with patch.object(walkthrough_ready.settings_manager, "load_all_settings", lambda: {}):
        card.present(SimpleNamespace(**values), copied=copied)
    return card, (card.width(), card.height()), None


def ready_copied(variant):
    return _ready_card(name="Write-up", shots=3, duration_s=72, first_shot=FIXTURES / "shot-1.png")


def ready_write_up_failed(variant):
    return _ready_card(
        shots=2, duration_s=75, first_shot=FIXTURES / "shot-2.png",
        write_up_error="Claude Code is not installed on this computer. Choose another agent "
                       "in Settings → Dictation → Walkthroughs.")


def ready_copy_failed(variant):
    return _ready_card(copied=False, shots=1, duration_s=11)


def snap_menu(variant):
    from services.settings import WalkthroughSnapMode
    from ui_qt.widgets.walkthrough_snap_menu import SnapMenu

    menu = SnapMenu(WalkthroughSnapMode.ALL, WalkthroughSnapMode.REGION, "Ctrl+Alt+S")
    return menu, (menu.width(), menu.height()), None


#: The storyboard's size in the references.
REVIEW_SIZE = (760, 720)
_REVIEW_SCRATCH = []


def _review_folder(write_ups=False, draft=False):
    """A copy of the storyboard's fixture walkthrough, with write-ups or a draft as asked."""
    import atexit
    import shutil
    import tempfile

    if not _REVIEW_SCRATCH:
        scratch = tempfile.TemporaryDirectory(prefix="fv-review-")
        atexit.register(scratch.cleanup)
        _REVIEW_SCRATCH.append(scratch)
    folder = Path(tempfile.mkdtemp(dir=_REVIEW_SCRATCH[0].name)) / "2026-10-20_14-32-05"
    shutil.copytree(FIXTURES / "2026-10-20_14-32-05", folder)
    if write_ups:
        for name in ("write-up.md", "write-up-1.md"):
            shutil.copyfile(FIXTURES / "write-ups" / name, folder / name)
            (folder / name).with_suffix(".pdf").write_bytes(b"%PDF-1.4 stand-in\n")
        (folder / "write-up.docx").write_bytes(b"stand-in\n")
    if draft:
        (folder / "draft.json").write_text(
            '{"version": 1, "started_at": "2026-10-20T14:32:05", "shots": []}', encoding="utf-8")
        (folder / "audio.wav").write_bytes(b"RIFF")
    return folder


class _Today(date):
    @classmethod
    def today(cls):
        return date(*_TODAY)


def _review():
    from ui_qt.widgets import walkthrough_review

    walkthrough_review.date = _Today
    return walkthrough_review.WalkthroughReview()


def review_spoken(variant):
    review = _review()
    review.show_folder(_review_folder())
    return review, REVIEW_SIZE, None


def review_write_up(variant):
    review = _review()
    review.show_folder(_review_folder(write_ups=True))
    return review, REVIEW_SIZE, None


def review_draft(variant):
    review = _review()
    review.show_folder(_review_folder(draft=True))
    return review, REVIEW_SIZE, None


def review_dirty(variant):
    review = _review()
    review.show_folder(_review_folder(write_ups=True), spoken=True)
    review.rows[0].editor.setPlainText(
        "On the billing page, Change plan takes three seconds before anything happens.")
    return review, REVIEW_SIZE, None


def prompt_dialog(variant):
    from unittest.mock import patch

    from services.walkthrough.styles import STARTER_STYLES
    from ui_qt.dialogs import write_up_prompt_dialog

    with patch.object(write_up_prompt_dialog.settings_manager, "load_all_settings", lambda: {}):
        dialog = write_up_prompt_dialog.WriteUpPromptDialog(STARTER_STYLES[1].instructions,
                                                            written=True)
    return dialog, (580, 500), dialog.prompt_edit


#: The Walkthrough tab's size in the references, and the day its walkthroughs are told against.
TAB_SIZE = (1100, 800)
#: The size OpenWhisper's own tab tests give it, where the storyboard's rows only just overflow.
TAB_SHORT_SIZE = (1100, 760)
#: The side panel's Walkthroughs page: the sidebar's expanded width.
PAST_SIZE = (380, 760)
_TODAY = (2026, 10, 21)
_TAB_SETTINGS = {}
#: The newest of the three walkthroughs, a draft, closed so the stage shows all three as recent.
_DRAFT_NAME = "2026-10-21_09-15-00"


def _scratch():
    import atexit
    import tempfile

    if not _REVIEW_SCRATCH:
        scratch = tempfile.TemporaryDirectory(prefix="fv-review-")
        atexit.register(scratch.cleanup)
        _REVIEW_SCRATCH.append(scratch)
    return Path(tempfile.mkdtemp(dir=_REVIEW_SCRATCH[0].name))


def _captures(kind):
    """A captures folder: "none" (empty), "one" (the fixture walkthrough) or "three" (the fixture
    written up, a copy of it as spoken started 2026-10-19 11:02:40, and a newer draft)."""
    import json
    import shutil

    root = _scratch() / "captures"
    root.mkdir()
    if kind == "none":
        return root
    written = root / "2026-10-20_14-32-05"
    shutil.copytree(FIXTURES / "2026-10-20_14-32-05", written)
    if kind == "one":
        return root
    shutil.copyfile(FIXTURES / "write-ups" / "write-up.md", written / "write-up.md")
    (written / "write-up.pdf").write_bytes(b"%PDF-1.4 stand-in\n")
    spoken = root / "2026-10-19_11-02-40"
    shutil.copytree(FIXTURES / "2026-10-20_14-32-05", spoken)
    manifest = json.loads((spoken / "walkthrough.json").read_text(encoding="utf-8"))
    manifest["started_at"] = "2026-10-19T11:02:40"
    (spoken / "walkthrough.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    draft = root / _DRAFT_NAME
    draft.mkdir()
    (draft / "draft.json").write_text(
        '{"version": 1, "started_at": "2026-10-21T09:15:00", "shots": []}', encoding="utf-8")
    return root


def _wait_until(done, what):
    import time

    from PyQt6.QtCore import QCoreApplication

    deadline = time.monotonic() + 10.0
    while not done():
        if time.monotonic() > deadline:
            raise SystemExit(f"{what} never finished")
        QCoreApplication.processEvents()
        time.sleep(0.005)


def _pin_tab(root, values):
    """The tab's world for a reference: its captures folder, today, no agent scan, and
    Settings held in memory (the tab saves what its switches show)."""
    from services.settings import settings_manager
    from ui_qt.widgets import action_relay, agent_picker, walkthrough_review, walkthrough_tab

    walkthrough_review.date = walkthrough_tab.date = _Today
    walkthrough_tab.captures_root = lambda: root
    agent_picker.cached_agents = lambda: None
    action_relay.start_agent_scan = lambda token, **_kwargs: None
    _TAB_SETTINGS.clear()
    _TAB_SETTINGS.update(values)
    settings_manager.load_all_settings = lambda: dict(_TAB_SETTINGS)
    settings_manager.save_setting = lambda key, value: _TAB_SETTINGS.__setitem__(key, value)


def _tab(kind, write_up=True, closed=""):
    from services.settings import SettingsKey
    from ui_qt.widgets.walkthrough_tab import WalkthroughTab

    _pin_tab(_captures(kind), {SettingsKey.WALKTHROUGH_WRITE_UP: write_up,
                               SettingsKey.WALKTHROUGH_CLOSED: closed})
    tab = WalkthroughTab()
    tab.set_hotkeys("Ctrl+Alt+W", "Ctrl+Alt+S")
    _wait_until(lambda: not tab.recent_loading, "Reading the recent walkthroughs")
    return tab


def tab_empty(variant):
    return _tab("three", closed=_DRAFT_NAME), TAB_SIZE, None


def tab_recording(variant):
    tab = _tab("none")
    tab.set_recording_state(True, True)
    return tab, TAB_SIZE, None


def tab_storyboard(variant):
    return _tab("one"), TAB_SIZE, None


def tab_storyboard_short(variant):
    return _tab("one"), TAB_SHORT_SIZE, None


def tab_write_up_off(variant):
    return _tab("one", write_up=False), TAB_SIZE, None


def tab_prompt(variant):
    tab = _tab("none")
    tab.resize(*TAB_SIZE)
    tab.edit_prompt()
    popover = tab.prompt_popover
    return popover, (popover.width(), popover.height()), tab.prompt_edit


def past_walkthroughs(variant):
    from datetime import date

    from ui_qt.widgets.past_walkthroughs import PastWalkthroughsPanel

    panel = PastWalkthroughsPanel(root=_captures("three"), today=lambda: date(*_TODAY))
    panel.refresh()
    _wait_until(lambda: panel._total is not None, "Reading the walkthroughs")
    return panel, PAST_SIZE, None


def new_style_dialog(variant):
    from ui_qt.dialogs.new_style_dialog import NewStyleDialog

    dialog = NewStyleDialog()
    return dialog, (540, 420), dialog.name_edit


# render_reference.py writes one folder per name: tests/reference/walkthrough_panel/<state>/,
# tests/reference/walkthrough_ready/<state>/, tests/reference/walkthrough_snap_menu/,
# tests/reference/walkthrough_review/<state>/, tests/reference/write_up_prompt_dialog/,
# tests/reference/walkthrough_tab/<state>/, tests/reference/past_walkthroughs/ and
# tests/reference/new_style_dialog/.
BUILDERS = {
    "walkthrough_ready/copied": ready_copied,
    "walkthrough_ready/write-up-failed": ready_write_up_failed,
    "walkthrough_ready/copy-failed": ready_copy_failed,
    "walkthrough_panel/recording": recording,
    "walkthrough_panel/paused": paused,
    "walkthrough_panel/holding": holding,
    "walkthrough_panel/writing": writing,
    "walkthrough_panel/done": done,
    "walkthrough_panel/hover": hover,
    "walkthrough_snap_menu": snap_menu,
    "walkthrough_review/spoken": review_spoken,
    "walkthrough_review/write-up": review_write_up,
    "walkthrough_review/draft": review_draft,
    "walkthrough_review/dirty": review_dirty,
    "write_up_prompt_dialog": prompt_dialog,
    "walkthrough_tab/empty": tab_empty,
    "walkthrough_tab/recording": tab_recording,
    "walkthrough_tab/storyboard": tab_storyboard,
    "walkthrough_tab/storyboard-760": tab_storyboard_short,
    "walkthrough_tab/write-up-off": tab_write_up_off,
    "walkthrough_tab/prompt": tab_prompt,
    "past_walkthroughs": past_walkthroughs,
    "new_style_dialog": new_style_dialog,
}
