"""Curated inputs for services/dictionary.py (M4.1 text-core): parsing, ranking, replacements,
the prompt block, spelling rules, the correction diff and every mutator.

A DictionaryTerm travels as the dict of its fields ({id, term, starred, heard, learned, new,
protected}). Settings travel as plain dicts keyed by the bare setting names.

    _parse(raw)                         the terms, as dicts
    load_dictionary(settings)           the terms, as dicts
    apply_replacements(text, terms)     terms as dicts
    prompt_block(terms)                 terms as dicts
    find_term(settings, text)           the term as a dict, or None
    spelling_rule(rule)                 [term, [heard...]] or None
    _correction(pasted, field)          [pasted word, new word] or None
    mutators                            {"result": ..., "settings": the settings after} or
                                        {"error": the DictionaryError's message}

A mutator runs on a deep copy of its settings. An id a mutator made with _new_id (random) is
written as "$new" in both the result and the settings: twelve lowercase hex digits that no
entry of the input had. Stable ids ("t" + eleven hex digits) are deterministic and kept.
"""

import copy
import hashlib
import re
from dataclasses import asdict

_KEY = "dictation_dictionary"
_RULES = "transcript_cleanup_rules"
_FRESH = re.compile(r"[0-9a-f]{12}")


def _stable(term):
    return "t" + hashlib.sha1(term.casefold().encode("utf-8")).hexdigest()[:11]


def _term(fields):
    from services.dictionary import DictionaryTerm

    fields = dict(fields)
    fields["heard"] = tuple(fields.get("heard", ()))
    return DictionaryTerm(**fields)


def _plain(term):
    if term is None:
        return None
    return {**asdict(term), "heard": list(term.heard)}


def _input_ids(settings):
    raw = settings.get(_KEY) if isinstance(settings, dict) else None
    if not isinstance(raw, list):
        return set()
    return {item.get("id") for item in raw if isinstance(item, dict)}


def _mask(value, known):
    if isinstance(value, dict):
        return {key: (_mask_id(item, known) if key == "id" else _mask(item, known))
                for key, item in value.items()}
    if isinstance(value, list):
        return [_mask(item, known) for item in value]
    return value


def _mask_id(value, known):
    if isinstance(value, str) and value not in known and _FRESH.fullmatch(value):
        return "$new"
    return value


def _mutator(name, convert):
    def run(module, settings, *args, **kwargs):
        from services.dictionary import DictionaryError

        known = _input_ids(settings)
        changed = copy.deepcopy(settings)
        try:
            result = getattr(module, name)(changed, *args, **kwargs)
        except DictionaryError as exc:
            return {"error": str(exc)}
        return _mask({"result": convert(result), "settings": changed}, known)

    return run


def _keep(value):
    return value


def _add_term(module, settings, term, heard=(), starred=False):
    return _mutator("add_term", _plain)(module, settings, term, heard=heard, starred=starred)


def _update_term(module, settings, term_id, term=None, heard=()):
    return _mutator("update_term", _plain)(module, settings, term_id, term=term, heard=heard)


def _add_learned(module, settings, word, protected=False):
    return _mutator("add_learned", _plain)(module, settings, word, protected=protected)


WRAPPERS = {
    "_parse": lambda module, raw: [_plain(term) for term in module._parse(raw)],
    "load_dictionary": lambda module, settings: [
        _plain(term) for term in module.load_dictionary(settings)],
    "apply_replacements": lambda module, text, terms: module.apply_replacements(
        text, [_term(fields) for fields in terms]),
    "prompt_block": lambda module, terms: module.prompt_block(
        [_term(fields) for fields in terms]),
    "find_term": lambda module, settings, text: _plain(module.find_term(settings, text)),
    "spelling_rule": lambda module, rule: (
        None if (found := module.spelling_rule(rule)) is None
        else [found[0], list(found[1])]),
    "_correction": lambda module, pasted, field: (
        None if (found := module._correction(pasted, field)) is None else list(found)),
    "add_term": _add_term,
    "update_term": _update_term,
    "set_starred": _mutator("set_starred", _plain),
    "delete_term": _mutator("delete_term", _plain),
    "undo_learned": _mutator("undo_learned", _plain),
    "clear_new": _mutator("clear_new", _keep),
    "add_learned": _add_learned,
    "move_spelling_rules": _mutator("move_spelling_rules", _keep),
}


