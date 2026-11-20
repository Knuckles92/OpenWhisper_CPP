"""Curated inputs for the voice summaries of ui_qt/dialogs/settings_models.py (M2.20,
ui_downloads/model_assignments.h).

ModelAssignments' methods run on a stub self: a plain object holding what the method reads (the
settings snapshot, the page's built set, its engine combo and labels), so no Settings window is
built and no cache, runtime or credential is read. Each wrapper says what it returns. Settings
are the raw saved mappings; the engines are the three FioriVoce has (api, parakeet, remote).

  voice_summary(settings)          the rail's text
  voice_detail(settings)           where dictation runs, for Home
  voice_is_cloud(settings)         audio goes to an API server off this computer
  voice_is_remote(settings)        audio leaves this computer
  engine_caption(backend)          the caption under the engine combo
  engine_filter(backend)           the Downloads filter of "Get models and runtimes"
  text_summary(provider, model, settings)
                                   the Cleanup model in use as messages name it (M4.12,
                                   CleanupModelSection::textSummary): the endpoint's display name
                                   and the model, or "default model"
  engine_inventory(settings, cached, installed)
                                   every text the inventory label shows, in order: the count with
                                   a "." tail, then (for a local engine) the runtime's tail.
                                   cached: model keys on this computer; installed: runtime ids.

The runtime a device resolves to is pinned: the GPU build is the CUDA runtime, Auto means the CPU
(Python's answer depends on whether its environment has a GPU library, and the C++ replay has no
GPU), and nothing is an Apple Silicon Mac.
"""

from types import SimpleNamespace
from unittest.mock import patch

PAIRING = {"host": "192.168.1.20", "port": 47821, "fingerprint": "ab" * 16,
           "host_name": "Studio PC"}
LOCAL_SERVER = {"custom": {"base_url": "http://127.0.0.1:8000/v1", "model": "tiny"}}
HOSTED_SERVER = {"custom": {"base_url": "https://speech.example.com/v1", "model": "big"}}

SETTINGS = [
    [{}],
    [{"selected_model": "parakeet"}],
    [{"selected_model": "parakeet",
      "local_asr_models": {"parakeet": "orukeet-v0.1"}, "local_asr_devices": {"parakeet": "cpu"}}],
    [{"selected_model": "parakeet",
      "local_asr_models": {"parakeet": "no-such-model"}, "local_asr_devices": {"parakeet": "cuda"}}],
    [{"selected_model": "parakeet", "local_asr_devices": {"parakeet": "tpu"}}],
    [{"selected_model": "parakeet", "local_asr_models": {"moonshine": "orukeet-v0.1"}}],
    [{"selected_model": "not-an-engine"}],
    [{"selected_model": "api"}],
    [{"selected_model": "api", "api_transcription_model": "gpt-4o-transcribe"}],
    [{"selected_model": "api_gpt4o"}],
    [{"selected_model": "api", "api_transcription_endpoint": "groq"}],
    [{"selected_model": "api", "api_transcription_endpoint": "speaches"}],
    [{"selected_model": "api", "api_transcription_endpoint": "whisper_cpp"}],
    [{"selected_model": "api", "api_transcription_endpoint": "custom"}],
    [{"selected_model": "api", "api_transcription_endpoint": "custom",
      "api_transcription_servers": LOCAL_SERVER}],
    [{"selected_model": "api", "api_transcription_endpoint": "custom",
      "api_transcription_servers": HOSTED_SERVER}],
    [{"selected_model": "api", "api_transcription_endpoint": "no-such-server"}],
    [{"selected_model": "remote"}],
    [{"selected_model": "remote", "remote_engine_client": PAIRING}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "host_name": ""}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "host_name": "  "}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "fingerprint": ""}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "host": ""}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "port": 0}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "port": 70000}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "port": "8000"}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "port": "http"}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "port": 47821.9}}],
    [{"selected_model": "remote", "remote_engine_client": {**PAIRING, "port": None}}],
    [{"selected_model": "remote", "remote_engine_client": "paired"}],
    [{"selected_model": "remote", "remote_engine_client": {
        "host": "h", "fingerprint": "f"}}],
]

INVENTORIES = [
    [{"selected_model": "parakeet"}, [], []],
    [{"selected_model": "parakeet"}, ["parakeet-v3"], []],
    [{"selected_model": "parakeet", "local_asr_devices": {"parakeet": "cpu"}},
     ["parakeet-v3"], ["asr-nvidia-cpu"]],
    [{"selected_model": "parakeet", "local_asr_devices": {"parakeet": "cpu"}},
     ["parakeet-v3", "orukeet-v0.1"], ["asr-nvidia-cpu", "asr-nvidia-cuda"]],
    [{"selected_model": "parakeet", "local_asr_devices": {"parakeet": "cuda"}},
     ["orukeet-v0.1"], ["asr-nvidia-cpu"]],
    [{"selected_model": "parakeet", "local_asr_devices": {"parakeet": "cuda"}},
     ["parakeet-v3"], ["asr-nvidia-cuda"]],
    [{"selected_model": "parakeet", "local_asr_devices": {"parakeet": "cpu"}},
     ["not-a-model"], []],
    [{"selected_model": "api"}, ["parakeet-v3"], ["asr-nvidia-cpu"]],
    [{"selected_model": "remote"}, [], []],
]

