"""Route by voice's word model, gate, request and status (services/actions/routing.py, M5.8
voice-routing).

    _words(text)                          [[norm, start, end, pause, name], ...]
    _sound(word), _alike(heard, name)     as the module answers
    sounds(words)                         [_sound(w) for w in words]
    alike_pairs(heard, names)             [[h, n] for h in heard for n in names if _alike(h, n)]
    _name_variants(name, spoken_names)    [[words, dropped, after_lead_ins, as_name], ...]
    benchmark()                           {"cases": [[id, action_set, route, text, expect, tags]],
                                          "aliases": {set: [alias, ...]}} of the benchmark
    action_set(name)                      the action dicts of a named set below
    gate(text, actions, route)            [[action_id, first, name_start, name_end, leading], ...]
    _spans(text, actions, route)          _spans(text, _words(text), gate(text, ...))
    plan(text, route, actions)            None, or {"text", "candidates", "matched", "state",
                                          "questions": json.dumps(questions), "spans": [[option,
                                          span], ...]}
    read(text, route, actions, answers)   plan(...).read(answers): None, or [action_id,
                                          confidence, probability, payload]
    _clip, _sentence_start, _without_polite_end, route_budget   as the module answers
    status(settings, key_present)         [enabled, ready, message, fix] with typesafe.key_present
                                          answering key_present

Actions are named sets of dicts of Action's fields (action_set gives each). The sentences are those of OpenWhisper's routing
tests and every case of benchmarks/voice_routing/cases.py (read here, not copied), over the
enabled actions of the case's set; the questions JSON is json.dumps's text, so the replay
compares key order and bytes.
"""

import itertools
import json

from benchmarks.voice_routing.cases import ACTION_SETS, CASES as BENCHMARK
from services import typesafe
from services.actions import routing
from services.actions.model import PRESETS, Action

AUTO, HOTKEY = routing.ROUTE_AUTO, routing.ROUTE_HOTKEY


def _action(spec):
    return Action(id=spec["id"], name=spec["name"], kind=spec["kind"],
                  params=dict(spec.get("params") or {}),
                  spoken_names=tuple(spec.get("spoken_names") or ()),
                  description=spec.get("description", ""), enabled=spec.get("enabled", True))


def _spec(action):
    return {"id": action.id, "name": action.name, "kind": action.kind,
            "params": dict(action.params), "spoken_names": list(action.spoken_names),
            "description": action.description, "enabled": action.enabled}


def _actions(name):
    return [_action(spec) for spec in _SETS[name]]


def _action_set(module, name):
    return _SETS[name]


def _words(module, text):
    return [[w.norm, w.start, w.end, w.pause, w.name] for w in module._words(text)]


def _sounds(module, words):
    return [module._sound(word) for word in words]


def _alike_pairs(module, heard, names):
    return [[h, n] for h in heard for n in names if module._alike(h, n)]


def _name_variants(module, name, spoken_names):
    action = Action("x", name, "clipboard", {}, spoken_names=tuple(spoken_names))
    return [[list(v.words), list(v.dropped), v.after_lead_ins, v.after_lead_ins_as_name]
            for v in module._name_variants(action)]


def _gate(module, text, actions, route):
    return [[m.action_id, m.first, m.name_start, m.name_end, m.leading]
            for m in module.gate(text, _actions(actions), route=route)]


def _span_list(module, text, actions, route):
    return module._spans(text, module._words(text), module.gate(text, _actions(actions),
                                                                  route=route))


def _plan(module, text, route, actions):
    request = module.plan(text, route, _actions(actions))
    if request is None:
        return None
    return {"text": request.text, "candidates": list(request.candidates),
            "matched": list(request.matched), "state": request.state,
            "questions": json.dumps(request.questions),
            "spans": [[option, span] for option, span in request.spans.items()]}


def _read(module, text, route, actions, answers):
    judgment = module.plan(text, route, _actions(actions)).read(answers)
    if judgment is None:
        return None
    return [judgment.action_id, judgment.confidence, judgment.probability, judgment.payload]


def _status(module, settings, key_present):
    original = typesafe.key_present
    typesafe.key_present = lambda *args, **kwargs: key_present
    try:
        status = module.status(settings)
    finally:
        typesafe.key_present = original
    return [status.enabled, status.ready, status.message, status.fix]


