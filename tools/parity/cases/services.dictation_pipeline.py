"""Curated inputs for services/dictation_pipeline.py (M1.19 dictation-core): the caret joins for
pasting, the paste-target check, History's context columns, the Stats counts, the recorded
cleanup level and the focus-capture decision table.

A job travels as a JSON object: {"mode", "focus", "target", "recognition", "source", "route"},
where focus and target are FocusSnapshot objects ({"identity", "text"}, null for no capture)
whose futures are already resolved, and recognition is {"language", "phrases"} or null. The
wrappers build the dataclasses and, where the pipeline asks focus_context.get_service(), install
a stub service for the call and restore the previous one.

stats_counts and cleanup_level call services the C++ pipeline takes as parameters
(dictation_stats.count_words / words_edited, cleanup_prompts.custom_prompt); their answers for
the case's inputs are returned beside the result so the replay can inject them.

rich_paste runs the live-dictation path of tests/test_personalize_fix_f5_rich_join.py without
the runtime: prepare_text with the test's snippets, finish_text without cleanup, then the joins.
Snippet expansion is M4's, so the C++ replay starts from the finished text and HTML recorded
here and checks the joins.
"""

from concurrent.futures import Future

from services import focus_context
from services.dictation_pipeline import DictationJob, JobMode
from services.focus_context import AppIdentity, FocusSnapshot, TextContext
from services.recognition_context import RecognitionContext


def _future(value):
    future = Future()
    future.set_result(value)
    return future


def _identity(fields):
    return None if fields is None else AppIdentity(**fields)


def _context(fields):
    return None if fields is None else TextContext(**fields)


def _snapshot(fields):
    if fields is None:
        return None
    return FocusSnapshot(_identity(fields.get("identity")), _context(fields.get("text")))


def _job(module, spec):
    if spec is None:
        return None
    focus = spec.get("focus")
    target = spec.get("target")
    recognition = spec.get("recognition")
    return module.DictationJob(
        mode=spec.get("mode", JobMode.DICTATION),
        focus=None if focus is None else _future(_snapshot(focus)),
        target=None if target is None else _future(_snapshot(target)),
        recognition=None if recognition is None else RecognitionContext(
            language=recognition["language"], phrases=tuple(recognition["phrases"])),
        source=spec.get("source", ""),
        route=spec.get("route", ""),
    )


class _StubService:
    def __init__(self, current=None):
        self.current = current
        self.requests = []

    def request(self, *, include_text, include_selection=False):
        self.requests.append((include_text, include_selection))
        return _future(FocusSnapshot())

    def identity_now(self, timeout_s):
        return self.current

    def current_identity(self):
        return self.current

    def shutdown(self):
        pass


def _with_service(service, fn):
    previous = focus_context.get_service()
    focus_context.set_service(service)
    try:
        return fn()
    finally:
        focus_context.set_service(previous)


def _text_for_paste(module, text, job):
    return module.text_for_paste(text, _job(module, job))


def _html_for_paste(module, html, text, job):
    return module.html_for_paste(html, text, _job(module, job))


def _paste_target(module, job, current):
    service = _StubService(_identity(current))
    return _with_service(service, lambda: [
        module.paste_target(_job(module, job)), module.paste_target_ok(_job(module, job))])


def _history_fields(module, job, info_level, live, settings):
    info = None if info_level is None else type("Info", (), {"level": info_level})()
    return module.history_fields(_job(module, job), info, live=live, settings=settings)


def _stats_counts(module, original, text):
    from services.dictation_stats import count_words, words_edited
    from services.snippets import SnippetPlan

    prepared = module.PreparedText(original=original, text=text, cleanup_input=text,
                                   plan=SnippetPlan(text), skip_cleanup=False)
    return {"counts": module.stats_counts(prepared), "corrections": words_edited(original, text),
            "words": {original: count_words(original), text: count_words(text)}}


def _cleanup_level(module, settings, has_profile):
    from services import cleanup_prompts

    profile = object() if has_profile else None
    return {"level": module.cleanup_level(settings, profile),
            "custom": cleanup_prompts.custom_prompt(settings)}


def _focus_requests(module, mode, settings, source, route, include_selection):
    service = _StubService()

    def run():
        focus, target = module._focus_requests(mode, settings, source=source, route=route,
                                               include_selection=include_selection)
        requests = list(service.requests)
        return {
            "focus": None if focus is None else list(requests.pop(0)),
            "target": None if target is None else list(requests.pop(0)),
        }

    return _with_service(service, run)


