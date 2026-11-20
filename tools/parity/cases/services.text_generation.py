"""Curated inputs for services/text_generation.py (fv_llm llm/generation.h, llm/openai_client.h).

Each generate case names a scenario of tests/fixtures/llm/generation/scenarios.json. The client
is text_llm.create_openai_client's real OpenAI SDK client (the profile's URL and provider headers,
the scenario's max_retries, 0 unless given) over an httpx MockTransport that answers the
scenario's replies in order (replies.json), so the SDK's own request building, retries and
response parsing run with no network; its retry sleeps are skipped. The vector records each turn's
TextResult (usage as a dict) or {"raises": class, "message": str(exc)}, and every request: method,
raw path, query, whether the Bearer key was the one given, the read timeout, the headers that are
not the SDK's own (lower-case names; User-Agent only when it is not "OpenAI/..."), and the JSON
body. The C++ replay is tests/llm/test_generation.cpp.
"""
import json
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ow_isolation  # noqa: E402

FIXTURES = ow_isolation.REPO / "tests" / "fixtures" / "llm" / "generation"
SECRET = "sk-test-SECRET-0123456789"
_SDK_HEADERS = {"authorization", "host", "content-length", "connection", "accept-encoding"}


def _load(name):
    return json.loads((FIXTURES / f"{name}.json").read_text(encoding="utf-8"))


def _profile(spec, settings):
    from services.text_llm import TextLLMProfile, get_profile

    if isinstance(spec, str):
        return get_profile(spec, settings)
    return TextLLMProfile(**spec)


def _reply(entry, replies):
    import httpx

    if isinstance(entry, str):
        return 200, {}, replies[entry], None
    if "error" in entry:
        return None, None, None, entry["error"]
    body = entry["json"] if "json" in entry else entry.get("text", "")
    return entry["status"], entry.get("headers", {}), body, None


def _record(request):
    headers = {}
    for name, value in request.headers.items():
        name = name.lower()
        if name in _SDK_HEADERS or name.startswith("x-stainless-"):
            continue
        if name == "user-agent" and value.startswith("OpenAI/"):
            continue
        headers[name] = value
    content = request.content
    return {
        "method": request.method,
        "path": request.url.raw_path.decode("ascii").split("?", 1)[0],
        "query": request.url.query.decode("ascii"),
        "bearer": request.headers.get("authorization") == f"Bearer {SECRET}",
        "timeout": request.extensions.get("timeout", {}).get("read"),
        "headers": dict(sorted(headers.items())),
        "body": json.loads(content) if content else None,
    }


def _result(result):
    usage = result.usage
    if usage is not None and not isinstance(usage, dict):
        usage = (usage.model_dump(exclude_none=True) if hasattr(usage, "model_dump")
                 else dict(vars(usage)))
    return {
        "text": result.text,
        "tool_calls": [{"id": c.id, "name": c.function.name, "arguments": c.function.arguments}
                       for c in result.tool_calls],
        "assistant_message": result.assistant_message,
        "usage": usage,
        "finish_reason": result.finish_reason,
    }


def _generate(module, name):
    import time

    import httpx
    import openai

    from services import text_llm, text_model_catalog
    from services.text_model_catalog import TextModelSpec

    document = _load("scenarios")
    replies = _load("replies")
    scenario = document["scenarios"][name]
    profile = _profile(scenario["profile"], scenario.get("settings", {}))
    text_model_catalog._server_specs.clear()
    if "server_specs" in scenario:
        text_model_catalog.remember_server_specs(
            profile.base_url,
            {model: TextModelSpec(**fields) for model, fields in scenario["server_specs"].items()})

    queue = list(scenario["replies"])
    seen = []
    cancel = threading.Event()

    def handler(request):
        seen.append(_record(request))
        status, headers, body, error = _reply(queue.pop(0), replies)
        if scenario.get("cancel_on_reply"):
            cancel.set()
        if error == "timeout":
            raise httpx.ReadTimeout("timed out", request=request)
        if error == "connect":
            raise httpx.ConnectError("[WinError 10061] refused", request=request)
        if isinstance(body, str):
            return httpx.Response(status, headers=headers, text=body)
        return httpx.Response(status, headers=headers, json=body)

    def factory(**kwargs):
        return openai.OpenAI(**kwargs, http_client=httpx.Client(
            transport=httpx.MockTransport(handler)))

    tools = scenario.get("tools", False)
    if tools is True:
        tools = [document["tool"]]
    messages = json.loads(json.dumps(scenario.get("messages", document["default_messages"])))
    kwargs = dict(model=scenario["model"], tools=tools or None, cancel_event=cancel,
                  session_id=scenario.get("session_id", ""),
                  reasoning_level=scenario.get("reasoning_level", ""))

    original_class, original_sleep = text_llm._openai_class, time.sleep
    text_llm._openai_class = lambda: factory
    time.sleep = lambda _seconds: None
    turns = []
    try:
        client = text_llm.create_openai_client(
            profile, api_key=SECRET, max_retries=scenario.get("max_retries", 0))
        for turn in range(2 if "tool_result" in scenario else 1):
            try:
                result = module.generate(client, profile, messages=messages, **kwargs,
                                         **json.loads(json.dumps(scenario.get("options", {}))))
            except Exception as exc:  # noqa: BLE001 - the vector records what the caller sees
                turns.append({"raises": type(exc).__name__, "message": str(exc)})
                break
            turns.append(_result(result))
            if turn == 0 and "tool_result" in scenario:
                messages = messages + [result.assistant_message, {
                    "role": "tool", "tool_call_id": result.tool_calls[0].id,
                    "content": scenario["tool_result"]}]
    finally:
        text_llm._openai_class, time.sleep = original_class, original_sleep
        text_model_catalog._server_specs.clear()
    return {"turns": turns, "requests": seen}


WRAPPERS = {"generate": _generate}

CASES = {"generate": [[name] for name in _load("scenarios")["scenarios"]]}

PINS = {
    "secret": "the key every request carries; vectors record only whether it was the one given",
    "fixtures": "tests/fixtures/llm/generation/scenarios.json and replies.json",
}
