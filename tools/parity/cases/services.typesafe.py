"""Curated inputs for services/typesafe.py (fv_llm llm/judge.h, llm/typesafe.h).

validate_answers and _status_outcome record their result, or for a TypeSafeError
{"error": str(exc)} so its sentence is pinned too. route_from_settings and route_copy record a
route's fields and properties (the text-model profile as its id and is_local). verify_key runs
the real function over a transport that answers the case's status and body, or raises the named
exception; its measured latency is written as "<t>". The C++ replay is tests/llm/test_typesafe.cpp.
"""
import re

NOUL = {"q": {"type": "noul", "instructions": "Is it?"}}
CHOICE = {"q": {"type": "choice", "instructions": "Which?", "criteria": {"a": "A", "b": "B"}}}
CHOICE_LIST = {"q": {"type": "choice", "instructions": "Which?", "criteria": ["a", "b"]}}
SCORE = {"q": {"type": "score", "instructions": "How?", "criteria": ["low", "mid", "high"]}}
SCORE_DICT = {"q": {"type": "score", "criteria": {"low": "L", "high": "H"}}}
SCORE_NONE = {"q": {"type": "score"}}
UNKNOWN = {"q": {"type": "rank", "instructions": "?"}}
UNTYPED = {"q": {"instructions": "?"}}
TWO = {"a": {"type": "noul", "instructions": "A?"},
       "b": {"type": "choice", "instructions": "B?", "criteria": {"x": None, "y": "Y"}}}


def _noul(value):
    return {"answers": {"q": {"type": "noul", "noul": value}}}


def _choice(**answer):
    return {"answers": {"q": dict({"type": "choice"}, **answer)}}


def _score(**answer):
    return {"answers": {"q": dict({"type": "score"}, **answer)}}


VALIDATE = [
    [_noul(0.25), NOUL], [_noul(0), NOUL], [_noul(1), NOUL], [_noul(1.0), NOUL],
    [_noul(1.5), NOUL], [_noul(-0.01), NOUL], [_noul(True), NOUL], [_noul(False), NOUL],
    [_noul("0.5"), NOUL], [_noul(None), NOUL], [{"answers": {"q": {"type": "noul"}}}, NOUL],
    [{"answers": {"other": {"type": "noul", "noul": 0.5}}}, NOUL],
    [{"answers": {"q": {"type": "choice", "choice": "a"}}}, NOUL],
    [{"no_answers": True}, NOUL], [{"answers": []}, NOUL], [{"answers": None}, NOUL],
    [[], NOUL], ["text", NOUL], [None, NOUL], [{"answers": {"q": 0.5}}, NOUL],
    [{"answers": {"q": _noul(0.5)["answers"]["q"], "extra": _noul(0.5)["answers"]["q"]}}, NOUL],
    [{"answers": {}}, {}],
    [_choice(choice="b", confidence=0.7, probabilities={"a": 0.3, "b": 0.7}), CHOICE],
    [_choice(choice="a"), CHOICE],
    [_choice(choice="a", confidence=True), CHOICE],
    [_choice(choice="a", confidence=1), CHOICE],
    [_choice(choice="a", confidence=None), CHOICE],
    [_choice(choice="a", confidence=1.2), CHOICE],
    [_choice(choice="a", confidence="0.5"), CHOICE],
    [_choice(choice="zzz", confidence=0.9), CHOICE],
    [_choice(choice=None, confidence=0.9), CHOICE],
    [_choice(choice=1, confidence=0.9), CHOICE],
    [_choice(choice="a", confidence=0.5, probabilities=None), CHOICE],
    [_choice(choice="a", confidence=0.5, probabilities=[]), CHOICE],
    [_choice(choice="a", confidence=0.5, probabilities=[1]), CHOICE],
    [_choice(choice="a", confidence=0.5, probabilities="x"), CHOICE],
    [_choice(choice="a", confidence=0.5, probabilities=0), CHOICE],
    [_choice(choice="a", confidence=0.5, probabilities=5), CHOICE],
    [_choice(choice="a", confidence=0.5,
             probabilities={"a": 1, "b": True, "c": "x", "d": None, "e": 0.25}), CHOICE],
    [_choice(choice="b", confidence=0.5), CHOICE_LIST],
    [_choice(choice="c", confidence=0.5), CHOICE_LIST],
    [_score(score=1.4, confidence=0.5), SCORE], [_score(score=2), SCORE],
    [_score(score=0), SCORE], [_score(score=7), SCORE], [_score(score=-1), SCORE],
    [_score(score="1"), SCORE], [_score(score=True, confidence="high"), SCORE],
    [_score(score=1, confidence=None), SCORE], [_score(score=1), SCORE_DICT],
    [_score(score=1.5), SCORE_DICT], [_score(score=0), SCORE_NONE], [_score(score=0.5), SCORE_NONE],
    [{"answers": {"q": {"type": "rank"}}}, UNKNOWN], [{"answers": {"q": {}}}, UNTYPED],
    [{"answers": {"b": {"type": "choice", "choice": "y", "confidence": 0.4},
                  "a": {"type": "noul", "noul": 0.9}}}, TWO],
    [{"answers": {"a": {"type": "noul", "noul": 0.9},
                  "b": {"type": "choice", "choice": "x", "confidence": 0.4}}}, TWO],
]

TEXT_SETTINGS = {"typesafe_enabled": True, "typesafe_provider": "text_model",
                 "meeting_llm_provider": "ollama", "meeting_llm_model": "qwen3:4b",
                 "transcript_cleanup_provider": "openrouter",
                 "transcript_cleanup_model": "vendor/small-model"}