# tests/test_personalize_fix_f5_rich_join.py
_CALENDAR = {"id": "cal", "trigger": "my calendar link",
             "text": "**[my calendar](https://cal.example/sam)**", "formatted": True}
_SIGNATURE = {"id": "sig", "trigger": "my sign off", "text": "**Dana Lee**", "formatted": True}


def _rich_paste(module, raw, before, after, caret_known):
    settings = {"dictation_snippets": [_CALENDAR, _SIGNATURE]}
    context = TextContext(before=before, after=after, caret_known=caret_known,
                          selection_known=True)
    job = DictationJob(mode=JobMode.DICTATION, focus=_future(FocusSnapshot(
        AppIdentity("OUTLOOK.EXE", "Outlook", platform="windows"), context)))
    prepared = module.prepare_text(raw, job, settings)
    finished = module.finish_text(prepared.cleanup_input, prepared)
    return {
        "finished_text": finished.text,
        "finished_html": finished.html,
        "plain": module.text_for_paste(finished.text, job),
        "html": module.html_for_paste(finished.html, finished.text, job),
    }


WRAPPERS = {
    "text_for_paste": _text_for_paste,
    "html_for_paste": _html_for_paste,
    "paste_target": _paste_target,
    "history_fields": _history_fields,
    "stats_counts": _stats_counts,
    "cleanup_level": _cleanup_level,
    "_focus_requests": _focus_requests,
    "rich_paste": _rich_paste,
}


def _app(app_id, name="", platform="windows", **extra):
    return {"app_id": app_id, "name": name, "platform": platform, **extra}


def _caret(before, after="", known=True):
    return {"before": before, "after": after, "caret_known": known, "selection_known": True,
            "source": "uia"}


def _chrome(hint):
    return _app("chrome.exe", "Google Chrome", pid=4242, window="0x1a2b", title_hint=hint)


_OUTLOOK = _app("OUTLOOK.EXE", "Outlook")
_NOTEPAD = _app("notepad.exe", "Notepad", pid=7, window="0x10")
_SLACK = _app("slack.exe", "Slack")
_SELF = _app("fiorivoce.exe", "FioriVoce", is_self=True)


def _job_spec(mode="dictation", focus=None, target=None, recognition=None, source="hotkey",
              route=""):
    return {"mode": mode, "focus": focus, "target": target, "recognition": recognition,
            "source": source, "route": route}


def _focus(identity=None, text=None):
    return {"identity": identity, "text": text}


_JOIN_CONTEXTS = [
    _caret("Dear Sam,"), _caret("I need"), _caret("Done."), _caret("I need", " for the trip."),
    _caret("Buy", "Next sentence."), _caret("Dear Sam,", known=False), None,
]
_JOIN_TEXTS = ["Thanks for the notes.", "Milk and eggs.", "Next item.", "", "- Milk"]

_TEXT_CASES = []
for _ctx in _JOIN_CONTEXTS:
    for _text in _JOIN_TEXTS:
        _TEXT_CASES.append([_text, _job_spec(focus=_focus(_OUTLOOK, _ctx))])
for _mode in ("command", "transform", "action", "walkthrough"):
    _TEXT_CASES.append(["Thanks.", _job_spec(mode=_mode, focus=_focus(_OUTLOOK,
                                                                     _caret("Dear Sam,")))])
_TEXT_CASES += [
    ["Thanks.", None],
    ["Thanks.", _job_spec()],
    ["Thanks.", _job_spec(focus=_focus(None, None))],
    ["Thanks.", _job_spec(focus=_focus(_OUTLOOK, None))],
]

_HTML_CASES = [
    ["<b>Thanks</b>", "Thanks", _job_spec(focus=_focus(_OUTLOOK, _caret("Dear Sam,")))],
    ["<b>Thanks</b>", "Thanks", _job_spec(mode="command",
                                          focus=_focus(_OUTLOOK, _caret("Dear Sam,")))],
    ["<b>Thanks</b>", "Thanks", None],
    ["", "Thanks", _job_spec(focus=_focus(_OUTLOOK, _caret("Dear Sam,")))],
    ["We should check see <b>the doc</b>.", "We should check see the doc.",
     _job_spec(focus=_focus(_OUTLOOK, _caret("and then I think", " by Friday")))],
    ["<i>Milk.</i>", "Milk.", _job_spec(focus=_focus(_OUTLOOK, _caret("Buy", "Next sentence.")))],
    ["<b>Hi</b>", "Hi", _job_spec(focus=_focus(_OUTLOOK, _caret("Dear Sam,", known=False)))],
    ["<b>Dana Lee</b>", "Dana Lee", _job_spec()],
]