def entry(term, **fields):
    return {"id": fields.pop("id", term.lower()), "term": term, **fields}


def terms(*entries):
    """Term dicts as _parse would make them from `entries`."""
    out = []
    for item in entries:
        out.append({"id": item.get("id", item["term"].lower()), "term": item["term"],
                    "starred": item.get("starred", False), "heard": item.get("heard", []),
                    "learned": item.get("learned", False), "new": item.get("new", False),
                    "protected": item.get("protected", False)})
    return out


_MALFORMED = [
    None, "Kubernetes", 7, {}, [],
    ["loose string", None, 7, {"term": ""}, {"term": "   "}, {"term": 5},
     {"term": "x" * 65}, entry("Ksenia", starred="yes", learned=1, heard="Sonia")],
    [entry("OpenWhisper", starred=True), entry("openwhisper", id="b"), entry("Kubernetes")],
    [{"term": "  Alex   Rivera "}, {"term": "Ksenia", "id": ""}, {"term": "Olu", "id": "  "}],
    [entry("One", id="same"), entry("Two", id="same"), entry("Three", id="same")],
    [entry("One", id=" padded "), entry("Two", id=7), entry("Three", id=None)],
    [entry("Word0", heard=["a1", "a2", "A1", "Word0", "a3", "a4", "a5", "a6", "x" * 65])],
    [entry("Word0", heard=["  spaced   out ", "", None, 3, "word0", "x" * 64])],
    [entry("Ksenia", learned=True, new=True, heard=["sonia"], protected=True),
     entry("Kubernetes", new=True, protected="yes")],
    [entry("Straße"), entry("STRASSE"), entry("İstanbul"), entry("i̇stanbul")],
    [entry("x" * 64), entry("y" * 65), entry("ü" * 64)],
    [entry(f"Word{i}") for i in range(505)],
    # The id-less "Same" derives the id "Other" already holds, so it is derived again.
    [{"term": "Other", "id": _stable("same")}, {"term": "Same"}, {"term": "other"}],
    [{"term": "A", "id": _stable("b")}, {"term": "B", "id": _stable(_stable("b") + "B")},
     {"term": "b"}, {"term": "C", "id": _stable("b")}],
    [{"term": "Alpha"}, {"term": "Bravo", "id": "t" + "0" * 11}, {"term": "alpha "}],
]

_TERMS = terms(entry("Alpha"), entry("Bravo", starred=True), entry("Charlie", protected=True),
               entry("Delta", starred=True, learned=True, protected=True))

_SPELLING = [
    'Always spell my name "Alex Rivera"',
    "Always spell the name “Ksenia” exactly as written.",
    'Spell "jon" as "John".',
    'Spell it "Ksenia", not "Sonia"',
    "Use British spelling.",
    'Write "Kubernetes" in full.',
    'Spell "k8s" as "Kubernetes" except in code blocks.',
    'Spell "a", "b" and "c" carefully.',
    'Always spell "' + "x" * 70 + '"',
    'Always spell "' + "x" * 64 + '"',
    'Always spell "' + "x" * 65 + '"',
    'Spelled "Olu" right, not as "Oloo"',
    'SPELLING: "Zoë"',
    'spells "Zoë" as "Zoe"',
    'spellcheck "Zoë"',
    'Spell "   " please',
    'Spell "  Alex   Rivera  "',
    'Spell "a" not "b" as "c"',
    'Spell “Ksenia" not "Sonia”',
    "Spell 'Ksenia' like that",
    'Spell "Ksenia"' + " " * 103,
    'Spell "Ksenia"' + " " * 104,
    'Spell "Ksenia", but only in emails',
    'Spell "Ksenia" whenever possible',
    'Spell "Ksenia" ifonly',
    'Spell_ "Ksenia"',
    "",
]