SETTINGS = [
    {}, {"typesafe_provider": "typesafe"}, {"typesafe_provider": "openrouter"},
    {"typesafe_provider": "bogus"}, {"typesafe_provider": 5}, {"typesafe_provider": None},
    {"typesafe_provider": "text_model"},
    TEXT_SETTINGS,
    dict(TEXT_SETTINGS, typesafe_text_model="cleanup"),
    dict(TEXT_SETTINGS, typesafe_text_model="bogus"),
    dict(TEXT_SETTINGS, meeting_llm_provider="lm_studio", meeting_llm_model="qwen/qwen3-4b",
         lm_studio_base_url="http://192.168.1.20:1234/v1"),
    dict(TEXT_SETTINGS, meeting_llm_provider="openai", meeting_llm_model="gpt-5-mini"),
    dict(TEXT_SETTINGS, meeting_llm_provider="gone", meeting_llm_model="x"),
    dict(TEXT_SETTINGS, typesafe_text_model="cleanup", transcript_cleanup_provider="claude_code",
         transcript_cleanup_model="sonnet"),
    dict(TEXT_SETTINGS, typesafe_text_model="cleanup", transcript_cleanup_provider="opencode_go",
         transcript_cleanup_model="glm-5.2"),
    dict(TEXT_SETTINGS, typesafe_text_model="cleanup", transcript_cleanup_provider="custom_abcd1234",
         transcript_cleanup_model="  local-model  ",
         text_llm_profiles=[{"id": "custom_abcd1234", "name": "Office box",
                             "base_url": "http://127.0.0.1:9000/v1",
                             "api_key_env": "OFFICE_KEY"}]),
    dict(TEXT_SETTINGS, meeting_llm_provider="ollama", meeting_llm_model="   "),
]

NAMES = ["TypeSafe", "OpenRouter", "Ollama", "LM Studio", "llama.cpp", "vLLM", "", "École",
         "eSpace", "Upstage", "İstanbul", "1Box", "agent"]

ENDPOINTS = ["https://api.typesafe.ai/v1/systemone", "http://127.0.0.1:9000/v1",
             "https://user:pw@Example.COM:8443/x", "not a url", "", "http://[::1]:11434/v1",
             "http://[::1:11434/v1"]


def _route_fields(route):
    return {
        "id": route.id, "name": route.name, "endpoint": route.endpoint, "model": route.model,
        "credential_env": route.credential_env, "headers": dict(route.headers),
        "timeout_s": route.timeout_s,
        "profile": None if route.profile is None else {"id": route.profile.id,
                                                       "is_local": route.profile.is_local},
        "is_local": route.is_local, "host": route.host, "key_label": route.key_label,
        "key_phrase": route.key_phrase,
    }


def _validate(module, body, questions):
    try:
        return {"ok": module.validate_answers(body, questions)}
    except module.TypeSafeError as exc:
        return {"error": str(exc)}


def _status_outcome(module, status, host, route_id):
    return module._status_outcome(status, host, module.ROUTES[route_id])


def _route_from_settings(module, settings):
    return _route_fields(module.route_from_settings(settings))


def _route_copy(module, name, endpoint):
    try:
        route = module.JudgeRoute(id="x", name=name, endpoint=endpoint, model="m",
                                  credential_env="")
        return {"host": route.host, "key_label": route.key_label, "key_phrase": route.key_phrase}
    except ValueError as exc:
        return {"error": type(exc).__name__}


def _verify(module, route_id, status, body, exc_name=None):
    def transport(payload, seconds):
        if exc_name is not None:
            raise {"TimeoutError": TimeoutError, "OSError": OSError,
                   "ConnectionRefusedError": ConnectionRefusedError}[exc_name]("x")
        return status, body

    ok, detail = module.verify_key("k", route=module.ROUTES[route_id], transport=transport)
    return [ok, re.sub(r"answered in \d+\.\d\d s\.", "answered in <t> s.", detail)]


GOOD = '{"answers": {"q": {"type": "noul", "noul": 1.0}}}'

CASES = {
    "validate_answers": VALIDATE,
    "_status_outcome": [[status, host, route_id]
                        for status in (200, 201, 204, 301, 400, 401, 402, 403, 404, 408, 422, 429,
                                       500, 502, 503)
                        for host, route_id in (("api.typesafe.ai", "typesafe"),
                                               ("openrouter.ai", "openrouter"))],
    "route_from_settings": [[settings] for settings in SETTINGS],
    "route_copy": [[name, "https://api.typesafe.ai/v1/systemone"] for name in NAMES]
                  + [["Box", endpoint] for endpoint in ENDPOINTS],
    "verify_key": [
        ["typesafe", 200, GOOD], ["openrouter", 200, GOOD],
        ["typesafe", 200, '{"answers": {"other": {"type": "noul", "noul": 1.0}}}'],
        ["typesafe", 200, "not json"], ["typesafe", 200, ""],
        ["typesafe", 200, '{"answers": {"q": {"type": "noul", "noul": 2}}}'],
        ["typesafe", 401, ""], ["openrouter", 402, ""], ["typesafe", 403, ""],
        ["typesafe", 404, ""], ["openrouter", 404, ""], ["typesafe", 429, ""],
        ["openrouter", 500, ""], ["typesafe", 201, GOOD],
        ["typesafe", 0, "", "TimeoutError"], ["openrouter", 0, "", "OSError"],
        ["typesafe", 0, "", "ConnectionRefusedError"],
    ],
}

WRAPPERS = {
    "validate_answers": _validate,
    "_status_outcome": _status_outcome,
    "route_from_settings": _route_from_settings,
    "route_copy": _route_copy,
    "verify_key": _verify,
}