def _benchmark(module):
    return {"cases": [[c.id, c.action_set, c.route, c.text, list(c.expect), list(c.tags)]
                      for c in BENCHMARK],
            "aliases": {name: list(actions) for name, actions in ACTION_SETS.items()}}


WRAPPERS = {
    "benchmark": _benchmark,
    "action_set": _action_set,
    "_words": _words,
    "sounds": _sounds,
    "alike_pairs": _alike_pairs,
    "_name_variants": _name_variants,
    "gate": _gate,
    "_spans": _span_list,
    "plan": _plan,
    "read": _read,
    "status": _status,
}

# -- the actions of OpenWhisper's routing tests ---------------------------------------------

_GATE_SET = [
    {"id": "c1", "name": "Ask Claude", "kind": "open_link",
     "params": {"template": "claude://claude.ai/new?q={text}"},
     "spoken_names": ["Claude", "hey Claude"]},
    {"id": "cc", "name": "Tell my Claude Code session", "kind": "channel",
     "spoken_names": ["Claude Code"]},
    {"id": "t1", "name": "Todo", "kind": "webhook", "params": {"url": "https://example.test/todo"},
     "spoken_names": ["to-do list"]},
    {"id": "s1", "name": "Send to my scripts", "kind": "scripts"},
    {"id": "g1", "name": "ChatGPT app", "kind": "open_link",
     "params": {"template": "https://chatgpt.com/?q={text}"}},
    {"id": "agent", "name": "Agent hotkey", "kind": "agent", "params": {"agent": "claude_code"}},
]
_PRESET_SET = [_spec(Action(id=p.id, name=p.name, kind=p.kind, params=dict(p.params)))
               for p in PRESETS] + [
    {"id": "todo", "name": "Todo", "kind": "webhook", "params": {"url": "http://127.0.0.1/x"}},
    {"id": "notes", "name": "Notes", "kind": "webhook", "params": {"url": "http://127.0.0.1/x"}},
]
_LOWERCASE_SET = [dict(spec, name=spec["name"].lower()) for spec in _PRESET_SET]
_DECIDE_SET = [
    {"id": "c1", "name": "Ask Claude", "kind": "open_link",
     "params": {"template": "claude://claude.ai/new?q={text}"}, "spoken_names": ["Claude"],
     "description": "Open a new Claude chat with my words."},
    {"id": "t1", "name": "Todo", "kind": "webhook",
     "params": {"url": "https://example.test/hook?token=secret"}, "spoken_names": ["to-do list"],
     "description": "Add an item to my todo list."},
]
_AGENTS_SET = [
    {"id": "h", "name": "Post to my webhook", "kind": "webhook",
     "params": {"url": "http://127.0.0.1/x"}},
    {"id": "home", "name": "Home Assistant", "kind": "webhook",
     "params": {"url": "http://127.0.0.1/x"}, "spoken_names": ["home"]},
    {"id": "x", "name": "Claude", "kind": "open_link", "params": {"template": "claude://x?q={text}"}},
    {"id": "ask", "name": "Quick answer", "kind": "agent",
     "params": {"agent": "codex", "mode": "ask"}, "description": "Ask Codex."},
    {"id": "oc", "name": "OpenCode", "kind": "agent", "params": {"agent": "opencode_cli"}},
    {"id": "un", "name": "Mystery", "kind": "agent", "params": {"agent": "someone"}},
    {"id": "pr", "name": "Journal", "kind": "program", "params": {"program": "C:/j.exe"},
     "description": "Écrire « une ligne » dans mon journal — vite."},
    {"id": "cp", "name": "Copy only", "kind": "clipboard"},
]

