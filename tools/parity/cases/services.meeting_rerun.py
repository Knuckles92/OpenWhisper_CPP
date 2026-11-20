"""Curated inputs for services/meeting_rerun.py (M7.9 meeting-core).

    rerun_options(meeting, settings) -> the fields OpenWhisper_CPP's RerunOptions carries:
        {timeout_s, asr_model_name, language, speaker_id_backend, speaker_audio_consent,
         redecode_coverage_guard}
    resolve_speaker_pass(settings) -> {ok, reason, offered, has_key}

Pins, so that OpenWhisper answers as OpenWhisper_CPP's build does:
- every call is on a Parakeet platform (meeting model default "parakeet-v3",
  parakeet_default_supported() True), as in services.settings_meeting's cases;
- the date is 2027-03-01, after OpenAI's shutdown, when OpenWhisper reads a stored "openai"
  speaker backend as "local", which OpenWhisper_CPP does on every date; so no key is ever looked up;
- a remote route never belongs to this computer (meeting/asr/remote.py _is_this_computer is
  False): the replay injects a reader with the same validation;
- the agent route (provider, model, endpoint, agent core) is left out: it joins with the meeting
  intelligence.
"""

import contextlib
import importlib
from datetime import date


@contextlib.contextmanager
def _pinned():
    import config
    from meeting.asr import remote
    from services import openai_retirement

    settings = importlib.import_module("services.settings")
    key = settings.SettingsKey.MEETING_ASR_MODEL
    setting = settings.SETTINGS_BY_KEY[key]
    saved = (setting._default, settings.SETTING_DEFAULTS, config.parakeet_default_supported,
             openai_retirement._today, remote._is_this_computer)
    setting._default = "parakeet-v3"
    settings.SETTING_DEFAULTS = {**saved[1], key: "parakeet-v3"}
    config.parakeet_default_supported = lambda *args, **kwargs: True
    openai_retirement._today = lambda: date(2027, 3, 1)
    remote._is_this_computer = lambda fingerprint: False
    try:
        yield
    finally:
        setting._default = saved[0]
        settings.SETTING_DEFAULTS = saved[1]
        config.parakeet_default_supported = saved[2]
        openai_retirement._today = saved[3]
        remote._is_this_computer = saved[4]


_FIELDS = ("timeout_s", "asr_model_name", "language", "speaker_id_backend",
           "speaker_audio_consent", "redecode_coverage_guard")


def _rerun_options(module, meeting, settings):
    with _pinned():
        options = module.rerun_options(meeting, settings)
    return {name: options[name] for name in _FIELDS}


def _resolve_speaker_pass(module, settings):
    with _pinned():
        gate = module.resolve_speaker_pass(settings)
    return {"ok": gate.ok, "reason": gate.reason, "offered": gate.offered,
            "has_key": bool(gate.api_key)}


_ROUTE = '{"fingerprint": "AB:CD", "family": "parakeet", "model": "parakeet-v3"}'
_ROUTE_FR = ('{"fingerprint": "AB:CD", "family": "parakeet", "model": "parakeet-v3", '
             '"language": "fr"}')

_SETTINGS = [
    {},
    {"meeting_language": "de", "meeting_redecode_coverage_guard": True,
     "meeting_audio_upload_consent_given": True, "meeting_speaker_id_backend": "off"},
    {"meeting_language": " FR ", "meeting_speaker_id_backend": "openai",
     "meeting_audio_upload_consent_given": True},
    {"meeting_language": "xx", "meeting_speaker_id_backend": "cloud"},
    {"selected_model": "parakeet", "local_asr_models": {"parakeet": "orukeet-v0.1"}},
    {"meeting_speech_separate": True, "meeting_asr_model": "orukeet-v0.1"},
]

_MEETINGS = [
    {},
    None,
    {"asr_model": "orukeet-v0.1"},
    {"asr_model": ""},
    {"asr_model": None},
    {"asr_model": "parakeet-v3", "asr_remote_json": None},
    {"asr_remote_json": _ROUTE},
    {"asr_remote_json": _ROUTE_FR, "asr_model": "parakeet-v3"},
    {"asr_remote_json": "{}"},
    {"asr_remote_json": "not json"},
]

CASES = {
    "rerun_options": [[meeting, settings] for meeting in _MEETINGS for settings in _SETTINGS[:2]]
    + [[{"asr_model": "parakeet-v3"}, settings] for settings in _SETTINGS[2:]]
    + [[{}, settings] for settings in _SETTINGS[2:]],
    "resolve_speaker_pass": [[settings] for settings in _SETTINGS],
}

WRAPPERS = {
    "rerun_options": _rerun_options,
    "resolve_speaker_pass": _resolve_speaker_pass,
}

PINS = {
    "today": "2027-03-01 (openai_retirement._today)",
    "platform_note": "a Parakeet platform: MEETING_ASR_MODEL default 'parakeet-v3', "
                     "parakeet_default_supported() True",
    "remote_note": "meeting.asr.remote._is_this_computer is False",
}
