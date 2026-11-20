"""Curated inputs for ui_qt/dialogs/hf_consent_dialog.py (M2.5, models/consent_text.h).

_title_text(model) and _body_text(model, policy, env_blocked, runtime, gpu_runtime) run on a stub
dialog. The body reads missing_runtime and gpu_runtime_offer of services.local_asr.catalog,
patched here to answer `runtime` and `gpu_runtime` (the latter is asked only when a runtime is
missing). FioriVoce's models are Parakeet's; "unknown-model" stands for a name outside the
catalog that is not a custom source."""

from types import SimpleNamespace


def _title(module, model):
    return module.HuggingFaceConsentDialog._title_text(SimpleNamespace(model_name=model))


def _body(module, model, policy, env_blocked, runtime, gpu_runtime):
    from services.local_asr import catalog
    saved = (catalog.missing_runtime, catalog.gpu_runtime_offer)
    catalog.missing_runtime = lambda name, settings: runtime
    catalog.gpu_runtime_offer = lambda name, settings: gpu_runtime
    try:
        stub = SimpleNamespace(model_name=model, policy=policy, env_blocked=env_blocked)
        return module.HuggingFaceConsentDialog._body_text(stub)
    finally:
        catalog.missing_runtime, catalog.gpu_runtime_offer = saved


WRAPPERS = {"_title_text": _title, "_body_text": _body}

_MODELS = ["parakeet-v3", "orukeet-v0.1", "unknown-model"]
_RUNTIMES = [
    [None, None],
    ["asr-nvidia-cpu", None],
    ["asr-nvidia-cpu", "asr-nvidia-cuda"],
    ["asr-nvidia-cuda", None],
    ["asr-nvidia-cpu", "asr-nvidia-vulkan"],
]

CASES = {
    "_title_text": [[model] for model in _MODELS],
    "_body_text": [
        [model, policy, env_blocked, runtime, gpu_runtime]
        for model in _MODELS
        for policy in ("ask", "always", "never")
        for env_blocked in (False, True)
        for runtime, gpu_runtime in _RUNTIMES
    ],
}
