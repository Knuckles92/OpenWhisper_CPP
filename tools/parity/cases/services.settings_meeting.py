"""Curated inputs for the meeting-speech half of services/settings.py (M7.5): resolve_meeting_speech,
own_meeting_speech, meeting_model_label and keep_separate_meeting_speech.

The functions live in services.settings, whose own case file (services.settings.py) belongs to
another package; golden_vectors.py imports the module named by this file's stem, so this file
registers services.settings under that name and its vectors go to
tests/golden/services.settings_meeting.json.

Every call runs on a Parakeet platform, as FioriVoce is: the default meeting model is
"parakeet-v3" (config.MEETING_ASR_MODEL, as macOS has "parakeet-v3-mlx") and
parakeet_default_supported() is True. OpenWhisper's Windows build falls back to Local Whisper
"auto" instead, an engine FioriVoce does not have. Settings use only engines and models both
apps have (Parakeet, Orukeet, the API, the paired computer); the Whisper keys are left out.

resolve_meeting_speech(settings) and own_meeting_speech(settings) give the MeetingSpeech as a
dict. keep_separate_meeting_speech(saved, new_install) runs on a stand-in manager holding `saved`
whose is_new_install() is `new_install`, and gives what it returned, the value it saved (None:
not saved) and the meeting speech the saved settings then resolve to.
"""

import contextlib
import dataclasses
import importlib
import sys

_settings = importlib.import_module("services.settings")
sys.modules["services.settings_meeting"] = _settings

_KEY = "meeting_speech_separate"


@contextlib.contextmanager
def _parakeet_platform():
    import config

    key = _settings.SettingsKey.MEETING_ASR_MODEL
    setting = _settings.SETTINGS_BY_KEY[key]
    saved_default = setting._default
    saved_defaults = _settings.SETTING_DEFAULTS
    saved_supported = config.parakeet_default_supported
    setting._default = "parakeet-v3"
    _settings.SETTING_DEFAULTS = {**saved_defaults, key: "parakeet-v3"}
    config.parakeet_default_supported = lambda *args, **kwargs: True
    try:
        yield
    finally:
        setting._default = saved_default
        _settings.SETTING_DEFAULTS = saved_defaults
        config.parakeet_default_supported = saved_supported


def _resolve(module, settings):
    with _parakeet_platform():
        return dataclasses.asdict(module.resolve_meeting_speech(settings))


def _own(module, settings):
    with _parakeet_platform():
        return dataclasses.asdict(module.own_meeting_speech(settings))


def _label(module, model):
    with _parakeet_platform():
        return module.meeting_model_label(model)


class _Manager:
    """What keep_separate_meeting_speech reads of a SettingsManager, in memory."""

    def __init__(self, saved, new_install):
        self.saved = dict(saved)
        self.new_install = new_install

    def is_new_install(self):
        return self.new_install

    def load_all_settings(self):
        return dict(self.saved)

    def mutate_settings(self, mutator):
        settings = dict(self.saved)
        result = mutator(settings)
        self.saved = settings
        return result


def _keep(module, saved, new_install):
    with _parakeet_platform():
        manager = _Manager(saved, new_install)
        written = module.keep_separate_meeting_speech(manager)
        return {
            "written": written,
            "saved": manager.saved.get(_KEY),
            "after": dataclasses.asdict(module.resolve_meeting_speech(manager.saved)),
        }


