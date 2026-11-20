"""Reference builders of the a02 leaf widgets (fv_ui_widgets, M0.10).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data.
Nothing here starts a timer's worth of time: animations are driven to their end values
(animate=False), and render_reference.py processes events once without running an event loop.
"""

PATH = r"C:\Users\someone\Documents\Recordings\meeting-2026-11-14.wav"


def empty_state(variant):
    from PyQt6.QtWidgets import QPushButton

    from ui_qt.widgets.empty_state import EmptyState

    empty = EmptyState("No snippets yet", "Type a trigger phrase and the text it expands to.")
    button = QPushButton("Add snippet")
    empty.add_action(button)
    # The button is the first focusable widget, so it takes focus on show; pinned explicitly.
    return empty, (420, 150), button


def button_family(variant):
    from PyQt6.QtWidgets import QHBoxLayout, QWidget

    from ui_qt.widgets.buttons import (
        Button,
        DangerButton,
        PrimaryButton,
        SuccessButton,
        WarningButton,
    )

    host = QWidget()
    row = QHBoxLayout(host)
    for button in (Button("Cancel"), PrimaryButton("Start"), DangerButton("Delete"),
                   SuccessButton("Save"), WarningButton("Pause")):
        row.addWidget(button)
        if isinstance(button, WarningButton):
            button.set_active(False)
    return host, (760, 80), None


def header_card(variant):
    from PyQt6.QtWidgets import QLabel

    from ui_qt.widgets.cards import HeaderCard

    card = HeaderCard("Transcription", collapsible=True)
    card.add_content_widget(QLabel("Text"))
    return card, (420, 200), None


def settings_switch(variant):
    from PyQt6.QtWidgets import QHBoxLayout, QWidget

    from ui_qt.widgets.settings_switch import SettingsSwitch

    host = QWidget()
    row = QHBoxLayout(host)
    row.setContentsMargins(8, 6, 8, 6)
    row.setSpacing(12)
    for checked, enabled in ((True, True), (False, True), (True, False)):
        switch = SettingsSwitch()
        switch.setChecked(checked)
        switch.setEnabled(enabled)
        row.addWidget(switch)
    row.addStretch()
    return host, (200, 40), None


def animated_progress_bar(variant):
    from ui_qt.widgets.animated_progress_bar import AnimatedProgressBar

    bar = AnimatedProgressBar()
    bar.set_fraction(0.4, animate=False)
    return bar, (300, 8), None


def eliding_label(variant):
    from ui_qt.widgets.eliding_label import ElidingLabel

    return ElidingLabel(PATH), (160, 24), None


def wrapped_label(variant):
    from ui_qt.widgets.wrapped_label import WrappedLabel

    return WrappedLabel(PATH, break_long_words=True), (200, 80), None


def feature_badge(variant):
    from PyQt6.QtWidgets import QHBoxLayout, QWidget

    from ui_qt.widgets.buttons import PrimaryButton
    from ui_qt.widgets.feature_badge import FeatureBadge

    host = QWidget()
    row = QHBoxLayout(host)
    row.setContentsMargins(0, 0, 0, 0)
    button = PrimaryButton("Walkthrough")
    row.addWidget(button)
    badge = FeatureBadge(button, "Opens a guided tour of this screen.")
    badge.set_new(True)
    return host, (260, 48), None


def no_wheel_spin_box(variant):
    from ui_qt.widgets.no_wheel import NoWheelSpinBox

    box = NoWheelSpinBox()
    box.setRange(0, 10)
    box.setValue(5)
    return box, (120, 32), None


def eliding_combo_box(variant):
    from ui_qt.widgets.no_wheel import ElidingComboBox

    combo = ElidingComboBox()
    combo.addItems(["Parakeet TDT 0.6B v3 (multilingual)", "Small"])
    return combo, (170, 32), None


def update_notice(variant):
    from ui_qt.widgets.update_notice import UpdateNoticeButton

    button = UpdateNoticeButton()
    button.show_version("2.9.0")
    return button, (180, 32), None


def update_notice_compact(variant):
    from ui_qt.widgets.update_notice import UpdateNoticeButton

    button = UpdateNoticeButton()
    button.set_compact(True)
    button.show_version("2.9.0")
    return button, (180, 32), None


def _tabs():
    from PyQt6.QtWidgets import QWidget

    from ui_qt.widgets.tabbed_content import TabbedContentWidget

    tabs = TabbedContentWidget()
    for title in ("Quick Record", "Upload File", "Walkthrough", "Meeting Mode"):
        tabs.add_tab(QWidget(), title)
    tabs.sync_stack_with_tab_bar()
    return tabs


def tab_bar(variant):
    return _tabs(), (720, 60), None


def tab_bar_recording(variant):
    tabs = _tabs()
    tabs.set_recording_state(True, 0)
    return tabs, (720, 60), None


