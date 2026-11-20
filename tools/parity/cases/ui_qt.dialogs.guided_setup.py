"""Curated inputs for ui_qt/dialogs/guided_setup.py (M2.26, ui_setup/guided_setup_dialog.h).

shortcut_help and _on_number_pad run as they are, over the trigger modes, the hands-free latch
and shortcuts on the number pad, on the mouse's side buttons and elsewhere. The side-button note
is the Windows hook's (USE_PYNPUT_BACKEND is false on Windows).

_refresh_speech is GuidedSetupDialog's real method run on a stand-in window: labels, bars and
buttons that record what they are told, a Settings window whose models answer the voice summary
and detail given, and the state fields the method reads (_download_progress, _speech_ready,
_download_error). Each case is [summary, detail, ready, progress, error], progress None or
[text, done, total]; the result is what the Record and Ready pages show
(speech_status in C++). model_download_progress runs on a stand-in whose download is the one
reported, and returns the text it stores.
"""

from types import SimpleNamespace

_TOGGLE = {"recording_trigger_mode": "toggle"}
_HOLD = {"recording_trigger_mode": "push_hold", "recording_hands_free_latch": True}
_HOLD_NO_LATCH = {"recording_trigger_mode": "push_hold", "recording_hands_free_latch": False}

_HOTKEYS = ["kp *", "ctrl+kp 1", "kp enter", "ctrl+alt+r", "ctrl+shift+space", "f9",
            "mouse4", "ctrl+mouse5", "", "skip+kp 2"]

CASES = {
    "shortcut_help": [[hotkey, settings]
                      for settings in ({}, _TOGGLE, _HOLD, _HOLD_NO_LATCH,
                                       {"recording_trigger_mode": "bogus"})
                      for hotkey in _HOTKEYS],
    "_on_number_pad": [[hotkey] for hotkey in _HOTKEYS + ["kpenter", "alt+kp", "KP 1"]],
    "_refresh_speech": [
        ["Parakeet TDT 0.6B v3", "On this computer · auto", True, None, ""],
        ["Parakeet TDT 0.6B v3", "On this computer · auto", False, None, ""],
        ["Parakeet TDT 0.6B v3", "On this computer · auto", False, None,
         "The download didn't finish. Check your connection and try again."],
        ["Parakeet TDT 0.6B v3", "On this computer · auto", False,
         ["Downloading…", 0, 0], ""],
        ["Parakeet TDT 0.6B v3", "On this computer · auto", False,
         ["Downloading… 300 MB of 1.20 GB", 300000000, 1200000000], ""],
        ["Parakeet TDT 0.6B v3", "On this computer · auto", False,
         ["Downloading… 1 KB of 3 KB", 1000, 3000], ""],
        ["Parakeet TDT 0.6B v3", "On this computer · cuda", True,
         ["Installing what the model runs on…", 5, 10], ""],
        ["Parakeet TDT 0.6B v3", "On this computer · auto", None,
         ["Installing what the model runs on…", 0, 0], "ignored"],
        ["Groq · whisper-large-v3-turbo", "Sent to api.groq.com for transcription", None, None,
         ""],
        ["Remote · not paired", "Pair with a host in Remote engine", None, None, ""],
        ["Parakeet TDT 0.6B v3", "On this computer · auto", True, None,
         "An old error a ready model hides."],
    ],
    "model_download_progress": [
        [0, 0], [300000000, 1200000000], [1000, 3000], [1, 999], [999, -1],
        [5000000000, 12000000000],
    ],
}


class _Label:
    def __init__(self):
        self.text, self.visible, self.properties = None, None, {}

    def setText(self, text):
        self.text = text

    def setVisible(self, visible):
        self.visible = bool(visible)

    def property(self, name):
        return self.properties.get(name)

    def setProperty(self, name, value):
        self.properties[name] = value

    def style(self):
        return None

    def update(self):
        pass


class _Bar(_Label):
    def __init__(self):
        super().__init__()
        self.maximum, self.value = None, None

    def setRange(self, minimum, maximum):
        self.maximum = maximum

    def setValue(self, value):
        self.value = value


