"""Curated inputs for services/snippets.py (M4.1 text-core): normalizing, loading, validation,
trigger notes, planning, placeholders, expansion and light Markdown.

A Snippet travels as its to_dict() ({id, trigger, text, formatted}); a SnippetPlan as
{text, whole: snippet or None, placeholders: [[token, snippet], ...]}.

    load_snippets(settings)              the snippets, as dicts
    validate_snippet(snippet, settings)  None, or {"error": the ValueError's message}
    save_snippet(settings, snippet)      the settings after, or {"error": ...}
    delete_snippet(settings, id)         the settings after
    find_duplicate(trigger, snippets, exclude_id)   the snippet or None
    trigger_note(trigger, snippets, exclude_id)
    plan_expansion(text, snippets)       the plan
    prompt_guard(text, snippets)         prompt_guard(plan_expansion(text, snippets))
    expand(cleaned, text, snippets)      expand(cleaned, plan_expansion(text, snippets)) as
                                         [plain, html, ok]
    plain_text(snippet), snippet_html(snippet)
"""

import copy

_KEY = "dictation_snippets"

CAL = {"id": "cal", "trigger": "my calendar link",
       "text": "https://cal.example.com/dana?ref=email#slots", "formatted": False}
EMAIL = {"id": "email", "trigger": "my email", "text": "dana@example.com", "formatted": False}
EMAIL_ADDRESS = {"id": "address", "trigger": "my email address", "text": "dana.lee@example.com",
                 "formatted": False}
SIGN_OFF = {"id": "sig", "trigger": "my sign-off",
            "text": "**Dana Lee**\nProduct designer\n[dana.design](https://dana.design)",
            "formatted": True}
ALL = [CAL, EMAIL, EMAIL_ADDRESS, SIGN_OFF]
TRICKY = {"id": "t", "trigger": "my tricky one", "text": "literal [[S2]] stays", "formatted": False}
PLAIN_LINES = {"id": "p", "trigger": "my address", "text": "1 Main St\r\nSpringfield <IL> & co",
               "formatted": False}


def _snippet(fields):
    from services.snippets import Snippet

    return Snippet(fields["id"], fields["trigger"], fields["text"], fields.get("formatted", False))


def _plain(snippet):
    return None if snippet is None else snippet.to_dict()


def _plan(plan):
    return {"text": plan.text, "whole": _plain(plan.whole),
            "placeholders": [[token, _plain(snippet)] for token, snippet in plan.placeholders]}


def _snippets(items):
    return [_snippet(fields) for fields in items]


def _validate(module, snippet, settings):
    try:
        module.validate_snippet(_snippet(snippet), settings)
    except ValueError as exc:
        return {"error": str(exc)}
    return None


def _save(module, settings, snippet):
    changed = copy.deepcopy(settings)
    try:
        module.save_snippet(changed, _snippet(snippet))
    except ValueError as exc:
        return {"error": str(exc)}
    return changed


def _delete(module, settings, snippet_id):
    changed = copy.deepcopy(settings)
    module.delete_snippet(changed, snippet_id)
    return changed


def _expand(module, cleaned, text, snippets):
    plain, html, ok = module.expand(cleaned, module.plan_expansion(text, _snippets(snippets)))
    return [plain, html, ok]


WRAPPERS = {
    "load_snippets": lambda module, settings: [_plain(s) for s in module.load_snippets(settings)],
    "validate_snippet": _validate,
    "save_snippet": _save,
    "delete_snippet": _delete,
    "find_duplicate": lambda module, trigger, snippets, exclude_id="": _plain(
        module.find_duplicate(trigger, _snippets(snippets), exclude_id=exclude_id)),
    "trigger_note": lambda module, trigger, snippets, exclude_id="": module.trigger_note(
        trigger, _snippets(snippets), exclude_id=exclude_id),
    "plan_expansion": lambda module, text, snippets: _plan(
        module.plan_expansion(text, _snippets(snippets))),
    "prompt_guard": lambda module, text, snippets: module.prompt_guard(
        module.plan_expansion(text, _snippets(snippets))),
    "expand": _expand,
    "plain_text": lambda module, snippet: module.plain_text(_snippet(snippet)),
    "snippet_html": lambda module, snippet: module.snippet_html(_snippet(snippet)),
}


def _many(count):
    return [{"id": f"id{i}", "trigger": f"phrase number {i}", "text": "text", "formatted": False}
            for i in range(count)]


