"""Curated inputs for services/transcript_cleanup.py (fv_llm llm/transcript_cleanup.h).

_attempt_budget runs on a stand-in cleaner holding only the client timeout and a client whose
with_options records the retries it is given: the vector is the wall-clock limit and those
retries (None when the dictation client is used as it is). provider_env_key reads the settings
given (text_llm._load_settings is replaced). cleanup runs a real TranscriptCleanup whose client is
text_llm.create_openai_client's real OpenAI SDK client over an httpx MockTransport answering the
case's reply (no network, retry sleeps skipped), with the settings and keys given; the vector
records the returned text, last_error and every request as services.text_generation's vectors
do, with a uuid4 session header written as "<uuid4>". Cases always pass a system prompt: the
legacy default prompt is services.cleanup_prompts' data. The C++ replay is
tests/llm/test_transcript_cleanup.cpp.
"""
import json
import re

SECRET = "sk-test-SECRET-0123456789"
_SDK_HEADERS = {"authorization", "host", "content-length", "connection", "accept-encoding"}
_UUID4 = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")

# Every case carries its provider's answer: {"status", "json"}.
REPLIES = {
    "chat": {"status": 200, "json": {"choices": [{"message": {
        "role": "assistant", "content": "  Clean text.  "}, "finish_reason": "stop"}]}},
    "chat_blank": {"status": 200, "json": {"choices": [{"message": {
        "role": "assistant", "content": "   "}, "finish_reason": "stop"}]}},
    "responses": {"status": 200, "json": {"id": "resp_1", "status": "completed", "output": [
        {"type": "message", "id": "msg_1", "role": "assistant", "status": "completed",
         "content": [{"type": "output_text", "text": "Clean text.", "annotations": []}]}]}},
    "401": {"status": 401, "json": {"error": {"message": "Incorrect API key"}}},
    "500": {"status": 500, "json": {"error": {"message": "synthetic"}}},
}


def _record(request):
    headers = {}
    for name, value in request.headers.items():
        name = name.lower()
        if name in _SDK_HEADERS or name.startswith("x-stainless-"):
            continue
        if name == "user-agent" and value.startswith("OpenAI/"):
            continue
        if name == "x-opencode-session" and _UUID4.match(value):
            value = "<uuid4>"
        headers[name] = value
    content = request.content
    return {
        "method": request.method,
        "path": request.url.raw_path.decode("ascii").split("?", 1)[0],
        "bearer": request.headers.get("authorization") == f"Bearer {SECRET}",
        "timeout": request.extensions.get("timeout", {}).get("read"),
        "headers": dict(sorted(headers.items())),
        "body": json.loads(content) if content else None,
    }


class _Client:
    def __init__(self):
        self.retries = None

    def with_options(self, max_retries):
        self.retries = max_retries
        return self


def _attempt_budget(module, client_timeout_s, timeout_s, deadline_s):
    cleaner = module.TranscriptCleanup.__new__(module.TranscriptCleanup)
    cleaner.client = _Client()
    cleaner._timeout_s = client_timeout_s
    _client, give_up_s = cleaner._attempt_budget(timeout_s, deadline_s)
    return {"give_up_s": give_up_s, "batch_retries": cleaner.client.retries}


def _with_settings(settings, keys, call):
    from services import text_llm

    original_load, original_resolve = text_llm._load_settings, text_llm.resolve_credential
    text_llm._load_settings = lambda given=None: given if given is not None else settings
    text_llm.resolve_credential = lambda name: keys.get(name)
    try:
        return call()
    finally:
        text_llm._load_settings, text_llm.resolve_credential = original_load, original_resolve


def _provider_env_key(module, provider, settings):
    return _with_settings(settings, {}, lambda: module.provider_env_key(provider))


def _cleanup(module, case):
    import time

    import httpx
    import openai

    from services import text_llm, text_model_catalog

    seen = []

    def handler(request):
        seen.append(_record(request))
        reply = case["reply"]
        return httpx.Response(reply["status"], json=reply["json"])

    def factory(**kwargs):
        return openai.OpenAI(**kwargs, http_client=httpx.Client(
            transport=httpx.MockTransport(handler)))

    def run():
        cleaner = module.TranscriptCleanup(provider=case.get("provider"), model=case.get("model"),
                                           api_key=SECRET, reasoning=case.get("reasoning"))
        text = cleaner.cleanup(case["text"], system_prompt=case["system_prompt"],
                               timeout_s=case.get("timeout_s"), deadline_s=case.get("deadline_s"))
        return {"text": text, "last_error": cleaner.last_error, "model": cleaner.model,
                "reasoning": cleaner.reasoning, "attempt_timeout_s": cleaner.attempt_timeout_s}

    original_class, original_sleep = text_llm._openai_class, time.sleep
    text_llm._openai_class = lambda: factory
    time.sleep = lambda _seconds: None
    text_model_catalog._server_specs.clear()
    try:
        result = _with_settings(case.get("settings", {}), case.get("keys", {}), run)
    finally:
        text_llm._openai_class, time.sleep = original_class, original_sleep
    result["requests"] = seen
    return result