_GMAIL = _chrome("Gmail")
_WHATSAPP = _chrome("WhatsApp")
_UNKNOWN_SITE = _chrome("")
_PASTE_CASES = []
for _current in (_GMAIL, _WHATSAPP, _UNKNOWN_SITE, _NOTEPAD, None):
    _PASTE_CASES += [
        [_job_spec(mode="transform", target=_focus(_GMAIL)), _current],
        [_job_spec(mode="transform", focus=_focus(_GMAIL)), _current],
        [_job_spec(mode="command", focus=_focus(_NOTEPAD), target=_focus(_GMAIL)), _current],
    ]
_PASTE_CASES += [
    [None, _GMAIL],
    [_job_spec(mode="transform"), _GMAIL],
    [_job_spec(mode="transform", target=_focus(None, _caret("x"))), _GMAIL],
    [_job_spec(mode="dictation", focus=_focus(_NOTEPAD)), _NOTEPAD],
    [_job_spec(mode="transform", target=_focus(_app("notepad.exe", "Notepad", pid=8,
                                                    window="0x10"))), _NOTEPAD],
]

_OVERRIDES = {"app_style_overrides": [{"match": "Slack", "category": "personal"}]}
_RECOGNITION = {"language": "fr", "phrases": ["Siobhan"]}
_HISTORY_CASES = [
    [None, None, True, {}],
    [None, None, False, {}],
    [None, "medium", False, {}],
    [_job_spec(), None, True, {}],
    [_job_spec(), None, False, {}],
    [_job_spec(focus=_focus(_OUTLOOK)), "light", True, {}],
    [_job_spec(focus=_focus(_SLACK), recognition=_RECOGNITION), "custom", True, {}],
    [_job_spec(focus=_focus(_SLACK), recognition=_RECOGNITION), "custom", True, _OVERRIDES],
    [_job_spec(focus=_focus(_GMAIL)), "", True, {}],
    [_job_spec(focus=_focus(_app("zzunknown.exe"))), None, True, {}],
    [_job_spec(focus=_focus(_app("", "Nameless"))), None, True, {}],
    [_job_spec(focus=_focus(_SELF)), None, True, {}],
    [_job_spec(focus=_focus(None, _caret("x"))), None, True, {}],
    [_job_spec(mode="command", focus=_focus(_NOTEPAD)), "profile", True, {}],
    [_job_spec(mode="transform"), None, False, {}],
    [_job_spec(mode="action", recognition={"language": "", "phrases": []}), None, True, {}],
    [_job_spec(mode="walkthrough", recognition=_RECOGNITION), "high", True, {}],
]

_STATS_CASES = [
    ["", ""], ["Hello there.", "Hello there."], ["I met Shivon.", "I met Siobhan."],
    ["one two three", "one three"], ["one three", "one two three"],
    ["teh cat sat on teh mat", "the cat sat on the mat"], ["你好世界", "你好世界!"],
    ["Hello, world", "Hello world"], ["a b c", "x y z w"],
]

_CLEANUP_CASES = [
    [{}, False], [{}, True],
    [{"transcript_cleanup_enabled": True}, False],
    [{"transcript_cleanup_enabled": True, "transcript_cleanup_level": "light"}, False],
    [{"transcript_cleanup_enabled": True, "transcript_cleanup_level": "high"}, False],
    [{"transcript_cleanup_enabled": True, "transcript_cleanup_level": "bogus"}, False],
    [{"transcript_cleanup_enabled": "yes", "transcript_cleanup_level": "high"}, False],
    [{"transcript_cleanup_enabled": True,
      "transcript_cleanup_prompt": "Make it sound like a pirate."}, False],
    [{"transcript_cleanup_enabled": False,
      "transcript_cleanup_prompt": "Make it sound like a pirate."}, False],
    [{"transcript_cleanup_enabled": True, "transcript_cleanup_prompt": "   "}, False],
    [{"transcript_cleanup_enabled": True,
      "transcript_cleanup_prompt": "Make it sound like a pirate."}, True],
]

_FOCUS_CASES = []
for _settings in ({}, {"app_context_enabled": False}, {"app_context_read_text": True},
                  {"app_context_enabled": False, "app_context_read_text": True},
                  {"app_context_enabled": "yes", "app_context_read_text": True}):
    for _mode in ("dictation", "command", "transform", "action", "walkthrough"):
        for _source in ("hotkey", "ui", "dbus", "api", "mcp", ""):
            for _route, _include in (("", False), ("auto", False), ("", True)):
                _FOCUS_CASES.append([_mode, _settings, _source, _route, _include])