_GATE_TEXTS = [
    "Claude, what's the capital of Peru?",
    "Ask Claude what the capital of Peru is",
    "Hey Claude, um, what's a mutex?",
    "Um, okay so, hey Claude, what's a mutex?",
    "All right Claude, draft a reply",
    "Can you ask Claude how to center a div",
    "Send this to my scripts: rotate the logs",
    "Send to my scripts rotate the logs",
    "Todo: buy milk",
    "To-do list, call the plumber",
    "Agent, add a dark mode toggle",
    "ChatGPT, translate this",
    "Claude Code, run the tests",
    "chat GPT, give me a packing list",
    "To do, email the landlord",
    "Cloud, what time zone is Tokyo in?",
    "Claud, what's the boiling point?",
    "Hey clod, three lunch ideas",
    "Clawed code, run the linter",
    "I told Claude yesterday that the plan changed.",
    "Today I went to the store.",
    "Claude's answer was wrong.",
    "Agents are the hot topic this year.",
    "Hey team, Claude Code shipped a release.",
    "Please can you ask and tell the Claude team",
    "",
    "Um, uh.",
    "Buy oat milk, add it to my to-do list.",
    "Ask Claude Code why the tests fail",
    "Hey Claude, um, can you explain recursion simply, please.",
    "Send this to my scripts: deploy 'staging' now!",
    "What's the weather in Lisbon? Ask Claude.",
    "Remind me to call the dentist",
    "Claude.",
    "Use my webhook to post this.",
    "Restart the webhook.",
    "Claude said the deadline moved.",
    "Codex, write a test.",
    "   ",
    "Claude, what's two plus two?",
]
_REVIEW_TEXTS = [
    "Could you send me the report by Friday?",
    "Could we move the meeting to three?",
    "Called the bank this morning, they said it's fine.",
    "Cold brew is cheaper if you make it at home.",
    "Child care pickup is at five today.",
    "Cloudy skies all week, bring a jacket.",
    "Clued in the team about the outage.",
    "Killed the stale jobs on the build box.",
    "Okay so could you take a look at the PR?",
    "So to do that we need a new index.",
    "Now to do this properly we should refactor first.",
    "To do that later we'd need a second server.",
    "To do it right we need more time.",
    "Chat about it with the team tomorrow.",
    "Send the scripts to Bob by Friday.",
    "Send my scripts to Bob by Friday.",
    "Use the webhook from the staging config.",
    "Use my webhook settings from staging.",
    "Open the app and click settings.",
    "Cloud, what time zone is Tokyo in?",
    "Ask cloud to translate good morning.",
    "Hey clod, three lunch ideas",
    "Claud, what's the boiling point?",
    "Clawed code, run the linter",
    "Tell Claude to summarize this thread.",
    "Can you ask ChatGPT for a packing list?",
    "Send this to my scripts: rotate the logs.",
    "Send it to my scripts, rotate the logs.",
    "Webhook, the build finished.",
    "Agent, add a dark mode toggle.",
    "To do, email the landlord.",
    "To-do: call the plumber.",
    "Todo buy milk",
    "chat gpt what's 15 percent of 80",
    "Chat GPT what's 15% of 80",
    "chat gpt can you translate this",
    "To do buy milk",
    "To do email the landlord",
    "To do, email the landlord",
    "To do list, call the plumber",
    "To do with the budget, ask finance.",
    "To do: that thing for the landlord",
    "Buy milk, add it to my to do",
    "Buy milk, add it to my to do please",
    "Tell Claude to write a haiku.",
    "Can you tell Claude what time it is?",
    "Tell Claude Code to fix the failing test.",
    "Ask my Claude Code session to run the tests.",
    "Open ChatGPT and ask for a lasagna recipe.",
    "Ask claude to summarize this.",
    "Use agent mode for this one.",
    "Use my Webhook settings from staging.",
    "Use it. Scripts are in the shared drive.",
    "Use my webhook to post this.",
    "Restart the webhook.",
]
_EXTRA_TEXTS = [
    "Can you post to my webhook: the build is green",
    "Lock the front door, home.",
    "Lock the front door and head home.",
    "Ask Claude to summarize this",
    "Home, turn on the porch light",
    "Quick answer: what's 2+2, thank you",
    "Quick answer what's the time thanks",
    "Mystery, do the thing.",
    "OpenCode, refactor the parser please!",
    "Journal: écrire « bonjour » — merci, please.",
    "CLAUDE, SHOUT THIS",
    "ÉCRIRE: Claude, résume ça.",
    "Ask Claude — résumé du « projet » à 東京, please?",
    "Hey hey hey hey hey Claude, hi",
    "Can you ask to tell my Claude what's up",
    "Copy only this sentence, thank you.",
    "Tell my Claude Code session to rebase onto main",
    "Hey Claude 😀, what's this emoji?",
    "home",
    "please",
    "thank you",
    "Claude\n\nnew paragraph\n",
    "  Claude,   spaced   out  ",
    "Turn off the lights, home. Thanks.",
    "It's for you, Claude",
]

