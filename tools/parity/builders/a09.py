"""Reference builders of the a09 History sidebar (fv_ui_history, M1.15), calendar (M2.22),
Stats window and export dialog (M2.27).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data. Three
entries, newest first: a cleaned dictation with its recording, a failed recording with its
recording, and a file transcription without one. Timestamps are naive local wall time, which
format_timestamp shows unchanged, and today is pinned to 2026-03-15 for the calendar button.

OpenWhisper_CPP differs from OpenWhisper in two places these builders put OpenWhisper_CPP's in:
- the model badge of a "parakeet (<label> | <device>)" model string shows the label alone
  (ui_history/history_text.cpp format_model_name), so _format_model_name is wrapped here;
- the Past Meetings panel the sidebar builds hidden is M7's, so it is taken out of the tree.

The recordings are written into the isolated data folder (out/parity/ow-data), never into
OpenWhisper's own. Nothing here runs an event loop: render_reference.py processes events once.
"""

import datetime as _datetime
from unittest.mock import patch

MODEL = "parakeet (Parakeet TDT 0.6B v3 | cuda)"
TODAY = (2026, 3, 15)
DICTATION_AUDIO = "recording_20260315_150000.wav"
FAILED_AUDIO = "recording_20260315_143000.wav"
CARD_WIDTH = 348

_pinned = False


def _pin():
    """Pins today's date and OpenWhisper_CPP's model badge, once per process."""
    global _pinned
    if _pinned:
        return
    _pinned = True

    from ui_qt.dialogs import history_entry_dialog
    from ui_qt.widgets import history_calendar, history_sidebar

    class PinnedDate(_datetime.date):
        @classmethod
        def today(cls):
            return cls(*TODAY)

    history_calendar.date = PinnedDate

    original = history_sidebar._format_model_name

    def openwhisper_cpp_model_name(model):
        base, _, detail = model.partition("(")
        if base.strip() == "parakeet":
            return detail.rstrip(")").split("|")[0].strip() or "Parakeet"
        return original(model)

    history_sidebar._format_model_name = openwhisper_cpp_model_name
    history_entry_dialog._format_model_name = openwhisper_cpp_model_name


def _recordings():
    """The two saved recordings, as files the History manager finds."""
    import os

    from services.history_manager import history_manager

    folder = history_manager.recordings_folder
    os.makedirs(folder, exist_ok=True)
    for name, size in ((DICTATION_AUDIO, 403200), (FAILED_AUDIO, 264600)):
        path = os.path.join(folder, name)
        if not os.path.isfile(path) or os.path.getsize(path) != size:
            with open(path, "wb") as handle:
                handle.write(b"\0" * size)


def _entries():
    from services.models import TranscriptionHistory

    _pin()
    _recordings()
    dictation = TranscriptionHistory(
        id="entry-dictation", text="Hello from OpenWhisper_CPP.", raw_text="hello from openwhisper-cpp",
        timestamp="2026-03-15T15:00:00", model=MODEL, audio_file=DICTATION_AUDIO,
        transcription_time=0.42, audio_duration=4.6, file_size=403200,
        cleanup_provider="openai", cleanup_model="gpt-4o-mini", source_name="Quick Record",
        app_name="Notepad", cleanup_level="medium", entry_kind="dictation",
    )
    failed = TranscriptionHistory(
        id="entry-failed", text="", timestamp="2026-03-15T14:30:00", model=MODEL,
        audio_file=FAILED_AUDIO, audio_duration=3.0, file_size=264600,
        source_name="Quick Record", entry_kind="failed_recording",
    )
    upload = TranscriptionHistory(
        id="entry-file",
        text="We reviewed the interview notes and agreed on the next steps for the launch.",
        timestamp="2026-03-15T14:00:00", model=MODEL, transcription_time=12.3,
        audio_duration=754.0, source_name="interview.mp3", entry_kind="file",
    )
    return dictation, failed, upload


def _card(entry, height):
    from ui_qt.widgets.history_sidebar import HistoryItemWidget

    return HistoryItemWidget(entry), (CARD_WIDTH, height), None


def history_item_dictation(variant):
    dictation, _failed, _upload = _entries()
    return _card(dictation, 196 if variant["scale"] == 100 else 232)