_PASTED = "Let's meet with Sonia about the launch."
_FIELDS = [
    "Dear team, Let's meet with Ksenia about the launch.",
    "let's meet with KSENIA about the launch. See you then",
    "Let's meet with Ksenia about the launch",
    "Let's meet with Sonia about the Kickoff.",
    "Let's meet with Sonia about the launch.",
    "Let's meet with sonia about the launch.",
    "Let's meet with Ksenia Petrova about the launch.",
    "Let's meet with Ksenia about the big launch.",
    "Let's meet with Ksenia and the launch.",
    "Let's meet with Monday about the launch.",
    "Let's meet with Al about the launch.",
    "Let's meet with S0n1a_ about the launch.",
    "Let's meet with 2024 about the launch.",
    "Completely different text in the field.",
    "",
    "Let's meet with «Ksenia» about the launch.",
    "Let's meet with Ksenia’s about the launch.",
    "Let's meet with O’Neil about the launch.",
    "Let's meet with Jean-Luc about the launch.",
    "Let's meet with jean@x about the launch.",
    "Let's meet with Ölüdeniz about the launch.",
    "Let's meet with Sonia-x about the launch.",
    "Let's   meet\nwith\tKsenia about the launch.",
    "Let's meet with Thanks about the launch.",
    "Let's meet with Ksenia about the launch. Let's meet with Sonia about the launch.",
]

_SETTINGS_RULES = {
    _RULES: ["Use British spelling.", 'Spell "jon" as "John".', 'Always spell my name "Ksenia"'],
    _KEY: [entry("Ksenia")],
}