_MARKDOWN = [
    "Hi *there* & **you** <script>\n\n- one\n* [two](https://two.example.com/?a=1&b=2)\n"
    "3. snake_case_name\n4) see https://x.example.com/a_b.\nBye _for now_",
    "[click](javascript:alert(1))", "[click](file:///etc/passwd)", "[click](data:text/html,x)",
    "[site](www.example.com)", "[Site](WWW.Example.com)", "[mail](mailto:a@b.c)",
    "[call](TEL:+1 555)", "[call](tel:+1555)", r"\*not italic\*", r"\\ \` \# \+ \- \. \! \q",
    "**Agenda**\n- *one*\n2. [two](https://two.example.com)",
    "**Dana Lee**\nProduct designer\n[dana.design](https://dana.design)",
    "\r\n\r\nline one\r\nline two\rline three\n\n", "", "   ", "\n\n\n",
    "1. one\n2. two\n\n\ntext after\n\n- a\n- b\n\n\n",
    "01. zero one\n02. zero two", "7) seven\n8) eight", "1234567890. too long a number",
    "    - four spaces", "   + three spaces\n-\tTabbed", "-no space", "* ", "*  *",
    "** not bold **", "**bold**text**more**", "__under__score__", "a__b__c", "_a_b_", "x_y_ z",
    "*it*alic*", "**[bold link](https://x.y)**", "[**bold label**](https://x.y)",
    "[label with ] bracket](https://x.y)", "[url](https://x.y/a b)", "[url](https://x.y/a(b))",
    "see http://a.b/c?d=e&f=g, then https://x.y.) and https://z.w/'q'",
    "https://a.b/<tag> and \"https://q.r/\"", "<b>raw</b> & 'quotes' \"double\"",
    "[x](https://a.b/?q=\"y\")", "Zoë **Ünïcödé** _ñ_ *文字*", "١. Arabic-Indic one",
    "**multi\nline**", "*", "**", "_", "__", "[]()", "[a]()", "[a](b)",
]

