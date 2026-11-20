"""Curated inputs for services/agent_mcp/controls.py (M5.9): the preferences an agent may see and
change, and what the token holder may do.

    validate(key, value)            SETTING_CONTROLS[key].validate: {"value": v} or {"error": text}
    resolve(key, saved)             SETTING_CONTROLS[key].resolve(saved)
    describe(key)                   SETTING_CONTROLS[key].describe()
    controls()                      every control's key, in SETTING_CONTROLS order
    writable_settings(saved)        sorted(writable_settings(saved))
    capabilities(saved)             AgentControls over in-memory settings: capabilities()
    get_settings(saved)             get_settings(): the result or {"error": text}
    update_settings(saved, changes) {"result" or "error", "saved": the settings afterwards}
    constants()                     WRITE_CAPABILITIES, LIVE

The settings are tests/fakes/settings.py's InMemorySettings; no database is opened (retitling is
covered by the agent_mcp transcript and tests/agentapi/test_agent_controls.cpp).
"""


def _control(controls, key):
    return controls.CONTROLS_BY_KEY[key]


def _validate(controls, key, value):
    try:
        return {"value": _control(controls, key).validate(value)}
    except controls.ControlError as exc:
        return {"error": str(exc)}


def _resolve(controls, key, saved):
    return _control(controls, key).resolve(saved)


def _describe(controls, key):
    return _control(controls, key).describe()


def _controls(controls):
    return [control.key for control in controls.SETTING_CONTROLS]


def _writable(controls, saved):
    return sorted(controls.writable_settings(saved))


def _agent(controls, saved):
    from tests.fakes.settings import InMemorySettings

    return controls.AgentControls("unused.db", InMemorySettings(dict(saved))), None


def _capabilities(controls, saved):
    agent, _ = _agent(controls, saved)
    return agent.capabilities()


def _get_settings(controls, saved):
    agent, _ = _agent(controls, saved)
    try:
        return agent.get_settings()
    except controls.ControlError as exc:
        return {"error": str(exc)}


def _update_settings(controls, saved, changes):
    from tests.fakes.settings import InMemorySettings

    settings = InMemorySettings(dict(saved))
    agent = controls.AgentControls("unused.db", settings)
    try:
        outcome = {"result": agent.update_settings(changes)}
    except controls.ControlError as exc:
        outcome = {"error": str(exc)}
    outcome["saved"] = settings.values
    return outcome


def _constants(controls):
    return {"WRITE_CAPABILITIES": list(controls.WRITE_CAPABILITIES), "LIVE": controls.LIVE}


WRAPPERS = {
    "validate": _validate,
    "resolve": _resolve,
    "describe": _describe,
    "controls": _controls,
    "writable_settings": _writable,
    "capabilities": _capabilities,
    "get_settings": _get_settings,
    "update_settings": _update_settings,
    "constants": _constants,
}

KEYS = [
    "auto_paste", "restore_clipboard", "copy_clipboard", "recording_trigger_mode",
    "local_asr_language", "streaming_enabled", "streaming_overlay_font_size",
    "streaming_overlay_layout", "recording_overlay_look", "recording_overlay_dot",
    "recording_overlay_text", "recording_overlay_clock", "ui_theme", "ui_font_scale",
    "minimize_tray", "transcript_cleanup_enabled", "transcript_cleanup_level",
    "transcript_cleanup_reasoning", "meeting_language", "meeting_report_ribbon",
    "meeting_report_brief", "meeting_report_signal", "confirm_history_entry_delete",
    "confirm_meeting_delete", "update_check_enabled", "update_notify_enabled",
]

GRANTED = {
    "mcp_settings_access": True,
    "mcp_writable_settings": {"auto_paste": True, "ui_font_scale": True, "ui_theme": True,
                              "streaming_enabled": True, "streaming_overlay_font_size": True,
                              "local_asr_language": True, "mcp_enabled": True},
}