WRAPPERS = {
    "_attempt_budget": _attempt_budget,
    "provider_env_key": _provider_env_key,
    "cleanup": _cleanup,
}

CUSTOM = {"id": "custom_abcd1234", "name": "Gateway", "base_url": "https://gateway.test/v1",
          "api_key_env": "GATEWAY_KEY"}
LOCAL = {"id": "custom_local01", "name": "Local", "base_url": "http://127.0.0.1:1234/v1",
         "api_key_env": ""}
PROMPT = "Rewrite as bullet points only."

CASES = {
    "_attempt_budget": [
        [8.0, None, None], [120.0, None, None], [60.0, None, None], [8.0, 120.0, None],
        [8.0, 30.0, 30.0], [120.0, 120.0, 120.0], [8.0, None, 5.0], [0.5, 0.5, 0.5],
        [8.0, 0.0, None], [2.5, None, None],
    ],
    "provider_env_key": [
        ["openai", {}], ["openrouter", {}], ["ollama", {}], ["groq", {}], ["opencode_go", {}],
        ["opencode_zen", {}], ["lm_studio", {"lm_studio_api_key_env": "LMSTUDIO_KEY"}],
        ["claude_code", {}], ["codex", {}], ["opencode_cli", {}], ["missing", {}], ["", {}],
        ["custom_abcd1234", {"text_llm_profiles": [CUSTOM]}],
        ["custom_local01", {"text_llm_profiles": [LOCAL]}],
        ["custom_abcd1234", {}],
    ],
    "cleanup": [[dict(case, reply=REPLIES[case["reply"]])] for case in [
        {"text": "um raw text", "system_prompt": PROMPT, "reply": "chat"},
        {"provider": "openrouter", "reasoning": "low", "text": "raw", "system_prompt": PROMPT,
         "reply": "chat"},
        {"provider": "openrouter", "model": "meta-llama/llama-3-8b", "reasoning": "extreme",
         "text": "raw", "system_prompt": "  Spaced prompt.  ", "reply": "chat"},
        {"provider": "openai", "text": "raw transcript", "system_prompt": PROMPT,
         "reasoning": "medium", "reply": "responses"},
        {"provider": "openai", "model": "o4-mini", "text": "raw", "system_prompt": PROMPT,
         "reasoning": "high", "timeout_s": 120.0, "reply": "responses"},
        {"provider": "openai", "model": "gpt-3.5-turbo", "text": "raw", "system_prompt": PROMPT,
         "reply": "chat"},
        {"provider": "groq", "model": "llama-3.3-70b-versatile", "text": "raw",
         "system_prompt": PROMPT, "reasoning": "high", "reply": "chat"},
        {"provider": "opencode_go", "model": "glm-5.2", "text": "raw", "system_prompt": PROMPT,
         "reply": "chat"},
        {"provider": "custom_abcd1234", "model": "gw-model", "text": "raw",
         "system_prompt": PROMPT, "settings": {"text_llm_profiles": [CUSTOM]},
         "keys": {"GATEWAY_KEY": "k"}, "reasoning": "high", "reply": "chat"},
        {"text": "keep me", "system_prompt": PROMPT, "reply": "chat_blank"},
        {"text": "raw text", "system_prompt": PROMPT, "reply": "401"},
        {"text": "raw text", "system_prompt": PROMPT, "reply": "500"},
        {"text": "raw text", "system_prompt": PROMPT, "timeout_s": 30.0, "deadline_s": 30.0,
         "reply": "500"},
        {"text": "   ", "system_prompt": PROMPT, "reply": "chat"},
        {"text": "", "system_prompt": PROMPT, "reply": "chat"},
        {"provider": "bogus", "model": "", "text": "raw", "system_prompt": PROMPT,
         "reply": "chat"},
    ]],
}

PINS = {
    "secret": "the key every request carries; vectors record only whether it was the one given",
    "session": "a uuid4 x-opencode-session header is recorded as <uuid4>",
}
