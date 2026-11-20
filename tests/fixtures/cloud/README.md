# Cloud speech replies

Response bodies the cloud speech tests (`tests/speech`) serve from a loopback server or a
`FakeHttpClient`. None was recorded from a live call: each is copied from OpenWhisper's own test
fixtures or written from the provider's documented shape.

| File | Source |
|---|---|
| `openai_gpt_transcribe.json` | `tests/test_openai_backend.py::test_sdk_sends_model_and_parses_response_without_network` |
| `openai_whisper_text.txt` | the same test, `response_format="text"` |
| `openai_model_not_found.json` | `tests/test_openai_retirement.py::test_gone_retiring_model_retries_with_gpt_transcribe` |
| `openai_invalid_value.json` | `tests/test_openai_retirement.py::test_other_errors_are_not_retried` |
| `openai_bad_keywords.json` | `tests/test_personalize_s4_openai.py::test_sdk_rejection_is_retried_without_hints` |
| `openai_invalid_api_key.json` | OpenAI's documented error object for a wrong key (401) |
| `openai_models.json` | OpenAI's documented `GET /v1/models` list shape |
| `groq_transcription.json` | Groq's documented `POST /openai/v1/audio/transcriptions` JSON reply |
| `speaches_models.json` | `tests/test_speech_endpoints.py::test_speaches_lists_only_its_speech_models` |
| `speaches_transcription.json` | `tests/test_speech_endpoints.py::test_isolated_upload_reaches_a_local_server` |
| `localai_capabilities.json` | `tests/test_speech_endpoints.py::test_localai_lists_models_that_transcribe` |
| `not_found.json` | the default route of `tests/test_speech_endpoints.py`'s `serve` helper |