_TEXT_SETS = [(_GATE_TEXTS, "gate"), (_REVIEW_TEXTS, "presets"), (_REVIEW_TEXTS, "lowercase"),
              (_GATE_TEXTS, "decide"), (_EXTRA_TEXTS, "agents"), (_EXTRA_TEXTS, "gate")]


def _enabled(case):
    return "benchmark:" + case.action_set


_LONG = "Claude, " + "lorem ipsum dolor sit amet " * 800 + "the end."
_WORDY = [
    {"id": f"a{i}", "name": f"Action number {i}", "kind": "scripts",
     "spoken_names": [f"spoken {i} {w}" for w in "abcdefgh"],
     "description": "A long description of what this does. " * 7}
    for i in range(50)
]

_SETS = {
    "gate": _GATE_SET, "presets": _PRESET_SET, "lowercase": _LOWERCASE_SET,
    "decide": _DECIDE_SET, "agents": _AGENTS_SET, "wordy": _WORDY, "empty": [],
    "none-id": [dict(_DECIDE_SET[0], id="none")],
    "repeated-id": [dict(_DECIDE_SET[0]), dict(_DECIDE_SET[0], name="Ask")],
}
for _name, _set in ACTION_SETS.items():
    _SETS["benchmark:" + _name] = [_spec(a) for a in _set.values() if a.enabled]


def _gate_cases():
    cases = []
    for texts, actions in _TEXT_SETS:
        for text, route in itertools.product(texts, (AUTO, HOTKEY)):
            cases.append([text, actions, route])
    for case in BENCHMARK:
        for route in (AUTO, HOTKEY):
            cases.append([case.text, _enabled(case), route])
    return cases


def _plan_cases():
    cases = []
    for texts, actions in _TEXT_SETS:
        routes = (AUTO, HOTKEY) if actions in ("gate", "decide", "agents") else (AUTO,)
        for text, route in itertools.product(texts, routes):
            cases.append([text, route, actions])
    for case in BENCHMARK:
        cases.append([case.text, case.route, _enabled(case)])
    cases += [
        [_LONG, AUTO, "decide"],
        ["Do the thing for me please.", HOTKEY, "wordy"],
        ["Action number 7, do the thing for me please.", HOTKEY, "wordy"],
        ["Claude, hi", "", "decide"],
        ["Claude, hi", "other", "decide"],
        ["Claude, hi", AUTO, "empty"],
        ["Remind me later", HOTKEY, "none-id"],
        ["Claude, none of this", AUTO, "repeated-id"],
        ["Claude, " + "x" * 400 + " end", AUTO, "decide"],
    ]
    return cases


def _words_of_benchmark():
    seen = []
    for case in BENCHMARK:
        for word in routing._words(case.text):
            if word.norm not in seen:
                seen.append(word.norm)
    return seen


def _name_words():
    seen = []
    for actions in list(ACTION_SETS.values()):
        for action in actions.values():
            for phrase in (action.name, *action.spoken_names):
                for word in routing._words(phrase):
                    if word.norm not in seen:
                        seen.append(word.norm)
    for spec in _GATE_SET + _PRESET_SET + _AGENTS_SET:
        for phrase in (spec["name"], *spec.get("spoken_names", ())):
            for word in routing._words(phrase):
                if word.norm not in seen:
                    seen.append(word.norm)
    return seen


def _answers(text, route, actions, choice, confidence, payload_index=0):
    """answering() of test_automation_routing_decide over this plan's options."""
    request = routing.plan(text, route, _actions(actions))
    options = list(request.questions["route"]["criteria"])
    rest = (1 - confidence) / max(1, len(options) - 1)
    answers = {"route": {"type": "choice", "choice": choice, "confidence": confidence,
                         "probabilities": {k: (confidence if k == choice else rest)
                                           for k in options}}}
    if "payload" in request.questions:
        spans = list(request.questions["payload"]["criteria"])
        picked = spans[payload_index]
        answers["payload"] = {"type": "choice", "choice": picked, "confidence": 0.9,
                              "probabilities": {k: (0.9 if k == picked else 0.1 / (len(spans) - 1))
                                                for k in spans}}
    return answers