# M4.12: the AI cleanup assignment as "Text model set to ..." names it.
_CUSTOM = {"text_llm_profiles": [
    {"id": "custom_abcd1234", "name": "LM Studio", "base_url": "http://127.0.0.1:1234/v1",
     "api_key_env": ""}]}
TEXT_SUMMARIES = [
    ["openai", "gpt-6-luna", {}],
    ["openrouter", "openrouter/free", {}],
    ["openrouter", "", {}],
    ["ollama", "llama3.2", {}],
    ["lm_studio", "", {}],
    ["groq", "llama-3.3-70b-versatile", {}],
    ["opencode_go", "glm-5.2", {}],
    ["custom_abcd1234", "local-qwen", _CUSTOM],
    ["custom_abcd1234", "", _CUSTOM],
    ["custom_gone", "x", {}],
    ["claude_code", "sonnet", {}],
    ["", "", {}],
]

CASES = {
    "text_summary": TEXT_SUMMARIES,
    "voice_summary": SETTINGS,
    "voice_detail": SETTINGS,
    "voice_is_cloud": SETTINGS,
    "voice_is_remote": SETTINGS,
    "engine_caption": [["parakeet"], ["api"], ["remote"], ["not-an-engine"]],
    "engine_filter": [["parakeet"], ["api"], ["remote"]],
    "engine_inventory": INVENTORIES,
}


class _Label:
    """What the page's labels record: every text set, in order."""

    def __init__(self):
        self.texts = []

    def setText(self, text):
        self.texts.append(text)


def _stub(module, settings):
    cls = module.ModelAssignments
    from services.remote_asr.settings import load_client_pairing

    stub = SimpleNamespace(_built=set(), _settings_snapshot=lambda: dict(settings),
                           _remote_pairing=lambda: load_client_pairing(dict(settings)))
    stub._engine_value = lambda: cls._engine_value(stub)
    stub.voice_is_cloud = lambda: cls.voice_is_cloud(stub)
    return stub


def _voice_summary(module, settings):
    return module.ModelAssignments.voice_summary(_stub(module, settings))


def _voice_detail(module, settings):
    return module.ModelAssignments.voice_detail(_stub(module, settings))


def _voice_is_cloud(module, settings):
    return module.ModelAssignments.voice_is_cloud(_stub(module, settings))


def _voice_is_remote(module, settings):
    return module.ModelAssignments.voice_is_remote(_stub(module, settings))


def _engine_caption(module, backend):
    label = _Label()
    stub = SimpleNamespace(engine_combo=SimpleNamespace(currentData=lambda: backend),
                           engine_caption=label)
    module.ModelAssignments._update_engine_caption(stub)
    return label.texts[-1]


def _engine_filter(module, backend):
    stub = SimpleNamespace(engine_combo=SimpleNamespace(currentData=lambda: backend))
    return module.ModelAssignments._engine_filter(stub)


def _engine_inventory(module, settings, cached, installed):
    from services.settings import SettingsKey
    from ui_qt.dialogs.settings_destinations import VOICE_MODEL

    cls = module.ModelAssignments
    backend = SettingsKey.SELECTED_MODEL.read(settings)
    label = _Label()
    stub = SimpleNamespace(
        _built={VOICE_MODEL}, engine_combo=SimpleNamespace(currentData=lambda: backend),
        engine_inventory_label=label, _settings_snapshot=lambda: dict(settings),
        _engine_runtime_pending={}, _engine_runtime_labels={},
        _engine_inventory_label_key=None, _engine_inventory_prefix="",
        _background_cache_scan=False)

    def checked(key, text, token):
        cls._on_engine_runtime_checked(stub, key, text, token)

    relay = module._worker_relay()
    relay.engine_runtime_checked.connect(checked)
    try:
        with patch("services.local_asr.cache.is_cached", side_effect=lambda key: key in cached), \
                patch("services.components.is_installed",
                      side_effect=lambda component: component in installed), \
                patch("services.local_asr.catalog.nvidia_gpu_runtime",
                      return_value="asr-nvidia-cuda"), \
                patch("services.local_asr.catalog._auto_device", return_value="cpu"), \
                patch("services.local_asr.catalog.apple_silicon", return_value=False):
            cls._refresh_engine_inventory(stub)
    finally:
        relay.engine_runtime_checked.disconnect(checked)
    return label.texts


def _text_summary(module, provider, model, settings):
    stub = SimpleNamespace(_active_text_provider=provider, _active_text_model=model,
                           _settings_snapshot=lambda: dict(settings))
    return module.ModelAssignments.text_summary(stub)


WRAPPERS = {
    "text_summary": _text_summary,
    "voice_summary": _voice_summary,
    "voice_detail": _voice_detail,
    "voice_is_cloud": _voice_is_cloud,
    "voice_is_remote": _voice_is_remote,
    "engine_caption": _engine_caption,
    "engine_filter": _engine_filter,
    "engine_inventory": _engine_inventory,
}

PINS = {"note": "ModelAssignments methods on a stub self; runtimes resolved with Auto on the CPU, "
                "the GPU build pinned to the CUDA runtime and a computer that is not an Apple "
                "Silicon Mac."}