CASES = {
    "validate": [
        ["auto_paste", True], ["auto_paste", False], ["auto_paste", 1], ["auto_paste", "true"],
        ["auto_paste", None], ["auto_paste", 0.0], ["auto_paste", [True]],
        ["ui_font_scale", 100], ["ui_font_scale", 115], ["ui_font_scale", 101],
        ["ui_font_scale", True], ["ui_font_scale", 100.0], ["ui_font_scale", "100"],
        ["ui_font_scale", None],
        ["streaming_overlay_font_size", 10], ["streaming_overlay_font_size", 48],
        ["streaming_overlay_font_size", 9], ["streaming_overlay_font_size", 49],
        ["streaming_overlay_font_size", 20.0], ["streaming_overlay_font_size", False],
        ["ui_theme", "light"], ["ui_theme", "dark"], ["ui_theme", "system"],
        ["ui_theme", "omarchy"], ["ui_theme", None], ["ui_theme", "neon"], ["ui_theme", "x" * 21],
        ["ui_theme", 5],
        ["local_asr_language", "en"], ["local_asr_language", "auto"],
        ["local_asr_language", "xx"], ["local_asr_language", "e" * 11],
        ["recording_overlay_look", "orb"], ["recording_overlay_look", "pulse"],
        ["recording_trigger_mode", "push_hold"], ["recording_trigger_mode", "hold"],
        ["transcript_cleanup_level", "high"], ["transcript_cleanup_reasoning", "ultra"],
        ["meeting_language", "AUTO"], ["meeting_language", "de"],
        ["streaming_overlay_layout", "footer"], ["streaming_overlay_layout", ""],
    ],
    "resolve": [[key, {}] for key in KEYS] + [
        ["auto_paste", {"auto_paste": False}],
        ["auto_paste", {"auto_paste": "no"}],
        ["ui_font_scale", {"ui_font_scale": 130}],
        ["ui_font_scale", {"ui_font_scale": 131}],
        ["ui_font_scale", {"ui_font_scale": "130"}],
        ["streaming_overlay_font_size", {"streaming_overlay_font_size": "big"}],
        ["streaming_overlay_font_size", {"streaming_overlay_font_size": 47}],
        ["ui_theme", {"ui_theme": "light"}],
        ["ui_theme", {"ui_theme": "neon"}],
        ["ui_theme", {"ui_theme": None}],
        ["recording_overlay_look", {"recording_overlay_look": "orb"}],
        ["local_asr_language", {"local_asr_language": "fr"}],
        ["local_asr_language", {"local_asr_language": 5}],
        ["meeting_language", {"meeting_language": "de"}],
    ],
    "describe": [[key] for key in KEYS],
    "controls": [[]],
    "writable_settings": [
        [{}],
        [{"mcp_writable_settings": {"auto_paste": True}}],
        [{"mcp_writable_settings": {"auto_paste": "yes", "ui_theme": 1, "minimize_tray": True}}],
        [{"mcp_writable_settings": {"mcp_enabled": True, "nope": True, "ui_font_scale": True}}],
        [{"mcp_writable_settings": ["auto_paste"]}],
        [{"mcp_writable_settings": "auto_paste"}],
        [{"mcp_writable_settings": None}],
    ],
    "capabilities": [
        [{}],
        [GRANTED],
        [{"mcp_settings_access": False, "mcp_writable_settings": {"auto_paste": True}}],
        [{"mcp_retitle_transcriptions": True, "mcp_retitle_meetings": "yes",
          "api_run_actions": True, "api_live_events": False, "mcp_events_enabled": True}],
        [{"api_control_recording": True, "api_text_filters": True, "api_agent_prompts": True}],
    ],
    "get_settings": [
        [{}],
        [{"mcp_settings_access": "yes"}],
        [GRANTED],
        [dict(GRANTED, ui_theme="light", auto_paste=False, ui_font_scale=999,
              recording_overlay_look="orb", local_asr_language=None)],
    ],
    "update_settings": [
        [{}, {"auto_paste": False}],
        [GRANTED, {"auto_paste": False, "ui_font_scale": 115}],
        [dict(GRANTED, ui_theme="dark"), {"ui_theme": "light"}],
        [dict(GRANTED, ui_theme="dark"), {"ui_theme": None}],
        [GRANTED, {"ui_theme": None}],
        [GRANTED, {"ui_theme": "light"}],
        [dict(GRANTED, streaming_overlay_enabled=True, streaming_paste_enabled=False,
              live_typing_enabled=True), {"streaming_enabled": True}],
        [GRANTED, {"minimize_tray": False}],
        [GRANTED, {"auto_paste": True, "minimize_tray": False}],
        [GRANTED, {"auto_paste": "true"}],
        [GRANTED, {"ui_font_scale": 101}],
        [GRANTED, {"streaming_overlay_font_size": 60}],
        [GRANTED, {"streaming_overlay_font_size": 5}],
        [GRANTED, {"local_asr_language": "xx"}],
        [GRANTED, {"mcp_enabled": True}],
        [GRANTED, {}],
        [GRANTED, {f"k{i}": True for i in range(27)}],
        [GRANTED, "not a dict"],
        [dict(GRANTED, mcp_settings_access=False), {"auto_paste": False}],
        [GRANTED, {"auto_paste": None}],
    ],
    "constants": [[]],
}
