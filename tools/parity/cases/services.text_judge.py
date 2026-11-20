"""Curated inputs for services/text_judge.py (fv_llm llm/text_judge.h).

render records (shown questions, option labels, answer schema). parse renders the questions
first for their labels and records the answers, or for a TypeSafeError {"error": str(exc)}.
_fit_context runs over a custom endpoint whose saved model metadata pins the context window, and
records None or the ContextTooSmall's message, window and need. _failure_reason runs on an
exception of the named class carrying status_code when a status is given, over a text-model route
to a local server ("My server", judge-1 at 127.0.0.1:9000). The C++ replay is
tests/llm/test_text_judge.cpp.
"""

NOUL = {"q": {"type": "noul", "instructions": "Is it?"}}
NOUL_CRITERIA = {"q": {"type": "noul", "instructions": "Is it?", "criteria": {"yes": "Y"}}}
NOUL_EMPTY_CRITERIA = {"q": {"type": "noul", "instructions": "Is it?", "criteria": {}}}
NOUL_BARE = {"q": {"type": "noul"}}
CHOICE = {"q": {"type": "choice", "instructions": "Which?",
                "criteria": {"a": "A", "b": None, "c": "C"}}}
CHOICE_ONE = {"q": {"type": "choice", "instructions": "Which?", "criteria": {"only": "O"}}}
CHOICE_NONE = {"q": {"type": "choice", "instructions": "Which?"}}
CHOICE_UNICODE = {"q": {"type": "choice", "instructions": "Qué?",
                        "criteria": {"café": "à côté", "日本": None}}}
MIXED = {"route": {"type": "choice", "instructions": "Which action?",
                   "criteria": {"a_todo": "Todo list", "none": "No action"}},
         "payload": {"type": "noul", "instructions": "Is there a payload?"},
         "span": {"type": "choice", "instructions": "Which span?",
                  "criteria": {"buy milk": None, "Todo, buy milk": None}}}
SCORE = {"q": {"type": "score", "instructions": "How?", "criteria": ["a", "b"]}}
MIXED_SCORE = {"a": {"type": "noul", "instructions": "A?"},
               "b": {"type": "score", "instructions": "B?", "criteria": ["x", "y"]}}

QUESTIONS = [NOUL, NOUL_CRITERIA, NOUL_EMPTY_CRITERIA, NOUL_BARE, CHOICE, CHOICE_ONE, CHOICE_NONE,
             CHOICE_UNICODE, MIXED, SCORE, MIXED_SCORE, {}]

PARSE = [
    ['{"q": 0.83}', NOUL], ['{"q": 0}', NOUL], ['{"q": 1}', NOUL], ["not json at all", NOUL],
    ["[0.5]", NOUL], ['{"other": 0.5}', NOUL], ['{"q": 1.5}', NOUL], ['{"q": true}', NOUL],
    ['{"q": "0.5"}', NOUL], ['{"q": null}', NOUL], ["", NOUL], ["{", NOUL], ["}{", NOUL],
    ['{"q": 0.2} trailing {"q": 0.9}', NOUL],
    ['<think>{"q": 0.1} maybe</think>\n```json\n{"q": 0.75}\n```', NOUL],
    ['<THINK>hmm</Think>{"q": 0.4}', NOUL],
    ['<think>one</think><think>{"q": 0.1}</think>{"q": 0.6}', NOUL],
    ['<think>never closed {"q": 0.3}', NOUL],
    ['Sure! {"q": 0.66} Hope that helps.', NOUL],
    ['{"q": {"option": "2", "probability": 0.9}}', CHOICE],
    ['{"q": {"option": 2, "probability": 0.9}}', CHOICE],
    ['{"q": {"option": "1", "probability": 1}}', CHOICE],
    ['{"q": {"option": "3", "probability": 0.2}}', CHOICE],
    ['{"q": {"option": "9", "probability": 0.9}}', CHOICE],
    ['{"q": {"option": "1", "probability": 2}}', CHOICE],
    ['{"q": {"option": "1", "probability": true}}', CHOICE],
    ['{"q": {"option": "1", "probability": "0.5"}}', CHOICE],
    ['{"q": {"option": "1"}}', CHOICE],
    ['{"q": "1"}', CHOICE],
    ['{"q": {"option": "1.0", "probability": 0.9}}', CHOICE],
    ['{"q": {"option": "1", "probability": 0.7}}', CHOICE_ONE],
    ['{"q": {"option": "2", "probability": 0.7}}', CHOICE_UNICODE],
    ['{"route": {"option": "1", "probability": 0.95}, "payload": 0.8, '
     '"span": {"option": "1", "probability": 0.6}}', MIXED],
    ['{"payload": 0.8, "route": {"option": "2", "probability": 0.5}}', MIXED],
]


def _parse(module, text, questions):
    labels = module.render(questions)[1]
    try:
        return {"ok": module.parse(text, questions, labels)}
    except module.TypeSafeError as exc:
        return {"error": str(exc)}


def _fit_context(module, window, content_chars, max_tokens):
    from services.text_llm import TextLLMProfile

    profile = TextLLMProfile("custom_box", "Box", "custom", "http://127.0.0.1:9000/v1", "",
                             model_metadata={"model": "m", "context_window": window})
    try:
        module._fit_context(profile, "m", content_chars, max_tokens)
        return None
    except module.ContextTooSmall as exc:
        return {"error": str(exc), "window": exc.window, "needed": exc.needed}


def _failure_reason(module, name, status, timeout_s):
    from services import typesafe
    from services.text_llm import TextLLMProfile

    profile = TextLLMProfile("custom_box", "My server", "custom", "http://127.0.0.1:9000/v1", "")
    route = typesafe.JudgeRoute(id="text_model", name="My server", endpoint=profile.base_url,
                                model="judge-1", credential_env="",
                                timeout_s=typesafe.TEXT_MODEL_TIMEOUT_S, profile=profile)
    error = type(name, (Exception,), {})("secret body")
    if status is not None:
        error.status_code = status
    return module._failure_reason(error, route, "My server · judge-1", timeout_s)


CASES = {
    "render": [[questions] for questions in QUESTIONS],
    "parse": PARSE,
    "confidence": [[1.0, 4], [0.25, 4], [0.1, 4], [0.7, 1], [0.7, 0], [0.9, 3], [0.5, 2],
                   [0.0, 2], [1.0, 2], [0.95, 3], [0.6, 10]],
    "_fit_context": [[32768, 100, 104], [4096, 24000, 104], [4096, 11000, 104],
                     [4096, 11500, 104], [1024, 0, 64], [1024, 2400, 64], [8192, 23000, 1024],
                     [500, 0, 0], [100000, 24000, 2000000]],
    "_failure_reason": [[name, status, timeout_s]
                        for name, status in (("AuthenticationError", 401),
                                             ("PermissionDeniedError", 403),
                                             ("NotFoundError", 404), ("BadRequestError", 400),
                                             ("InternalServerError", 500),
                                             ("RateLimitError", 429),
                                             ("APITimeoutError", None), ("TimeoutError", None),
                                             ("APIConnectionError", None),
                                             ("RuntimeError", None), ("ValueError", None))
                        for timeout_s in (30.0, 5)] + [["APITimeoutError", None, 2.5],
                                                       ["APITimeoutError", None, 3.5]],
}

WRAPPERS = {
    "parse": _parse,
    "_fit_context": _fit_context,
    "_failure_reason": _failure_reason,
}