CASES = {
    "_parse": [[raw] for raw in _MALFORMED],
    "load_dictionary": [[{}], [{_KEY: "Kubernetes"}], [None], [{_KEY: _MALFORMED[5]}],
                        [{_KEY: _MALFORMED[6]}]],
    "apply_replacements": [
        ["Sonia and SENYA met Sonias", terms(entry("Ksenia", heard=["sonia", "senya"]))],
        ["ship cooper netties and cooper",
         terms(entry("Kubernetes", heard=["cooper", "cooper netties"]),
               entry("Netty", heard=["Kubernetes"]))],
        ["Sonia called", terms(entry("Ksenia", heard=["sonia"], learned=True))],
        ["", terms(entry("Ksenia", heard=["sonia"]))],
        ["john", terms(entry("Jon", heard=["john"]), entry("Jonn", heard=["john"], starred=True))],
        ["ksenya came", terms(entry("Ksenia", heard=["Ksenya"]))],
        ["İzmir office, izmir office, IZMİR office", terms(entry("Izmir HQ", heard=["izmir"]))],
        ["İstanbul ofis", terms(entry("Istanbul Ofis", heard=["İstanbul"]))],
        ["a b c d", terms(entry("B", heard=["a"]), entry("C", heard=["b"]),
                          entry("D", heard=["c"]))],
        ["k eight s and k8s", terms(entry("Kubernetes", heard=["k eight s", "K8S"]))],
        ["no terms", []],
    ],
    "recognition_phrases": [
        [{_KEY: [entry("Alpha"), entry("Bravo", starred=True), entry("Charlie"),
                 entry("Delta", starred=True, learned=True)]}],
        [{_KEY: [entry(f"Word{i}") for i in range(55)] + [entry("Late", starred=True)]}],
        [{_KEY: [entry("Alpha")], "dictionary_steer_recognition": False}],
        [{_KEY: [entry("Alpha")], "dictionary_steer_recognition": "no"}],
        [{}],
        [None],
    ],
    "prompt_block": [
        [terms(entry("Alpha"), entry("Bravo", starred=True))],
        [[]],
        [_TERMS],
        [terms(*[entry(f"Word{i}") for i in range(205)])],
        [terms(*[entry("w" * 60 + f"{i:04d}") for i in range(60)])],
        [terms(*[entry(f"P{i}", protected=True) for i in range(60)])],
        [terms(*[entry("w" * 60 + f"{i:04d}") for i in range(60)],
               entry("Kept", protected=True))],
        [terms(entry("Zoë"), entry("ü" * 63))],
    ],
    "summary": [
        [{}], [{_KEY: [entry("A")]}], [{_KEY: [entry("A"), entry("B", learned=True, new=True)]}],
        [{_KEY: [entry("A", learned=True, new=True), entry("B", learned=True, new=True)]}],
        [{_KEY: [entry("A", new=True)]}], [None],
    ],
    "parse_heard": [
        [" sonia, senya ;  ksen ya,, "], [""], ["one\ntwo\r\nthree"], ["a;;b,,c"],
        ["  　 "], ["tab\tsep, x"],
    ],
    "spelling_rule": [[rule] for rule in _SPELLING],
    "spelling_rules": [
        [_SETTINGS_RULES], [{}], [{_RULES: "not a list"}],
        [{_RULES: ['  Spell "Ann"  ', 7, "", "   ", 'Spell "Bo"']}],
        [{_RULES: [f'Spell "W{i}"' for i in range(55)]}],
    ],
    "move_spelling_rules": [
        [_SETTINGS_RULES],
        [{_RULES: ["Use British spelling."]}],
        [{_RULES: ['Spell "Ksenia"'], _KEY: [entry("Ksenia")]}],
        [{_RULES: ['Spell "jon" as "John"'],
          _KEY: [entry(f"Word{i}") for i in range(500)]}],
        [{_RULES: ['Spell "Word3" as "w three"'],
          _KEY: [entry(f"Word{i}") for i in range(500)]}],
        [{_RULES: ['Spell "a" as "b"', 'Spell "b" as "a"']}],
    ],
    "_correction": [[_PASTED, field] for field in _FIELDS] + [
        ["Call Sonia", "Call Ksenia"], ["Call Sonia now", "Call Ksenia now"],
        ["we dont ship today", "we don't ship today"],
        ["the eshop is open", "the e-shop is open"],
        ["", "anything"], ["one two", "one two"],
        ["a b c d e", "x a b Zed d e y"],
        ["a b c d e", "a b Zed Zed d e"],
    ],
    "learned_correction": [[_PASTED, field] for field in _FIELDS[:6]],
    "reverted_cleanup": [
        ["lets meet with Ksenia about the launch", "Sonia", "Ksenia"],
        ["lets meet with ksenia about the launch", "Sonia", "KSENIA"],
        ["lets meet with Ksenia about the launch", "Ksenia", "Sonia"],
        ["lets meet with Sonia about the launch", "Sonia", "Ksenia"],
        [None, "Sonia", "Ksenia"],
        ["", "Sonia", "Ksenia"],
        ["meet «Ksenia»!", "Sonia", "ksenia"],
        ["Straße", "x", "STRASSE"],
    ],
    "find_term": [
        [{_KEY: [entry("Ksenia"), entry("Alex Rivera")]}, "ksenia"],
        [{_KEY: [entry("Ksenia"), entry("Alex Rivera")]}, "  alex   RIVERA "],
        [{_KEY: [entry("Ksenia")]}, "Sonia"],
        [{}, "Ksenia"],
    ],
    "add_term": [
        [{}, "Kubernetes", ["cooper netties"]],
        [{_KEY: [entry("Kubernetes", heard=["cooper netties"])]}, "  Ksenia ", [], True],
        [{}, ""],
        [{}, "x" * 65],
        [{}, "Word", ["y" * 65]],
        [{}, "Word", ["a", "b", "c", "d", "e", "f"]],
        [{}, "Word", ["a", "b", "c", "d", "e", "word", "A"]],
        [{_KEY: [entry("ksenia", learned=True, new=True)]}, "Ksenia", ["sonia"]],
        [{_KEY: [entry("Ksenia", heard=["sonia"])]}, "Ksenia", ["Sonia"]],
        [{_KEY: [entry("Ksenia", heard=["a", "b", "c"])]}, "Ksenia", ["d", "e", "f"]],
        [{_KEY: [entry("Ksenia")]}, "Ksenia", [], True],
        [{_KEY: [entry("Ksenia", starred=True)]}, "Ksenia", [], False],
        [{_KEY: [entry(f"Word{i}") for i in range(500)]}, "Two"],
        [{_KEY: [entry(f"Word{i}") for i in range(500)]}, "word3", ["w three"]],
        [{_KEY: [entry("A"), entry("B"), entry("C")]}, "b", [], True],
    ],
    "update_term": [
        [{_KEY: [entry("Kubernetes"), entry("Ksenia")]}, "kubernetes", "ksenia", []],
        [{_KEY: [entry("Kubernetes"), entry("Ksenia")]}, "kubernetes", "K8s", ["kates"]],
        [{_KEY: [entry("Olu", learned=True, new=True)]}, "olu", "Olúwasẹ́un", ["olu"]],
        [{_KEY: [entry("Olu")]}, "missing", "X", []],
        [{_KEY: [entry("Olu")]}, "olu", "", []],
        [{_KEY: [entry("Olu")]}, "olu", "OLU", ["  o   lu "]],
    ],
    "set_starred": [
        [{_KEY: [entry("Ksenia", learned=True, new=True)]}, "ksenia", True],
        [{_KEY: [entry("Ksenia", starred=True)]}, "ksenia", False],
        [{_KEY: [entry("Ksenia")]}, "missing", True],
    ],
    "delete_term": [
        [{_KEY: [entry("Kubernetes"), entry("Ksenia")]}, "kubernetes"],
        [{_KEY: [entry("Ksenia")]}, "kubernetes"],
        [{}, "x"],
    ],
    "undo_learned": [
        [{_KEY: [entry("Ksenia", learned=True, new=True), entry("Typed")]}, "typed"],
        [{_KEY: [entry("Ksenia", learned=True, new=True), entry("Typed")]}, "ksenia"],
    ],
    "clear_new": [
        [{_KEY: [entry("A", learned=True, new=True), entry("B", learned=True, new=True),
                 entry("C")]}],
        [{_KEY: [entry("A", learned=True)]}],
        [{}],
    ],
    "add_learned": [
        [{}, "Ksenia"],
        [{_KEY: [entry("Ksenia", learned=True, new=True)]}, "ksenia"],
        [{"dictionary_learn_enabled": False}, "Olu"],
        [{}, "  Ksenia   Petrova "],
        [{}, ""],
        [{}, "x" * 65],
        [{_KEY: [entry(f"Word{i}") for i in range(500)]}, "Olu"],
        [{_KEY: [entry("Kubernetes")]}, "Ksenia", True],
        [{_KEY: [entry("Ksenia")]}, "ksenia", True],
        [{_KEY: [entry("Ksenia", protected=True)]}, "ksenia", True],
    ],
}

TRACE = {
    "tests": [
        "tests/test_personalize_s4_dictionary.py",
        "tests/test_personalize_s4_learning.py",
        "tests/test_dictionary_cleanup_revert.py",
    ],
    "functions": [
        "recognition_phrases", "summary", "parse_heard", "spelling_rule", "spelling_rules",
        "learned_correction", "reverted_cleanup", "_correction",
    ],
}