CASES = {
    "text_for_paste": _TEXT_CASES,
    "html_for_paste": _HTML_CASES,
    "paste_target": _PASTE_CASES,
    "history_fields": _HISTORY_CASES,
    "stats_counts": _STATS_CASES,
    "cleanup_level": _CLEANUP_CASES,
    "_focus_requests": _FOCUS_CASES,
    "rich_paste": [
        ["Here is my calendar link.", "Hi Sam,", "", True],
        ["Here is my calendar link.", "Thanks", " and talk soon", True],
        ["My sign off.", "Regards,", "", True],
        ["My sign off.", "Dear Sam,", "", False],
    ],
}

# M4.9 dictation-cleanup: the cleanup prompt of one pass and what Stats count from a prepared
# transcript with snippets and the dictionary. compose_cleanup_prompt runs prepare_text on the
# case's raw words first (as _maybe_cleanup_transcript does) and returns the cleanup input with
# the prompt; a profile travels as CleanupProfile's fields.


def _compose_cleanup_prompt(module, settings, job, profile, rules, raw, batch_context):
    from services.cleanup_profiles import CleanupProfile

    built = _job(module, job)
    prepared = module.prepare_text(raw, built, settings)
    chosen = None if profile is None else CleanupProfile(**profile)
    prompt = module.compose_cleanup_prompt(job=built, settings=settings, profile=chosen,
                                           rules=rules, prepared=prepared,
                                           batch_context=batch_context)
    return {"cleanup_input": prepared.cleanup_input, "prompt": prompt}


def _prepared_stats_counts(module, raw, job, settings):
    built = _job(module, job)
    prepared = module.prepare_text(raw, built, settings)
    return {"text": prepared.text, "cleanup_input": prepared.cleanup_input,
            "skip_cleanup": prepared.skip_cleanup, "counts": module.stats_counts(prepared)}


WRAPPERS.update({
    "compose_cleanup_prompt": _compose_cleanup_prompt,
    "prepared_stats_counts": _prepared_stats_counts,
})

_TERMINAL = _app("windowsterminal.exe", "Windows Terminal", pid=8, window="0x8")
_CODE = _app("code.exe", "VS Code", pid=6)
_WHATSAPP_APP = _app("whatsapp.exe", "WhatsApp", pid=3)
_NEW_INSTALL = {"app_style_tones": {"email": "formal", "work": "casual", "personal": "casual",
                                    "other": "formal"}}
_DICTIONARY = {"dictation_dictionary": [
    {"id": "a", "term": "Ksenia", "heard": ["Sonia"], "starred": True},
    {"id": "b", "term": "Kubernetes"},
    {"id": "c", "term": "Acme", "learned": True, "protected": True},
]}
_SNIPPETS = {"dictation_snippets": [
    {"id": "cal", "trigger": "my calendar link", "text": "https://cal.example.com/dana",
     "formatted": False},
    {"id": "sig", "trigger": "my sign off", "text": "**Dana Lee**", "formatted": True},
]}
_ON = {"transcript_cleanup_enabled": True}
_RULES = ["Spell Acme correctly.", "Expand SCWA."]
_EMAIL_PROFILE = {"id": "email", "name": "Email",
                  "instructions": "Format as an email with a subject line.", "hotkey": "",
                  "use_learned_rules": True}
_NOTES_PROFILE = {"id": "notes", "name": "Notes", "instructions": "Make bullet notes.",
                  "hotkey": "", "use_learned_rules": False}
_RAW = "so um first milk second eggs and my calendar link for sonia"


def _with(*parts):
    merged = {}
    for part in parts:
        merged.update(part)
    return merged


_COMPOSE_CASES = []
for _settings in (
    {}, _ON, _with(_ON, {"transcript_cleanup_level": "light"}),
    _with(_ON, {"transcript_cleanup_level": "high"}),
    _with(_ON, {"transcript_cleanup_level": "bogus"}),
    _with(_ON, {"transcript_cleanup_prompt": "  Bullet points only.  "}),
    _with(_ON, {"transcript_cleanup_prompt": "   "}),
    _with(_ON, {"app_styles_enabled": False}),
):
    for _rules in ([], _RULES):
        _COMPOSE_CASES.append([_settings, _job_spec(focus=_focus(_NOTEPAD)), None, _rules,
                               "first milk second eggs", None])