def _payload_options(text, route, actions):
    return list(routing.plan(text, route, _actions(actions)).questions["payload"]["criteria"])


_ASKED = "Claude, what's two plus two?"
_POLITE = "Hey Claude, um, can you explain recursion simply, please."
_SPANS = _payload_options(_POLITE, AUTO, "gate")


def _read_cases():
    cases = []
    for choice, confidence, index in (("c1", 0.93, 0), ("c1", 0.5, 1), ("c1", 0.49, 0),
                                      ("none", 0.99, 0), ("c1", 0.9, 2)):
        cases.append([_ASKED, AUTO, "decide",
                      _answers(_ASKED, AUTO, "decide", choice, confidence, min(index, 2))])
    for index in range(len(_SPANS)):
        cases.append([_POLITE, AUTO, "gate", _answers(_POLITE, AUTO, "gate", "c1", 0.8, index)])
    for text in ("Buy oat milk, add it to my to-do list.", "Remind me to call the dentist."):
        cases.append([text, HOTKEY, "decide", _answers(text, HOTKEY, "decide", "t1", 0.9)])
    spans = _payload_options(_ASKED, AUTO, "decide")
    canned = [
        None, {}, [], {"route": None}, {"route": []}, {"route": {"choice": "gone",
                                                                  "confidence": 1.0}},
        {"payload": {"choice": "none"}}, {"route": {"choice": 5}},
        {"route": {"choice": "c1", "confidence": 0.9}},
        {"route": {"choice": "c1", "confidence": 0.9}, "payload": {"choice": spans[0]}},
        {"route": {"choice": "c1", "confidence": 0.9}, "payload": {"choice": "missing"}},
        {"route": {"choice": "c1", "confidence": 0.9}, "payload": "text"},
        {"route": {"choice": "c1", "confidence": 0.9}, "payload": {"choice": None}},
        {"route": {"choice": "c1", "confidence": 0.9, "probabilities": {"c1": 0.95}},
         "payload": {"choice": spans[0], "confidence": 0.2,
                     "probabilities": {spans[0]: 0.4, spans[1]: 0.3, "none": 0.3}}},
        {"route": {"choice": "c1", "confidence": 0.9},
         "payload": {"probabilities": {spans[0]: 0.5, spans[1]: 0.5}}},
        {"route": {"choice": "c1", "confidence": 0.9},
         "payload": {"probabilities": {spans[1]: 0.25, "none": 0.5, spans[0]: 0.5}}},
        {"route": {"choice": "c1", "confidence": 0.9},
         "payload": {"probabilities": {spans[0]: "high", spans[1]: True}}},
        {"route": {"choice": "c1", "confidence": 0.9},
         "payload": {"probabilities": {spans[0]: None}, "choice": spans[0]}},
        {"route": {"choice": "c1", "confidence": 0.9},
         "payload": {"probabilities": {}, "choice": spans[0]}},
        {"route": {"choice": "c1", "confidence": 0.9},
         "payload": {"probabilities": {"unknown": 0.9, spans[0]: 0.2}}},
        {"route": {"choice": "c1", "confidence": "0.75"}},
        {"route": {"choice": "c1", "confidence": True}},
        {"route": {"choice": "c1", "confidence": None, "probabilities": {"c1": 0.4}}},
        {"route": {"choice": "c1", "confidence": 0, "probabilities": []}},
        {"route": {"choice": "c1", "confidence": 0.8, "probabilities": ["c1"]}},
        {"route": {"choice": "c1", "confidence": 0.8, "probabilities": {"c1": "0.6"}}},
        {"route": {"choice": "none", "confidence": 0.8, "probabilities": {"t1": 0.6}}},
        {"route": {"choice": "c1", "confidence": "abc"}},
        {"route": {"choice": "c1", "confidence": [1]}},
        {"route": {"choice": "c1", "confidence": 0.8, "probabilities": {"c1": None}}},
        {"route": {"choice": "c1", "confidence": 0.8}, "payload": {"choice": [spans[0]]}},
    ]
    for answers in canned:
        cases.append([_ASKED, AUTO, "decide", answers])
    return cases


