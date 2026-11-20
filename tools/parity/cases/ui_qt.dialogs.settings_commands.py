"""Curated inputs for ui_qt/dialogs/settings_commands.py (M4.4 text-prompts): the Commands
rail value over the Command Mode shortcut and the transforms library.
"""

_ROW = {"id": "a", "name": "A", "instruction": "Do it.", "hotkey": ""}

CASES = {
    "rail_value": [[settings] for settings in (
        {},
        {"hotkeys": {"command_mode": "ctrl+alt+k"}},
        {"hotkeys": {"command_mode": ""}},
        {"hotkeys": {"command_mode": 7}},
        {"hotkeys": "oops"},
        {"hotkeys": None},
        {"text_transforms": []},
        {"text_transforms": [_ROW]},
        {"text_transforms": [_ROW, {**_ROW, "id": "b", "name": "B"}],
         "hotkeys": {"command_mode": "f9", "record_toggle": "f8"}},
        {"text_transforms": "oops"},
    )],
}