def collapsible_toggle(variant):
    from ui_qt.widgets.collapsible_header import CollapsibleSectionToggle

    return CollapsibleSectionToggle("Transcription", expanded=True), (260, 28), None


def collapsible_toggle_collapsed(variant):
    from ui_qt.widgets.collapsible_header import CollapsibleSectionToggle

    return CollapsibleSectionToggle("Transcription", expanded=False), (260, 28), None


def downloads_label(variant):
    from ui_qt.widgets.downloads_label import DownloadsLabel

    return DownloadsLabel("The model is missing. Get it in Downloads."), (200, 24), None


def decode_label(variant):
    from ui_qt.widgets.decode_label import DecodeLabel

    label = DecodeLabel()
    # Two spaces in a row are pinned as &nbsp;&nbsp; so the separator keeps its width.
    label.set_segments(
        [("Language: ", False), ("English", True), ("  \u00b7  ", False), ("99% match", True)],
        animate=False,
    )
    return label, (300, 24), None


def hotkey_hover_hint(variant):
    from ui_qt.widgets.buttons import HotkeyHoverHint

    # The pill alone, at the size it takes for this text; show() starts its fade-in, which grab()
    # does not see (window opacity is not painted into the widget).
    hint = HotkeyHoverHint()
    hint.set_hotkey("Ctrl+Shift+R")
    return hint, (112, 28), None


# Composite widgets (M2.1).


def field_help_card_open(variant):
    from ui_qt.widgets.field_help import FieldHelp

    help_button = FieldHelp(
        "Model", "The speech model dictation uses.",
        [("Open Settings → Downloads", "downloads")],
    )
    # The caption is its own window here, so show_help hosts the card on it; the card is the
    # grabbed widget, at the size show_help gave it. The caption must outlive the card.
    help_button.show()
    help_button.show_help()
    card = help_button.card
    card._caption = help_button
    return card, (card.width(), card.height()), None


_LOOKS = (
    ("Ribbon", "A glowing trace of your voice"),
    ("Bars", "A scrolling level meter"),
    ("Mirror wave", "Your voice's outline, mirrored"),
    ("Aurora", "Flowing waves"),
    ("Pulse", "A ring that swells and ripples"),
    ("Dots", "Dots that ripple as you speak"),
    ("Bait ball", "A spinning school lifts the swell"),
    ("Real fish", "Fish school under the waves"),
    ("Deep-sea glow", "Glowing fish in the deep"),
    ("Fizz", "Glasses filling with fizz"),
    ("Classic", "The original particles"),
)


def segmented_bar_looks(variant):
    from ui_qt.widgets.segmented_bar import SegmentedBar

    # The 11 looks of RecordingOverlayLook, in its order, with Bars current.
    bar = SegmentedBar(list(_LOOKS))
    bar.setCurrentIndex(1)
    return bar, (640, 352), None


def segmented_bar_compact(variant):
    from ui_qt.widgets.segmented_bar import SegmentedBar

    return SegmentedBar([("Header", ""), ("Footer", "")], compact=True), (300, 36), None


def _tile_size(variant, height_100, height_130):
    return (480, height_100 if variant["scale"] == 100 else height_130)


def setting_tile_toggle(variant):
    from ui_qt.utils.icons import design_icon
    from ui_qt.widgets.setting_tile import SettingTile

    tile = SettingTile(
        "Paste into the active window",
        "Types the transcript where your cursor is once dictation finishes.",
        design_icon("bolt-green.svg"),
    )
    tile.checkbox.setChecked(True)
    return tile, _tile_size(variant, 72, 82), None


def setting_tile_field(variant):
    from ui_qt.utils.icons import design_icon
    from ui_qt.widgets.no_wheel import NoWheelComboBox
    from ui_qt.widgets.setting_tile import FieldTile

    combo = NoWheelComboBox()
    combo.addItems(["Default (100%)", "Large (115%)", "Extra large (130%)"])
    tile = FieldTile("Text size", "How large text is across the app.", combo,
                     design_icon("typography-blue.svg"))
    return tile, _tile_size(variant, 112, 128), None


def setting_tile_notice(variant):
    from ui_qt.utils.icons import design_icon
    from ui_qt.widgets.buttons import Button
    from ui_qt.widgets.setting_tile import InfoTile

    tile = InfoTile("Microphone access is off",
                    "Turn on microphone access for desktop apps to record.",
                    design_icon("microphone-blue.svg"))
    tile.setProperty("kind", "notice")
    tile.add_actions(Button("Open privacy settings"))
    return tile, _tile_size(variant, 120, 136), None


