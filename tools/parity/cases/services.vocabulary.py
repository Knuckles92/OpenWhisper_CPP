"""Curated inputs for services/vocabulary.py (M4.1 text-core): the caseless fold with its origin
map, and whole-word replacement.

    _fold_with_origin(text)       [folded, origin as a list]
    replace_terms(text, rules)    rules travel as a list of [term, replacement] pairs in mapping
                                  order (a traced call's dict is passed as it is)

The random strings come from random.Random(_SEED) over an alphabet of the letters whose fold is
special (dotted and dotless i, sharp s, ligatures, final sigma, Kelvin sign, combining dots),
word separators and a spread of scripts; each one's rules are words taken from the string itself,
recased, so most of them match.
"""

import random

_DOT = "̇"
_SEED = 4101


def _fold_with_origin(module, text):
    folded, origin = module._fold_with_origin(text)
    return [folded, list(origin)]


def _replace_terms(module, text, rules):
    return module.replace_terms(text, dict(rules))


WRAPPERS = {"_fold_with_origin": _fold_with_origin, "replace_terms": _replace_terms}

_ALPHABET = (
    list("abcxyz iIİıßẞﬁﬃﬀΣσςÉéŉǰ’—.,-_'KkKſǅǄǆΐΰ")
    + [_DOT, _DOT, "İ", "́", " ", "　", "\n"]
    + list("ДжЖёЁ中文ア한ﬆ\U0001F600\U00010400\U00010428")
)


def _random_strings(count):
    rng = random.Random(_SEED)
    strings = []
    for _ in range(count):
        strings.append("".join(rng.choice(_ALPHABET) for _ in range(rng.randint(0, 24))))
    return strings, rng


def _random_rules(text, rng):
    words = [word for word in text.split() if word.strip(".,-_'’—")]
    rules = []
    for index in range(rng.randint(1, 3)):
        if words and rng.random() < 0.8:
            word = rng.choice(words).strip(".,—")
            word = rng.choice([word, word.upper(), word.lower(), word.casefold(), word.title()])
        else:
            word = "".join(rng.choice(_ALPHABET) for _ in range(rng.randint(1, 4)))
        if word not in [term for term, _replacement in rules]:
            rules.append([word, f"R{index}"])
    return rules


_STRINGS, _RNG = _random_strings(200)

_FOLD_TEXTS = [
    "", "plain ascii, with digits 123!", "".join(chr(code) for code in range(128)),
    "café — we’ll ship", "die Straße hier", "İİ İzmir", "i" + _DOT + "zmir",
    "I" + _DOT + _DOT + "x", "ﬁ" + _DOT + " a" + _DOT + _DOT, _DOT + "i", "ΑΣ ΣΑΣ σς",
    "İstanbul", "straße", "ﬁle", "Ǆ ǅ ǆ", "x" + _DOT + "i" + _DOT + "y" + _DOT,
    "ΐ ΰ ŉ ǰ", "K elvin", "ſoft", "\U00010400\U00010428",
]

CASES = {
    "_fold_with_origin": [[text] for text in _FOLD_TEXTS + _STRINGS[:40]],
    "replace_terms": [
        ["İstanbul office", [["istanbul", "Istanbul HQ"]]],
        ["istanbul office", [["İstanbul", "Istanbul HQ"]]],
        ["ISTANBUL İSTANBUL i̇stanbul", [["istanbul", "X"]]],
        ["die Straße hier", [["strasse", "Strasse"]]],
        ["die STRASSE hier", [["straße", "Straße"]]],
        ["Straße und koln.", [["koln", "Köln"]]],
        ["a ﬁle here", [["file", "File"]]],
        ["a ﬁle here", [["fi", "FI"]]],
        ["xﬁ here", [["x", "X"]]],
        ["Ǆ and ǆ and ǅ", [["ǆ", "dz"]]],
        ["an i" + _DOT + _DOT + "zmir trip", [["izmir", "Izmir"]]],
        ["x ﬁ" + _DOT + "x", [["fix", "FIX"]]],
        ["İZMİR", [["i" + _DOT + "zmir", "Izmir"]]],
        ["Izmirli", [["izmir", "X"]]],
        ["x", [["", "never"]]],
        ["", [["a", "b"]]],
        ["a b", []],
        ["ship cooper netties and cooper",
         [["cooper", "Kubernetes"], ["cooper netties", "Kubernetes"], ["Kubernetes", "Netty"]]],
        ["john and JOHN", [["John", "Jon"], ["john", "Jonn"]]],
        ["a b c", [["a", "b"], ["b", "c"]]],
        ["sonia's sonia_x sonia2 sonia.", [["sonia", "Ksenia"]]],
        ["ΣΑΣ σας", [["σας", "Sas"]]],
        ["Kelvin kelvin", [["kelvin", "K"]]],
        ["café café", [["cafe", "Café"]]],
        ["one two one two", [["one two", "1-2"]]],
        ["ﬀ ff", [["ff", "FF"]]],
        ["ﬀ ff", [["f", "F"]]],
    ] + [[text, _random_rules(text, _RNG)] for text in _STRINGS],
}

# Only this test: the file's call-counting test counts the trace plugin's own calls and fails
# under it.
TRACE = {
    "tests": [
        "tests/test_personalize_fix2_g4_fold_speed.py::test_replace_terms_folds_case_like_unicode",
    ],
    "functions": ["replace_terms"],
}

PINS = {"seed": _SEED}