def history_item_failed(variant):
    _dictation, failed, _upload = _entries()
    return _card(failed, 176 if variant["scale"] == 100 else 210)


def _sidebar():
    from ui_qt.widgets.history_sidebar import HistorySidebar

    sidebar = HistorySidebar()
    # Past Meetings is M7's page; it stays hidden here and has no OpenWhisper_CPP widget yet.
    sidebar.meetings_content_widget.setParent(None)
    sidebar.meetings_content_widget.deleteLater()
    del sidebar.meetings_content_widget
    return sidebar


def history_sidebar_expanded(variant):
    entries = list(_entries())
    sidebar = _sidebar()
    # Opened with the animation at its end; the load the open would start is replaced by its
    # result.
    sidebar._is_expanded = True
    sidebar._refresh_pending = False
    sidebar._set_sidebar_width(sidebar.EXPANDED_WIDTH)
    sidebar._history_load_generation = 1
    sidebar._apply_history_results(1, "", entries, "")
    return sidebar, (sidebar.EXPANDED_WIDTH, 760), None


def _edge_tab(expanded):
    from ui_qt.widgets.history_sidebar import HistoryEdgeTab

    tab = HistoryEdgeTab()
    tab.set_shortcut_hint("Ctrl+H")
    tab.set_expanded(expanded)
    return tab, (24, 120), None


def history_edge_tab_collapsed(variant):
    return _edge_tab(False)


def history_edge_tab_expanded(variant):
    return _edge_tab(True)


def history_entry_dialog_dictation(variant):
    from ui_qt.dialogs.history_entry_dialog import HistoryEntryDialog

    dictation, _failed, _upload = _entries()
    dialog = HistoryEntryDialog(dictation)
    return dialog, (720, 700), None


def history_delete_confirm_with_audio(variant):
    from PyQt6.QtCore import QCoreApplication
    from PyQt6.QtWidgets import QMessageBox

    from ui_qt.widgets import history_sidebar

    dictation, _failed, _upload = _entries()
    sidebar = _sidebar()
    shown = []

    def capture(box):
        shown.append(box)
        return QMessageBox.StandardButton.No

    with (
        patch.object(history_sidebar.history_manager, "get_entry_by_id", return_value=dictation),
        patch.object(QMessageBox, "exec", capture),
    ):
        sidebar._confirm_delete(dictation.id, None, None)
    box = shown[0]
    # The box sizes itself as it shows; that size is the one recorded.
    box.show()
    QCoreApplication.processEvents()
    size = (box.width(), box.height())
    box.hide()
    box._keep_sidebar = sidebar
    return box, size, None


# ---- the History calendar (M2.22) ----
#
# The calendar reads through a fixed source (OpenWhisper's tests use the same kind of FakeSource):
# the three entries above on 2026-03-15 plus twelve across February and March 2026, made into
# items by services.history_calendar.entry_item. The dialog reads at once (threaded=False), weeks
# start on Sunday, and its animations are taken to their ends: the year strip's bars grown, the
# agenda cards' fade-in finished, and the agenda's placeholder deleted. Today's pulse waits on a
# 220 ms timer, which never fires in a render.

_CALENDAR_ENTRIES = (
    # id, timestamp, text, source_name, entry_kind, audio_duration
    ("cal-01", "2026-02-03T09:12:00", "Notes for the February planning meeting.", "Quick Record",
     "dictation", 6.2),
    ("cal-02", "2026-02-03T16:40:00", "The budget review went through every line of the plan.",
     "budget-review.mp3", "file", 312.0),
    ("cal-03", "2026-02-17T11:05:00", "Call the printer about the order.", "Quick Record",
     "dictation", 3.4),
    ("cal-04", "2026-02-24T08:30:00", "Draft the reply to the landlord.", "Quick Record",
     "dictation", 5.0),
    ("cal-05", "2026-03-02T10:15:00", "Book the room for Thursday.", "Quick Record", "dictation",
     4.1),
    ("cal-06", "2026-03-02T13:20:00", "Everyone shared what they finished yesterday.",
     "standup.m4a", "file", 640.0),
    ("cal-07", "2026-03-09T09:00:00", "Water the plants.", "Quick Record", "dictation", 2.8),
    ("cal-08", "2026-03-10T17:45:00", "Ideas for the spring newsletter and the photo page.",
     "Quick Record", "dictation", 7.5),
    ("cal-09", "2026-03-11T12:00:00", "Today we look at how rivers shape their valleys.",
     "lecture.wav", "file", 1800.0),
    ("cal-10", "2026-03-13T09:30:00", "Send the invoice before Friday.", "Quick Record",
     "dictation", 3.0),
    ("cal-11", "2026-03-14T18:10:00", "Pick up the bike from the shop.", "Quick Record",
     "dictation", 4.4),
    ("cal-12", "2026-03-14T20:05:00", "Remember the birthday card.", "Quick Record", "dictation",
     2.2),
)