_SETTINGS = [
    {},
    {"selected_model": "parakeet"},
    {"selected_model": "parakeet", "local_asr_models": {"parakeet": "orukeet-v0.1"}},
    {"selected_model": "parakeet", "local_asr_models": {"parakeet": "parakeet-v3"}},
    {"selected_model": "parakeet", "local_asr_models": {"parakeet": "nemotron-3.5"}},
    {"selected_model": "parakeet", "local_asr_models": {"parakeet": 7}},
    {"selected_model": "parakeet", "local_asr_models": "parakeet-v3"},
    {"local_asr_models": {"parakeet": "orukeet-v0.1"}},
    {"selected_model": "remote"},
    {"selected_model": "remote", "local_asr_models": {"parakeet": "orukeet-v0.1"}},
    {"selected_model": "api"},
    {"selected_model": "api", "local_asr_models": {"parakeet": "orukeet-v0.1"}},
    {"selected_model": "api_whisper"},
    {"selected_model": "api_gpt4o"},
    {"selected_model": "api_gpt4o_mini"},
    {"selected_model": 5},
    {"selected_model": None},
    {"selected_model": "parakeet", "meeting_asr_model": "orukeet-v0.1"},
    {"selected_model": "parakeet", "meeting_asr_model": "parakeet-v3",
     "local_asr_models": {"parakeet": "orukeet-v0.1"}},
    {"selected_model": "parakeet", "meeting_asr_model": "not-a-model"},
    {"selected_model": "parakeet", "meeting_asr_model": ""},
    {"selected_model": "parakeet", "meeting_asr_model": 12},
    {"selected_model": "parakeet", "meeting_asr_source": "remote"},
    {"selected_model": "api", "meeting_asr_model": "orukeet-v0.1"},
    {"selected_model": "remote", "meeting_asr_model": "parakeet-v3"},
    {_KEY: True},
    {_KEY: True, "selected_model": "remote"},
    {_KEY: True, "selected_model": "api"},
    {_KEY: True, "selected_model": "api", "local_asr_models": {"parakeet": "orukeet-v0.1"}},
    {_KEY: True, "meeting_asr_model": "orukeet-v0.1"},
    {_KEY: True, "meeting_asr_model": "orukeet-v0.1", "meeting_asr_source": "remote"},
    {_KEY: True, "meeting_asr_source": "remote", "selected_model": "parakeet"},
    {_KEY: True, "meeting_asr_source": "elsewhere"},
    {_KEY: True, "selected_model": 5},
    {_KEY: False, "selected_model": "remote"},
    {_KEY: "yes", "selected_model": "remote"},
    {_KEY: 1, "selected_model": "remote"},
]

_KEEP = [
    ({}, True),
    ({}, False),
    ({"selected_model": "parakeet"}, False),
    ({"selected_model": "parakeet"}, True),
    ({"selected_model": "parakeet", "meeting_asr_model": "parakeet-v3"}, False),
    ({"selected_model": "parakeet", "meeting_asr_model": "orukeet-v0.1"}, False),
    ({"selected_model": "parakeet", "meeting_asr_model": "orukeet-v0.1"}, True),
    ({"selected_model": "parakeet", "local_asr_models": {"parakeet": "orukeet-v0.1"},
      "meeting_asr_model": "orukeet-v0.1"}, False),
    ({"selected_model": "remote", "meeting_asr_model": "parakeet-v3"}, False),
    ({"selected_model": "remote", "meeting_asr_source": "remote"}, False),
    ({"selected_model": "parakeet", "meeting_asr_source": "remote"}, False),
    ({"selected_model": "api"}, False),
    ({"selected_model": "api", "meeting_asr_model": "orukeet-v0.1"}, False),
    ({"selected_model": "api_whisper"}, False),
    ({_KEY: True, "selected_model": "parakeet"}, False),
    ({_KEY: False, "selected_model": "remote", "meeting_asr_model": "parakeet-v3"}, False),
]

CASES = {
    "resolve_meeting_speech": [[settings] for settings in _SETTINGS],
    "own_meeting_speech": [[settings] for settings in _SETTINGS],
    "meeting_model_label": [["parakeet-v3"], ["orukeet-v0.1"]],
    "keep_separate_meeting_speech": [[saved, new] for saved, new in _KEEP],
}

WRAPPERS = {
    "resolve_meeting_speech": _resolve,
    "own_meeting_speech": _own,
    "meeting_model_label": _label,
    "keep_separate_meeting_speech": _keep,
}

PINS = {
    "platform_note": "every call on a Parakeet platform: MEETING_ASR_MODEL default 'parakeet-v3', "
                     "parakeet_default_supported() True",
    "keep_note": "keep_separate_meeting_speech(saved, new_install) -> {written, saved, after}",
}
