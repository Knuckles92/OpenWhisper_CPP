"""Curated inputs for ui_qt/dialogs/settings_fast_judgments.py (fv_ui_settings, M5.19).

The section is a QObject over widgets, so every vector runs a real FastJudgmentsSection built
over a settings dict and with its key lookup replaced (_key_present answers from a table of which
route has a key), and records what it would show:

- rail_value: the rail's text for (settings, whether the chosen route's key resolves);
- _destination_copy and _text_model_label: the route that the settings choose, and the text model
  route they name;
- render_key_state: the key notice (shown or not, its title and description, the "Use {other}"
  button and its text, the Add a key button's tone and tooltip) for (settings, keys present by
  route id), over recorders standing in for the widgets;
- _render_route: the Judge tile's route bar and text-model bar details, the tile's description
  and the destination line, for the same arguments.
"""
from __future__ import annotations

from types import SimpleNamespace

KEY_FEATURES = (
    "typesafe_topic_shift_enabled",
    "typesafe_voice_commands_enabled",
    "typesafe_citations_enabled",
    "typesafe_semantic_search_enabled",
    "typesafe_question_radar_enabled",
    "typesafe_highlights_enabled",
)

ON = {"typesafe_enabled": True}
OPENROUTER = {"typesafe_provider": "openrouter"}
TEXT_MODEL = {"typesafe_provider": "text_model"}
LOCAL = {"meeting_llm_provider": "ollama", "meeting_llm_model": "qwen3:4b"}
LM_STUDIO = {"meeting_llm_provider": "lm_studio", "meeting_llm_model": "qwen3-4b"}
HOSTED = {"meeting_llm_provider": "openrouter", "meeting_llm_model": "openai/gpt-5-mini"}
CLEANUP = {"typesafe_text_model": "cleanup", "transcript_cleanup_provider": "openrouter",
           "transcript_cleanup_model": "anthropic/claude-sonnet-4.5"}
CLEANUP_LOCAL = {"typesafe_text_model": "cleanup", "transcript_cleanup_provider": "lm_studio",
                 "transcript_cleanup_model": "llama3.2"}
NO_FEATURES = {name: False for name in KEY_FEATURES}

SETTINGS = [
    {},
    ON,
    {**ON, **NO_FEATURES},
    {**ON, **NO_FEATURES, "typesafe_highlights_enabled": True},
    {**ON, **{name: True for name in KEY_FEATURES}},
    {**ON, **NO_FEATURES, "typesafe_citations_enabled": True,
     "typesafe_question_radar_enabled": True},
    {"typesafe_enabled": False, "typesafe_citations_enabled": True},
    {**ON, **OPENROUTER},
    {**ON, **OPENROUTER, **NO_FEATURES},
    {**ON, **OPENROUTER, "typesafe_voice_commands_enabled": True},
    {**ON, **TEXT_MODEL, **LOCAL},
    {**ON, **TEXT_MODEL, **LOCAL, **NO_FEATURES},
    {**ON, **TEXT_MODEL, **LM_STUDIO, "typesafe_highlights_enabled": True},
    {**ON, **TEXT_MODEL, **HOSTED},
    {**ON, **TEXT_MODEL, **HOSTED, **NO_FEATURES},
    {**ON, **TEXT_MODEL, **HOSTED, **CLEANUP},
    {**ON, **TEXT_MODEL, **LOCAL, **CLEANUP},
    {**ON, **TEXT_MODEL, **LOCAL, **CLEANUP_LOCAL},
    {**TEXT_MODEL, **LOCAL},
    {**OPENROUTER},
    {"typesafe_provider": "unknown-route"},
    {"typesafe_enabled": "yes", "typesafe_provider": "text_model", "meeting_llm_provider": "nope"},
]

#: Which routes have a key: {route id: bool}.
KEYS = [
    {"typesafe": False, "openrouter": False, "text_model": False},
    {"typesafe": True, "openrouter": False, "text_model": False},
    {"typesafe": False, "openrouter": True, "text_model": False},
    {"typesafe": True, "openrouter": True, "text_model": False},
    {"typesafe": False, "openrouter": False, "text_model": True},
    {"typesafe": True, "openrouter": True, "text_model": True},
]


def _section(module, settings, present=None):
    section = module.FastJudgmentsSection(
        None,
        settings_snapshot=lambda: settings,
        persist=lambda key, value: None,
        bindings=None,
        focus_api_keys=lambda env: None,
        set_rail_value=lambda value: None,
    )
    if present is not None:
        section._key_present = lambda route=None: bool(
            present[(route or section.selected_route()).id])
    return section