class _CalendarSource:
    """CalendarSource's four reads, from a fixed list of items."""

    notice = ""

    def __init__(self, items):
        self.items = list(items)

    def load_index(self):
        from services.history_calendar import Mark

        return [Mark(it.when, it.kind, it.place, it.record_id, it.seconds) for it in self.items]

    def remote_wanted(self):
        return False

    def load_remote(self, local_ids):
        return []

    def load_month(self, year, month):
        return sorted((it for it in self.items if (it.when.year, it.when.month) == (year, month)),
                      key=lambda it: (it.when, it.record_id))


def _calendar_items():
    from datetime import datetime

    from services.history_calendar import entry_item
    from services.models import TranscriptionHistory

    entries = list(_entries())
    for record_id, stamp, text, source, kind, seconds in _CALENDAR_ENTRIES:
        entries.append(TranscriptionHistory(
            id=record_id, text=text, timestamp=stamp, model=MODEL, audio_duration=seconds,
            source_name=source, entry_kind=kind,
        ))
    return [entry_item(entry, datetime.fromisoformat(entry.timestamp)) for entry in entries]


def _finish_fade(widget):
    from PyQt6.QtCore import QSequentialAnimationGroup

    for group in widget.findChildren(QSequentialAnimationGroup):
        group.stop()
    widget.setGraphicsEffect(None)


def history_calendar_dialog_march(variant):
    from datetime import date

    from PyQt6.QtCore import QCoreApplication, QEvent

    from ui_qt.dialogs.history_calendar_dialog import HistoryCalendarDialog

    _pin()
    dialog = HistoryCalendarDialog(source=_CalendarSource(_calendar_items()),
                                   today=lambda: date(*TODAY), first_weekday=6, threaded=False)
    dialog.refresh()
    strip = dialog.year_strip
    strip._grow.setCurrentTime(strip._grow.duration())
    for card in dialog._cards:
        _finish_fade(card)
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete.value)
    return dialog, (1180, 800), None


def calendar_month_grid_crowded_day(variant):
    from datetime import date, datetime

    from services.history_calendar import (
        DICTATION, FILE, HERE, MEETING, CalendarIndex, CalendarItem, Mark,
    )
    from ui_qt.widgets.history_calendar import MonthGrid

    _pin()
    # The 12th holds every kind and more lines than its cell has room for.
    records = [("g-d%d" % n, datetime(2026, 3, 12, 8, 5 * n), DICTATION, f"Quick note {n + 1}")
               for n in range(7)]
    records += [
        ("g-m1", datetime(2026, 3, 12, 15, 0), MEETING, "Design review"),
        ("g-f1", datetime(2026, 3, 12, 17, 30), FILE, "interview.wav"),
        ("g-d10", datetime(2026, 3, 3, 9, 0), DICTATION, "Groceries for the week"),
        ("g-f2", datetime(2026, 3, 5, 14, 0), FILE, "lecture-notes.mp3"),
        ("g-m2", datetime(2026, 3, 9, 10, 0), MEETING, "Weekly sync"),
        ("g-d11", datetime(2026, 3, 15, 9, 30), DICTATION, "Plan the trip"),
    ]
    items = [CalendarItem(kind=kind, record_id=record_id, when=when, title=title, preview="",
                          seconds=60.0, place=HERE, source={"id": record_id})
             for record_id, when, kind, title in records]
    index = CalendarIndex([Mark(it.when, it.kind, it.place, it.record_id, it.seconds)
                           for it in items])
    by_day = {}
    for it in sorted(items, key=lambda it: (it.when, it.record_id)):
        by_day.setdefault(it.day, []).append(it)
    grid = MonthGrid()
    grid.set_month(2026, 3, days=index.days(2026, 3), items=by_day,
                   milestones=index.milestone_days(2026, 3), today=date(*TODAY), first_weekday=6)
    grid.set_selected(date(2026, 3, 12), animate=False)
    return grid, (760, 560), None


