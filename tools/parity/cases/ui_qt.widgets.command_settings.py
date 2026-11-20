"""Curated inputs for ui_qt/widgets/command_settings.py (M4.4 text-prompts): the Command Mode
hint for each recording mode.
"""

CASES = {
    "command_mode_hint": [[settings] for settings in (
        {}, {"recording_trigger_mode": "toggle"}, {"recording_trigger_mode": "push_hold"},
        {"recording_trigger_mode": "PUSH_HOLD"}, {"recording_trigger_mode": 1},
    )],
}