def _rail_value(module, settings, key_present):
    section = _section(module, settings)
    section._key_present = lambda route=None: bool(key_present)
    return section.rail_value()


def _destination_copy(module, settings):
    section = _section(module, settings)
    return {"route": section.selected_route().id,
            "text": module.FastJudgmentsSection._destination_copy(section.selected_route())}


def _text_model_label(module, settings):
    section = _section(module, settings)
    route = module.typesafe_text_model_route(settings)
    return module.FastJudgmentsSection._text_model_label(route)


class _Recorder:
    """A widget stand-in that keeps what it is given."""

    def __init__(self, checked=False):
        self.visible = None
        self.text = None
        self.tooltip = None
        self.checked = checked
        self.description = None
        self.title_label = SimpleNamespace(setText=self._set_title)
        self.title = None
        self.index = None
        self.details = {}
        self.enabled = None

    def _set_title(self, text):
        self.title = text

    def setVisible(self, visible):
        self.visible = bool(visible)

    def setText(self, text):
        self.text = text

    def setToolTip(self, text):
        self.tooltip = text

    def isChecked(self):
        return self.checked

    def set_description(self, text):
        self.description = text

    def setCurrentIndex(self, index):
        self.index = index

    def set_detail(self, index, text):
        self.details[index] = text

    def setEnabled(self, enabled):
        self.enabled = bool(enabled)


def _render_key_state(module, settings, present):
    from services.settings import resolve_typesafe_enabled

    section = _section(module, settings, present)
    notice, switch, add = _Recorder(), _Recorder(), _Recorder()
    shown = {}
    section.__dict__.update(
        typesafe_key_notice=notice,
        typesafe_switch_route_btn=switch,
        open_typesafe_key_btn=add,
        typesafe_enabled_check=_Recorder(checked=resolve_typesafe_enabled(settings)),
    )
    section._render_route = lambda: None
    original = module.set_style_property
    module.set_style_property = lambda _widget, prop, value: shown.update({prop: value})
    try:
        section.render_key_state()
    finally:
        module.set_style_property = original
    return {
        "route": section.selected_route().id,
        "notice_visible": notice.visible,
        "title": notice.title,
        "description": notice.description,
        "switch_visible": switch.visible,
        "switch_text": switch.text,
        "add_tone": shown.get("tone"),
        "add_tooltip": add.tooltip,
    }


def _render_route(module, settings, present):
    section = _section(module, settings, present)
    route_bar, text_bar = _Recorder(), _Recorder()
    tile, label, test = _Recorder(), _Recorder(), _Recorder()
    section.__dict__.update(
        typesafe_route_bar=route_bar,
        typesafe_text_model_bar=text_bar,
        typesafe_route_tile=tile,
        typesafe_destination_label=label,
        typesafe_route_test_btn=test,
    )
    section._render_route()
    return {
        "route": section.selected_route().id,
        "route_index": route_bar.index,
        "route_details": [route_bar.details[index] for index in sorted(route_bar.details)],
        "text_model_visible": text_bar.visible,
        "text_model_index": text_bar.index,
        "text_model_details": [text_bar.details[index] for index in sorted(text_bar.details)],
        "description": tile.description,
        "destination": label.text,
        "test_enabled": test.enabled,
    }


WRAPPERS = {
    "FastJudgmentsSection.rail_value": _rail_value,
    "FastJudgmentsSection._destination_copy": _destination_copy,
    "FastJudgmentsSection._text_model_label": _text_model_label,
    "FastJudgmentsSection.render_key_state": _render_key_state,
    "FastJudgmentsSection._render_route": _render_route,
}

CASES = {
    "FastJudgmentsSection.rail_value": [
        [settings, key_present] for settings in SETTINGS for key_present in (False, True)
    ],
    "FastJudgmentsSection._destination_copy": [[settings] for settings in SETTINGS],
    "FastJudgmentsSection._text_model_label": [[settings] for settings in SETTINGS],
    "FastJudgmentsSection.render_key_state": [
        [settings, present] for settings in SETTINGS for present in KEYS
    ],
    "FastJudgmentsSection._render_route": [
        [settings, present] for settings in SETTINGS for present in KEYS
    ],
}

PINS = {
    "note": "each vector runs a real FastJudgmentsSection over the settings with _key_present "
            "answering from a table of which route has a key; widgets are recorders. "
            "route is the route id the settings choose",
}