for _who in (_OUTLOOK, _SLACK, _WHATSAPP_APP, _TERMINAL, _CODE, _GMAIL, _SELF, None):
    for _settings in (_ON, _with(_ON, _NEW_INSTALL),
                      _with(_ON, _NEW_INSTALL, {"app_styles_enabled": False})):
        _COMPOSE_CASES.append([_settings, _job_spec(focus=_focus(_who)), None, [],
                               "first milk second eggs", None])
for _mode in ("dictation", "command", "transform", "action"):
    _COMPOSE_CASES.append([_with(_ON, _NEW_INSTALL), _job_spec(mode=_mode, focus=_focus(_TERMINAL)),
                           None, [], "first milk", None])
    _COMPOSE_CASES.append([_ON, _job_spec(mode=_mode, focus=_focus(None)), None, [],
                           "first milk", None])
_COMPOSE_CASES += [
    # No capture at all (app awareness off) and uploads.
    [_ON, _job_spec(), None, [], "first milk", None],
    [_ON, None, None, _RULES, "first milk", None],
    [_ON, None, None, _RULES, "first milk", "Two halves of one call."],
    [_with(_ON, {"transcript_cleanup_level": "high"}), _job_spec(focus=_focus(None)), None,
     _RULES, "first milk", "Part 2 follows part 1."],
    # The dictionary, protected words and the snippet guard.
    [_with(_ON, _DICTIONARY), _job_spec(focus=_focus(_NOTEPAD)), None, [], _RAW, None],
    [_with(_ON, _DICTIONARY, _SNIPPETS), _job_spec(focus=_focus(_NOTEPAD)), None, [], _RAW, None],
    [_with(_ON, _SNIPPETS), _job_spec(focus=_focus(_NOTEPAD)), None, [], "my sign off", None],
    [_with(_ON, _SNIPPETS, {"snippets_enabled": False}), _job_spec(focus=_focus(_NOTEPAD)), None,
     [], _RAW, None],
    [_with(_ON, _SNIPPETS), _job_spec(mode="command", focus=_focus(_NOTEPAD)), None, [], _RAW,
     None],
    # Caret text read and not read.
    [_with(_ON, _NEW_INSTALL, {"app_context_read_text": True}),
     _job_spec(focus=_focus(_SLACK, _caret("Thanks for the notes, I", " by Friday."))), None, [],
     "so i'll send it", None],
    [_with(_ON, _NEW_INSTALL),
     _job_spec(focus=_focus(_SLACK, _caret("Thanks for the notes, I", " by Friday."))), None, [],
     "so i'll send it", None],
    [_with(_ON, {"app_context_read_text": True}),
     _job_spec(focus=_focus(_OUTLOOK, _caret("Dear Sam,", known=False))), None, [],
     "thanks for the notes", None],
    [_with(_ON, {"app_context_read_text": True}), _job_spec(focus=_focus(_NOTEPAD)), None, [],
     "thanks for the notes", None],
    [_with(_ON, _DICTIONARY, _SNIPPETS, _NEW_INSTALL, {"app_context_read_text": True}),
     _job_spec(focus=_focus(_SLACK, _caret("Hi team,"))), None, _RULES, _RAW, "Batch notes."],
    # Profiles: a profile always wins over styles and the list note.
    [_ON, _job_spec(focus=_focus(_NOTEPAD)), _EMAIL_PROFILE, _RULES, "first milk", None],
    [{}, _job_spec(focus=_focus(_SLACK)), _EMAIL_PROFILE, _RULES, "first milk", None],
    [_with(_NEW_INSTALL, _DICTIONARY, _SNIPPETS, {"app_context_read_text": True}),
     _job_spec(focus=_focus(_SLACK, _caret("Hi team,"))), _NOTES_PROFILE, _RULES, _RAW, None],
    [_ON, None, _NOTES_PROFILE, [], "first milk", "ctx"],
]

_STATS_PREPARED_CASES = []
for _raw in ("my sign off", "My sign off.", "here is my calendar link thanks",
             "ask sonia about kubernetes", "ask sonia about my calendar link",
             "just words", ""):
    for _spec in (_job_spec(), None, _job_spec(mode="command")):
        _STATS_PREPARED_CASES.append([_raw, _spec, _with(_DICTIONARY, _SNIPPETS)])
_STATS_PREPARED_CASES.append(["my sign off", _job_spec(),
                              _with(_SNIPPETS, {"snippets_enabled": False})])

CASES.update({
    "compose_cleanup_prompt": _COMPOSE_CASES,
    "prepared_stats_counts": _STATS_PREPARED_CASES,
})