def setting_tile_intro(variant):
    from ui_qt.utils.icons import design_icon
    from ui_qt.widgets.buttons import Button, neutral_button
    from ui_qt.widgets.setting_tile import InfoTile

    tile = InfoTile("What's new",
                    "Live preview shows your words while you speak, and long dictations paste "
                    "as fast as short ones.",
                    design_icon("wand-purple.svg"))
    tile.setProperty("kind", "intro")
    tile.add_trailing(neutral_button(Button("Got it")))
    return tile, _tile_size(variant, 96, 112), None


_RAIL = (
    ("Dictation", "microphone-blue.svg", (
        ("recording", "Recording"), ("voice_model", "Voice model"), ("cleanup", "AI cleanup"),
        ("remote_engine", "Remote engine"), ("walkthroughs", "Walkthroughs"))),
    ("Personalize", "wand-purple.svg", (
        ("dictionary", "Dictionary"), ("snippets", "Snippets"), ("styles", "Apps & styles"),
        ("cleanup_rules", "Learned rules"), ("cleanup_profiles", "Profiles"),
        ("commands", "Commands"))),
    ("Automate", "bolt-green.svg", (
        ("actions", "Actions"), ("hooks", "Hooks"), ("mcp", "API & MCP"))),
    ("Meeting Mode", "users-amber.svg", (
        ("meeting_voice", "Voice & speakers"), ("meeting_intelligence", "Intelligence"),
        ("meeting_fast", "Fast judgments"), ("meeting_after", "After the meeting"),
        ("meeting_dashboard", "Dashboard"))),
    ("Models & storage", "download-blue.svg", (
        ("downloads", "Downloads"), ("runtime", "Runtime"), ("backup", "Backup & restore"))),
    ("App", "adjustments-slate.svg", (
        ("general", "General"), ("appearance", "Appearance"), ("hotkeys", "Hotkeys"),
        ("api_keys", "API keys"), ("advanced", "Developer options"))),
)


def nav_rail_dictation_open(variant):
    from ui_qt.utils.icons import design_icon
    from ui_qt.widgets.nav_rail import NavRail

    # As the Settings window builds it (settings_dialog._CHAPTERS, appkit/destinations.h).
    rail = NavRail()
    rail.add_destination("overview", "Home", design_icon("home-blue.svg"))
    for chapter, icon, pages in _RAIL:
        rail.add_chapter(chapter, design_icon(icon))
        for key, name in pages:
            rail.add_destination(key, name)
    rail.set_value("recording", "Auto-paste on")
    rail.set_value("appearance", "Dark · Bars")
    rail.select("recording")
    return rail, (NavRail.RAIL_WIDTH, 440 if variant["scale"] == 100 else 560), None


def searchable_combo_badge(variant):
    from ui_qt.widgets.searchable_combo import SearchableComboBox

    combo = SearchableComboBox()
    combo.addItems(["gpt-4o-mini", "gpt-4o"])
    combo.set_badge("Active")
    return combo, (260, 32), None


def button_row_three(variant):
    from ui_qt.widgets.button_row import ButtonRow
    from ui_qt.widgets.buttons import Button

    row = ButtonRow([Button("Open model folder"), Button("Copy path"), Button("Show details")])
    return row, (300, 84), None


def reflow_stacked(variant):
    from PyQt6.QtWidgets import QLabel

    from ui_qt.widgets.buttons import PrimaryButton
    from ui_qt.widgets.reflow import Reflow

    return Reflow(QLabel("Snippets expand as you type"), PrimaryButton("Add snippet")), \
        (240, 72), None


BUILDERS = {
    "empty_state": empty_state,
    "button_family": button_family,
    "header_card": header_card,
    "settings_switch": settings_switch,
    "animated_progress_bar": animated_progress_bar,
    "eliding_label": eliding_label,
    "wrapped_label": wrapped_label,
    "feature_badge": feature_badge,
    "no_wheel_spin_box": no_wheel_spin_box,
    "eliding_combo_box": eliding_combo_box,
    "update_notice": update_notice,
    "update_notice_compact": update_notice_compact,
    "tab_bar": tab_bar,
    "tab_bar_recording": tab_bar_recording,
    "collapsible_toggle": collapsible_toggle,
    "collapsible_toggle_collapsed": collapsible_toggle_collapsed,
    "downloads_label": downloads_label,
    "decode_label": decode_label,
    "hotkey_hover_hint": hotkey_hover_hint,
    "field_help_card/open": field_help_card_open,
    "segmented_bar/looks": segmented_bar_looks,
    "segmented_bar/compact": segmented_bar_compact,
    "setting_tile/toggle": setting_tile_toggle,
    "setting_tile/field": setting_tile_field,
    "setting_tile/notice": setting_tile_notice,
    "setting_tile/intro": setting_tile_intro,
    "nav_rail/dictation-open": nav_rail_dictation_open,
    "searchable_combo/badge": searchable_combo_badge,
    "button_row/three": button_row_three,
    "reflow/stacked": reflow_stacked,
}