def _settings_battery():
    batteries = []
    actions = {"present": [{"id": "c1", "name": "Ask Claude", "kind": "clipboard",
                            "spoken_names": ["Claude"]}],
               "off": [{"id": "c1", "name": "Ask Claude", "kind": "clipboard",
                        "spoken_names": ["Claude"], "enabled": False}],
               "none": [], "garbage": "garbage"}
    hotkeys = {"set": {"route_record": "ctrl+alt+r"}, "padded": {"route_record": "  ctrl+alt+r "},
               "blank": {"route_record": "   "}, "empty": {}, "null": None,
               "number": {"route_record": 5}, "list": ["route_record"], "missing": "missing"}
    providers = {
        "default": {},
        "typesafe": {"typesafe_provider": "typesafe"},
        "openrouter": {"typesafe_provider": "openrouter"},
        "ollama": {"typesafe_provider": "text_model", "meeting_llm_provider": "ollama",
                   "meeting_llm_model": "qwen3:4b"},
        "cleanup": {"typesafe_provider": "text_model", "typesafe_text_model": "cleanup",
                    "transcript_cleanup_provider": "openrouter",
                    "transcript_cleanup_model": "vendor/small-model"},
    }
    for enabled, typesafe_on, provider, acts, keys, on_dictation, key in itertools.product(
            (True, False), (True, False), providers, actions, ("set", "empty"), (True, False),
            (True, False)):
        if not enabled and (provider != "default" or acts != "present"):
            continue
        if not typesafe_on and (acts != "present" or keys != "set"):
            continue
        settings = {"voice_routing_enabled": enabled, "typesafe_enabled": typesafe_on,
                    "voice_routing_on_dictation": on_dictation, "actions": actions[acts],
                    "hotkeys": hotkeys[keys], **providers[provider]}
        batteries.append([settings, key])
    for name, value in hotkeys.items():
        settings = {"voice_routing_enabled": True, "typesafe_enabled": True,
                    "actions": actions["present"]}
        if value != "missing":
            settings["hotkeys"] = value
        batteries.append([settings, True])
    batteries += [[{}, True], [{}, False], [None, True], [{"voice_routing_enabled": "yes"}, True],
                  [{"voice_routing_enabled": 1, "typesafe_enabled": 1, "actions": actions["present"],
                    "voice_routing_on_dictation": 1}, True]]
    return batteries


