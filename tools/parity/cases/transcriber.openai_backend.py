"""Curated inputs for transcriber/openai_backend.py (fv_speech transcription_hints, response_format).

Both take a SpeechEndpoint; a case names it as (endpoint id, settings dict) and the wrapper
builds it with services.speech_endpoints.configured_endpoint. The recognition context is
(language, phrases), or null for no context. The C++ replay is
tests/speech/test_transcription_hints.cpp.
"""

CUSTOM = {"api_transcription_servers": {"custom": {"base_url": "http://127.0.0.1:9000/v1"}}}
REMOTE_SPEACHES = {"api_transcription_servers": {
    "speaches": {"base_url": "http://gpu-box:8000/v1", "model": "Systran/faster-whisper-large-v3"}}}

ENDPOINTS = [
    ["openai", {}], ["groq", {}], ["speaches", {}], ["speaches", REMOTE_SPEACHES],
    ["localai", {}], ["vllm", {}], ["whisper_cpp", {}], ["custom", CUSTOM],
]
MODELS = ["gpt-transcribe", "gpt-4o-transcribe", "gpt-4o-mini-transcribe", "whisper-1",
          "Systran/faster-whisper-small", "whisper-large-v3-turbo"]
CONTEXTS = [
    None,
    ["", []],
    ["auto", []],
    ["AUTO", ["Ksenia"]],
    ["de-DE", ["Ksenia", "Kubernetes"]],
    ["en_US", [f"w{i}" for i in range(80)] + ["x" * 70]],
    ["pt-BR", ["  New   York  ", "", "x" * 65, "ß" * 64, "日本語", "tab\there"]],
    ["FR", [f"phrase number {i:02d} with words" for i in range(30)]],
    ["zh", ["a" * 64] * 60],
    ["", ["Ksenia, Kubernetes"]],
]


def _endpoint(endpoint_id, settings):
    from services.speech_endpoints import configured_endpoint

    return configured_endpoint(endpoint_id, settings)


def _context(context):
    from services.recognition_context import RecognitionContext

    if context is None:
        return None
    language, phrases = context
    return RecognitionContext(language=language, phrases=tuple(phrases))


def _transcription_hints(module, endpoint_id, settings, model, context):
    return module.transcription_hints(_endpoint(endpoint_id, settings), model, _context(context))


def _response_format(module, endpoint_id, settings, model):
    return module.response_format(_endpoint(endpoint_id, settings), model)


WRAPPERS = {
    "transcription_hints": _transcription_hints,
    "response_format": _response_format,
}

CASES = {
    "transcription_hints": [
        [endpoint_id, settings, model, context]
        for endpoint_id, settings in ENDPOINTS
        for model in MODELS
        for context in CONTEXTS
    ],
    "response_format": [
        [endpoint_id, settings, model]
        for endpoint_id, settings in ENDPOINTS
        for model in MODELS
    ],
}

PINS = {
    "endpoint": "configured_endpoint(id, settings) from services.speech_endpoints",
    "context": "RecognitionContext(language, tuple(phrases)), or None for a null context",
}