CASES = {
    "normalize": [
        ["  My  Sign-Off!  "], ["Don’t, Acme.com"], ["?!"], [""], ["İSTANBUL Straße"],
        ["a_b c-d e.f. g..h"], ["don''t it’’s"], ["数字 ١٢٣ x"], ["éte"],
        ["MY CALENDAR-LINK."], ["the [[S1]] marker"],
    ],
    "load_snippets": [
        [{_KEY: [
            CAL, "not a dict", {"id": "x", "trigger": "no text"},
            {"id": "y", "trigger": "blank", "text": "   "},
            {"id": "z", "trigger": "a", "text": "too short a trigger"},
            {"id": "w", "trigger": "t" * 61, "text": "too long a trigger"},
            {"id": "v", "trigger": "too long a text", "text": "x" * 10_001},
            {"id": "cal", "trigger": "same id", "text": "duplicate id"},
            {"id": "u", "trigger": "My Calendar, Link!", "text": "duplicate trigger"},
            {"id": 7, "trigger": "bad id", "text": "text"},
            {**EMAIL, "formatted": "yes"}, SIGN_OFF,
        ]}],
        [{_KEY: "nope"}], [{}], [None],
        [{_KEY: _many(205)}],
        [{_KEY: [{"id": "  sp  ", "trigger": "  spaced trigger  ", "text": " keep spaces "},
                 {"id": "", "trigger": "empty id", "text": "x"},
                 {"id": "q", "trigger": "say [[S1]]", "text": "x"},
                 {"id": "r", "trigger": "x" * 60, "text": "y" * 10_000},
                 {"id": "s", "trigger": "!!", "text": "x"},
                 {"id": "t", "trigger": "ok trigger", "text": "x", "formatted": 1}]}],
    ],
    "validate_snippet": [
        [{"id": "n", "trigger": "", "text": "text"}, {_KEY: [CAL]}],
        [{"id": "n", "trigger": "!!", "text": "text"}, {_KEY: [CAL]}],
        [{"id": "n", "trigger": "x", "text": "text"}, {_KEY: [CAL]}],
        [{"id": "n", "trigger": "x" * 61, "text": "text"}, {_KEY: [CAL]}],
        [{"id": "n", "trigger": "say [[S1]]", "text": "text"}, {_KEY: [CAL]}],
        [{"id": "n", "trigger": "my phrase", "text": "  \n"}, {_KEY: [CAL]}],
        [{"id": "n", "trigger": "my phrase", "text": "x" * 10_001}, {_KEY: [CAL]}],
        [{"id": "n", "trigger": "MY CALENDAR-LINK.", "text": "text"}, {_KEY: [CAL]}],
        [{"id": "cal", "trigger": "My calendar link", "text": "new text"}, {_KEY: [CAL]}],
        [{"id": "new", "trigger": "one more phrase", "text": "text"}, {_KEY: _many(200)}],
        [{"id": "id3", "trigger": "phrase number 3", "text": "edited"}, {_KEY: _many(200)}],
        [{"id": "n", "trigger": "  ok  ", "text": "x"}, {}],
    ],
    "save_snippet": [
        [{}, CAL],
        [{_KEY: [CAL, EMAIL]},
         {"id": "cal", "trigger": "  my booking link ", "text": "https://cal.example.com/new"}],
        [{_KEY: [CAL]}, {"id": "n", "trigger": "my calendar link", "text": "dup"}],
        [{_KEY: [CAL, "junk"]}, {**EMAIL, "formatted": True}],
    ],
    "delete_snippet": [
        [{_KEY: [CAL, EMAIL]}, "cal"], [{_KEY: [CAL, EMAIL]}, "missing"],
        [{_KEY: [CAL, "junk"]}, "missing"], [{}, "x"],
    ],
    "find_duplicate": [
        ["My Email!", ALL, ""], ["my email", ALL, "email"], ["?!", ALL, ""],
        ["my-calendar link", ALL, ""],
    ],
    "trigger_note": [
        ["My Email!", ALL, ""], ["my email", ALL, "email"], ["send my calendar link", ALL, ""],
        ["my calender link", ALL, ""], ["signature", ALL, ""], ["my home address", ALL, ""],
        ["", ALL, ""], ["calendar", ALL, ""], ["my", ALL, ""], ["email address", ALL, ""],
        ["my sign off", ALL, "sig"], ["my sign off please", ALL, ""], ["word", [], ""],
    ],
    "plan_expansion": [
        [text, ALL] for text in [
            "My calendar link.", "my CALENDAR link", " my calendar, link! ",
            "Here's my Calendar link, thanks!",
            "Use my email address, or my email, or my email address",
            "Check my emails today", "That's my calendar. Link it later",
            "that was my sign off for today", "hello there", "", "The [[S1]] marker and my email",
            "send it to my email address and my calendar link", "mail my email please",
            "my sign off", "1 < 2 & my email\nthen my sign off", "my—email and my – email",
            "my email—address", "my, email,address", "My Email Address!", "MY EMAIL ADDRESS my",
            "my  email   address", "my\nemail", "my emaıl", "my email’s", "x my email",
        ]
    ] + [
        ["my calendar link", []],
        ["my tricky one and my email", [TRICKY, EMAIL]],
        ["one two three and one two", [
            {"id": "a", "trigger": "one two", "text": "A"},
            {"id": "b", "trigger": "one two three", "text": "B"},
            {"id": "c", "trigger": "two three", "text": "C"}]],
        ["ab cd and abcd", [
            {"id": "a", "trigger": "ab cd", "text": "A"},
            {"id": "b", "trigger": "abc d", "text": "B"}]],
        ["one two", [{"id": "a", "trigger": "one two", "text": "A"},
                     {"id": "b", "trigger": "ONE-TWO", "text": "B"}]],
        ["x one two y", [{"id": "a", "trigger": "One two", "text": "A"},
                         {"id": "b", "trigger": "one, two", "text": "B"}]],
        ["!!", [{"id": "a", "trigger": "!!", "text": "A"}]],
    ],
    "prompt_guard": [
        ["hello", ALL], ["my calendar link", ALL], ["mail my email", ALL],
        ["my email, my email address and my calendar link", ALL],
        ["my email and my email", ALL],
    ],
    "expand": [
        ["Send it to [[S1]] and [[S2]].", "send it to my email address and my calendar link",
         ALL],
        ["Mail [[ s1 ]], please.", "mail my email please", ALL],
        ["Mail it, please.", "mail my email please", ALL],
        ["Mail [[S1]] and [[S1]], please.", "mail my email please", ALL],
        ["Mail [[S1]] to [[S2]].", "mail my email please", ALL],
        ["My calendar link.", "My calendar link.", ALL],
        ["anything", "my sign off", ALL],
        ["1 < 2 & [[S1]]\nthen [[S2]]", "1 < 2 & my email\nthen my sign off", ALL],
        ["[[S1]] and [[S2]]", "my tricky one and my email", [TRICKY, EMAIL]],
        ["Untouched [[S1]]", "hello", ALL],
        ["At [[S1]] & [[S2]].", "at my address and my sign off", [PLAIN_LINES, SIGN_OFF]],
        ["At [[S1]] & [[S01]].", "at my address and my address", [PLAIN_LINES]],
        ["At [[S2]] then [[S1]].", "at my address and my email", [PLAIN_LINES, EMAIL]],
        ["At [[S١]] and [[S2]].", "at my address and my email", [PLAIN_LINES, EMAIL]],
        ["[[S1]]", "my sign off and my sign off", ALL],
    ],
    "plain_text": [[s] for s in ALL + [TRICKY, PLAIN_LINES]],
    "snippet_html": [[s] for s in ALL + [TRICKY, PLAIN_LINES]],
    "markdown_to_html": [[text] for text in _MARKDOWN],
    "markdown_to_plain": [[text] for text in _MARKDOWN],
}

TRACE = {
    "tests": ["tests/test_personalize_s5_snippets.py"],
    "functions": ["normalize", "markdown_to_html", "markdown_to_plain"],
}