def calendar_month_picker_months(variant):
    from ui_qt.widgets.history_calendar import MonthPicker

    _pin()
    picker = MonthPicker()
    picker.set_data({(2025, 11): 4, (2025, 12): 9, (2026, 1): 6, (2026, 2): 4, (2026, 3): 11},
                    (2026, 3), (2026, 3))
    # As popup() leaves it, without showing it under an anchor: this year, this month in focus.
    picker._year = 2026
    picker._focus = 2
    size = picker.sizeHint()
    return picker, (size.width(), size.height()), None


# ---- the Stats window and the History export dialog (M2.27) ----
#
# The Stats window shows a summary counted from a seeded table: 40 days back from today
# (2026-03-15) in UTC, with dictations into several apps, a few files, a walkthrough, actions and
# a command, written by dictation_stats.record into the isolated database after a reset. Its own
# count (a worker started when it shows) is replaced by nothing, so the seeded summary stays. The
# weeks of the charts start on the en_US locale's Sunday, pinned on both sides. OpenWhisper_CPP's
# product name replaces OpenWhisper's in the intro and the features heading.
#
# The export dialog lists five entries given as export dicts; its default output path sits in a
# fixed folder under OpenWhisper_CPP's file name, and its hidden date edits hold fixed dates.

STATS_APPS = ("Slack", "Outlook", "Notes", "Word", "Chrome")
STATS_CATEGORIES = ("work", "email", "personal", "")
STATS_CLEANUP = ("light", "medium", "")
EXPORT_ROOT = r"C:\Users\Dana\AppData\Local\OpenWhisper_CPP"
EXPORT_ENTRIES = (
    # id, formatted timestamp, preview
    ("export-1", "Mar 15, 2026 03:00 PM", "Hello from OpenWhisper_CPP."),
    ("export-2", "Mar 14, 2026 08:05 PM", "Remember the birthday card."),
    ("export-3", "Mar 14, 2026 06:10 PM", "Pick up the bike from the shop."),
    ("export-4", "Mar 13, 2026 09:30 AM", "Send the invoice before Friday."),
    ("export-5", "Mar 11, 2026 12:00 PM", "Today we look at how rivers shape their valleys."),
)


def _pin_locale():
    from PyQt6.QtCore import QLocale

    QLocale.setDefault(QLocale(QLocale.Language.English, QLocale.Country.UnitedStates))


def _stats_rows():
    """The seeded stats rows: (fields for dictation_stats.record), oldest last."""
    from datetime import date, timedelta

    today = date(*TODAY)
    rows = []
    for back in range(40):
        if back % 9 == 4:
            continue
        day = today - timedelta(days=back)
        for index in range(1 + back % 3):
            count = 20 + (back * 7 + index * 13) % 60
            text = " ".join(["word"] * count)
            fields = {
                "text": text,
                "raw_text": text if index % 2 else text + " um uh",
                "entry_kind": "dictation",
                "audio_duration": count / 2,
                "app_name": STATS_APPS[(back + index) % len(STATS_APPS)],
                "app_category": STATS_CATEGORIES[(back + index) % len(STATS_CATEGORIES)],
                "cleanup_level": STATS_CLEANUP[(back * 2 + index) % len(STATS_CLEANUP)],
                "language": "it" if (back + index) % 6 == 0 else "en",
                "timestamp": f"{day.isoformat()}T{8 + (back * 3 + index * 5) % 11:02d}:"
                             f"{(index * 17) % 60:02d}:00+00:00",
            }
            if back == 0 and index == 0:
                fields["stats_counts"] = {"snippets": 1, "snippet_words": 8, "corrections": 2}
            rows.append(fields)
        stamp = f"{day.isoformat()}T13:00:00+00:00"
        if back in (2, 9, 25):
            rows.append({"text": "a long talk about the river and its valley",
                         "entry_kind": "file", "audio_duration": 600.0 * (1 + back % 3),
                         "timestamp": stamp})
        if back == 5:
            rows.append({"text": "steps", "entry_kind": "walkthrough", "audio_duration": 120.0,
                         "stats_counts": {"shots": 4}, "timestamp": stamp})
        if back in (1, 3, 12):
            rows.append({"text": "send it", "entry_kind": "action",
                         "stats_counts": {"target": "Slack" if back != 12 else "Notes"},
                         "timestamp": stamp})
        if back == 3:
            rows.append({"text": "Formal now.", "raw_text": "formal nw", "entry_kind": "command",
                         "timestamp": stamp})
    return rows