def _refresh_speech(module, summary, detail, ready, progress, error):
    models = SimpleNamespace(voice_summary=lambda: summary, voice_detail=lambda: detail)
    window = SimpleNamespace(
        _settings=SimpleNamespace(models=models),
        _download_progress=tuple(progress) if progress else None,
        _speech_ready=ready,
        _download_error=error,
        speech_progress=_Bar(), ready_progress=_Bar(),
        speech_summary=_Label(), ready_speech_note=_Label(),
        speech_download_button=_Label(), ready_download_button=_Label(),
        ready_speech=_Label(), ready_title=_Label(), ready_lede=_Label(),
    )
    module.GuidedSetupDialog._refresh_speech(window)
    return {
        "summary_line": window.speech_summary.text,
        "ready_note": window.ready_speech_note.text,
        "tone": window.speech_summary.properties.get("tone"),
        "ready_note_tone": window.ready_speech_note.properties.get("tone"),
        "progress_visible": window.speech_progress.visible,
        "ready_progress_visible": window.ready_progress.visible,
        "bar_maximum": window.speech_progress.maximum,
        "bar_value": window.speech_progress.value,
        "record_download_visible": window.speech_download_button.visible,
        "ready_download_visible": window.ready_download_button.visible,
        "ready_row_visible": window.ready_speech.visible,
        "ready_title": window.ready_title.text,
        "ready_lede": window.ready_lede.text,
    }


def _model_download_progress(module, done, total):
    window = SimpleNamespace(_model_busy="parakeet-v3", _download_progress=None,
                             _refresh_speech=lambda: None)
    module.GuidedSetupDialog.model_download_progress(window, "parakeet-v3", done, total)
    return list(window._download_progress)


# provider_summary (M4.14, ui_setup/cleanup_provider_row.h): each case is [settings, key_saved];
# resolve_credential is replaced by an answer that holds a key for every variable (key_saved true)
# or for none, so no key store, environment or .env is read. The result is the 5-tuple as a list.
_LOCAL_OLLAMA = {"ollama_base_url": "http://localhost:11434/v1"}
_LAN_OLLAMA = {"ollama_base_url": "http://192.168.1.10:11434/v1"}


def _custom(url, env="COMPANY_KEY"):
    return {"transcript_cleanup_provider": "custom_company", "transcript_cleanup_model": "m",
            "text_llm_profiles": [{"id": "custom_company", "name": "Company endpoint",
                                   "kind": "custom", "base_url": url, "api_key_env": env}]}


_PROVIDER_SETTINGS = [
    {},
    {"transcript_cleanup_provider": "openai", "transcript_cleanup_model": "my-model"},
    {"transcript_cleanup_provider": "openrouter", "transcript_cleanup_model": "my-model"},
    {"transcript_cleanup_provider": "groq", "transcript_cleanup_model": "my-model"},
    {"transcript_cleanup_provider": "opencode_go", "transcript_cleanup_model": "my-model"},
    {"transcript_cleanup_provider": "ollama", "transcript_cleanup_model": "my-model",
     **_LOCAL_OLLAMA},
    {"transcript_cleanup_provider": "ollama", "transcript_cleanup_model": "my-model",
     **_LAN_OLLAMA},
    {"transcript_cleanup_provider": "lm_studio", "transcript_cleanup_model": "my-model"},
    _custom("https://example.com/v1"),
    _custom("http://127.0.0.1:8080/v1"),
    _custom("http://127.0.0.1:8080/v1", env=""),
    # A saved custom endpoint that is gone reads as OpenRouter (the resolver's fallback).
    {"transcript_cleanup_provider": "custom_gone", "transcript_cleanup_model": "m"},
    {"transcript_cleanup_provider": "bogus"},
]

CASES["provider_summary"] = [[settings, saved] for settings in _PROVIDER_SETTINGS
                             for saved in (True, False)]


def _provider_summary(module, settings, key_saved):
    from unittest.mock import patch

    with patch.object(module, "resolve_credential", return_value="key" if key_saved else ""):
        return list(module.provider_summary(settings))


WRAPPERS = {"_refresh_speech": _refresh_speech, "model_download_progress": _model_download_progress,
            "provider_summary": _provider_summary}