CASES = {
    "benchmark": [[]],
    "action_set": [[name] for name in _SETS],
    "_words": [
        ["Tell Claude. Claude, ChatGPT and okay? Scripts"],
        ["Hey Claude, um, can you explain recursion simply, please."],
        ["don't stop the to-do‐list ’n’ rock-and-roll's ‘quoted’ _under_score_ x_y"],
        ["ÉCRIRE: Élan, l'Ête; Straße STRASSE ǅemal ﬁne İstanbul"],
        ["東京 は 晴れ。Claude 😀 ok… done— next – last"],
        ["Mr. Smith asked Dr. Who? Yes! Really… Sure"],
        ["  leading and trailing  "], [""], ["   "], ["123 abc4 4abc 12-34 a1-b2"],
        ["Ask Claude\nwhat's up\tnow"], ["a-b-c' d'' e--f -g h-"],
    ] + [[case.text] for case in BENCHMARK],
    "_sound": [[w] for w in (
        "claude", "cloud", "clod", "clawed", "clawd", "claud", "could", "called", "cold", "child",
        "killed", "cloudy", "clued", "claudes", "codex", "codecs", "today", "todo", "agent",
        "agents", "urgent", "chatgpt", "phone", "quick", "xray", "jazz", "rhythm", "y", "yes",
        "eye", "code", "made", "tossed", "added", "seed", "", "bcd", "école", "über", "naïve",
        "street", "aaa", "queue", "phish", "ck", "boy", "rain", "new", "few")],
    "_alike": [list(pair) for pair in (
        ("cloud", "claude"), ("clod", "claude"), ("claud", "claude"), ("clawed", "claude"),
        ("clawd", "claude"), ("codecs", "codex"), ("could", "claude"), ("called", "claude"),
        ("cold", "claude"), ("child", "claude"), ("killed", "claude"), ("cloudy", "claude"),
        ("clued", "claude"), ("claudes", "claude"), ("today", "todo"), ("agents", "agent"),
        ("urgent", "agent"), ("todo", "todo"), ("cat", "cat"), ("cat", "cats"),
        ("klawd", "claude"), ("cloudfront", "claude"), ("école", "ecole"))],
    "sounds": [[_words_of_benchmark()], [_name_words()]],
    "alike_pairs": [[_words_of_benchmark(), _name_words()]],
    "_name_variants": [
        ["Ask Claude", ["Claude", "hey Claude"]], ["Tell my Claude Code session", ["Claude Code"]],
        ["Todo", ["to-do list"]], ["Send to my scripts", []], ["ChatGPT app", []],
        ["Agent hotkey", []], ["Webhook", []], ["Post to my webhook", []],
        ["Home Assistant", ["home", "house"]], ["ask claude", []],
        ["tell my claude code session", []], ["send to my scripts", []], ["My app", []],
        ["To", []], ["a b", []], ["Use the", ["", "  ", "...", "Hey"]],
        ["Session chat app", []], ["Can you add to my list please", []],
        ["Quick answer", ["quick question"]], ["Copy only", ["copy this"]],
        ["Dear diary", ["dear diary", "Dear Diary"]], ["", ["Claude"]], ["Écrire", ["école"]],
    ],
    "gate": _gate_cases(),
    "_spans": [
        [_POLITE, "gate", AUTO],
        ["Send this to my scripts: deploy 'staging' now!", "gate", AUTO],
        ["What's the weather in Lisbon? Ask Claude.", "gate", HOTKEY],
        ["Ask Claude Code why the tests fail", "gate", AUTO],
        ["Claude.", "gate", AUTO],
        ["Claude, to that about can you, please do it, thanks!", "gate", AUTO],
        ["Buy milk, add it to my to do please", "presets", HOTKEY],
        ["Claude, thank you.", "gate", AUTO],
        ["Claude, um, thanks; please.", "gate", AUTO],
        ["  Claude —  spaced out please  ", "gate", AUTO],
        ["Turn off the lights, home. Thanks.", "agents", HOTKEY],
    ],
    "plan": _plan_cases(),
    "read": _read_cases(),
    "_clip": [
        ["short", 3, 2], ["abcdefghij", 3, 2], ["abcdefgh", 3, 2], ["abcdefghi", 3, 2],
        ["東京東京東京東京東京", 3, 2], ["😀😀😀😀😀😀😀😀😀", 3, 2], ["", 3, 2],
        ["x" * 1903, 1500, 400], ["x" * 1904, 1500, 400],
    ],
    "_sentence_start": [
        ["what's up"], ["What's up"], ["iPhone sales"], ["éclair"], ["ßtraße"], ["ǆ"],
        ["a"], [""], ["1abc"], ["ﬁne"], ["x"], ["xY"], ["xy"],
    ],
    "_without_polite_end": [
        ["explain recursion simply, please."], ["explain recursion, Thanks!"],
        ["do it, thank you."], ["please"], ["thanks!"], ["say please now"],
        ["call mom PLEASE?!"], ["ok , ; please ..."], ["pleased to meet you"],
        ["thanks a lot, please"], ["foo please\n"], ["foo — please"],
    ],
    "route_budget": [[settings] for settings in (
        {}, {"typesafe_provider": "typesafe"}, {"typesafe_provider": "openrouter"},
        {"typesafe_provider": "text_model", "meeting_llm_provider": "ollama",
         "meeting_llm_model": "qwen3:4b"},
        {"typesafe_provider": "text_model", "typesafe_text_model": "cleanup",
         "transcript_cleanup_provider": "openrouter"},
        {"typesafe_provider": "text_model"}, {"typesafe_provider": "unknown"},
    )],
    "status": _settings_battery(),
}