def _stats_summary():
    from datetime import date, timezone

    from services import dictation_stats as stats

    stats.reset()
    for fields in _stats_rows():
        stats.record(fields, None)
    return stats.load_summary(today=date(*TODAY), tz=timezone.utc)


def _stats_dialog(summary, variant):
    from PyQt6.QtWidgets import QLabel

    from ui_qt.dialogs import stats_dialog

    _pin_locale()
    # The count the window starts as it shows would replace the pinned summary.
    stats_dialog._run = lambda work, token: None
    dialog = stats_dialog.StatsDialog()
    dialog.findChild(QLabel, "statsIntro").setText(
        "What you do with OpenWhisper_CPP, counted and kept on this computer.")
    for label in dialog.findChildren(QLabel, "statsSectionTitle"):
        if label.text() == "Across OpenWhisper":
            label.setText("Across OpenWhisper_CPP")
    dialog._apply(0, summary, "")
    scale = variant["scale"] / 100
    return dialog, (round(640 * scale), round(720 * scale)), None


def stats_dialog_week(variant):
    return _stats_dialog(_stats_summary(), variant)


def stats_dialog_empty(variant):
    from services import dictation_stats as stats

    return _stats_dialog(stats.StatsSummary(), variant)


def _export_entries():
    return [{"id": record_id, "formatted_timestamp": stamp, "preview_text": preview,
             "text": preview, "timestamp": "2026-03-15T15:00:00+00:00", "has_audio": False}
            for record_id, stamp, preview in EXPORT_ENTRIES]


def _export_dialog(variant):
    from PyQt6.QtCore import QDate

    from ui_qt.dialogs.history_export_dialog import HistoryExportDialog

    entries = _export_entries()
    # The default path is made as the dialog is built: OpenWhisper_CPP's file name in a fixed folder.
    with (
        patch.object(HistoryExportDialog, "_DEFAULT_FILE_STEM", "openwhisper_cpp_history"),
        patch.object(HistoryExportDialog, "_export_root", lambda self: EXPORT_ROOT),
    ):
        dialog = HistoryExportDialog(entry_provider=lambda: entries)
    dialog.from_date.setDate(QDate(2026, 2, 15))
    dialog.to_date.setDate(QDate(2026, 3, 15))
    return dialog


def _export_size(dialog):
    hint = dialog.sizeHint()
    return max(dialog.minimumWidth(), min(dialog.maximumWidth(), hint.width())), hint.height()


def history_export_dialog_all(variant):
    dialog = _export_dialog(variant)
    return dialog, _export_size(dialog), None


def history_export_dialog_selected(variant):
    from PyQt6.QtCore import Qt

    dialog = _export_dialog(variant)
    dialog.selected_radio.setChecked(True)
    for row in (0, 2, 4):
        dialog.selected_list.item(row).setCheckState(Qt.CheckState.Checked)
    return dialog, _export_size(dialog), None


BUILDERS = {
    "history_item/dictation": history_item_dictation,
    "history_item/failed": history_item_failed,
    "history_sidebar/expanded": history_sidebar_expanded,
    "history_edge_tab/collapsed": history_edge_tab_collapsed,
    "history_edge_tab/expanded": history_edge_tab_expanded,
    "history_entry_dialog/dictation": history_entry_dialog_dictation,
    "history_delete_confirm/with-audio": history_delete_confirm_with_audio,
    "history_calendar_dialog/march": history_calendar_dialog_march,
    "calendar_month_grid/crowded-day": calendar_month_grid_crowded_day,
    "calendar_month_picker/months": calendar_month_picker_months,
    "stats_dialog/week": stats_dialog_week,
    "stats_dialog/empty": stats_dialog_empty,
    "history_export_dialog/all": history_export_dialog_all,
    "history_export_dialog/selected": history_export_dialog_selected,
}
