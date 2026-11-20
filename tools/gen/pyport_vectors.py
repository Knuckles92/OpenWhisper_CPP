"""Write tests/golden/builtins.json: golden vectors of the Python builtins fv_pyport ports.

    & $env:FIORIVOCE_OW_PYTHON -I -B tools/gen/pyport_vectors.py

Run it with OpenWhisper's Python 3.12. Only the standard library is imported: the vectors are of
round, format, repr, int, float, str methods, re, json, datetime, difflib and html, not of
OpenWhisper code. The format is the parity harness's (a33): {"module", "source_commit", "pins",
"functions": {name: [{"args": [...], "result": ...} | {"args": [...], "raises": "<type>"}]}},
floats written with repr, NaN and infinities tagged {"$float": "nan"|"inf"|"-inf"}, integers
outside int64 tagged {"$int": "<digits>"}, a string holding a lone surrogate tagged
{"$code_points": [...]}. A raising case also records "message" where the
message is part of the behaviour (json, datetime, re.sub; re.error's without its position). source_commit is the OpenWhisper commit of
FIORIVOCE_OW_ROOT, or of the checkout holding the interpreter's virtual environment.
"""

from __future__ import annotations

import datetime
import difflib
import html
import json
import math
import os
import re
import subprocess
import sys
import unicodedata
import warnings
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "tests" / "golden" / "builtins.json"
INT64 = (-(2**63), 2**63 - 1)


def encode(value):
    if isinstance(value, str) and any(0xD800 <= ord(c) <= 0xDFFF for c in value):
        # UTF-8 cannot hold a lone surrogate, so the code points are listed.
        return {"$code_points": [ord(c) for c in value]}
    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, int):
        return value if INT64[0] <= value <= INT64[1] else {"$int": str(value)}
    if isinstance(value, float):
        if math.isnan(value):
            return {"$float": "nan"}
        if math.isinf(value):
            return {"$float": "inf" if value > 0 else "-inf"}
        return value
    if isinstance(value, (list, tuple)):
        return [encode(v) for v in value]
    if isinstance(value, dict):
        return {str(k): encode(v) for k, v in value.items()}
    return {"$repr": repr(value)}


def cases(fn, arg_lists, *, message=False):
    out = []
    for args in arg_lists:
        case = {"args": encode(list(args))}
        try:
            case["result"] = encode(fn(*args))
        except Exception as exc:  # the exception type is part of the contract
            case["raises"] = type(exc).__name__
            if message:
                # re.error's str() appends " at position N", which pyport does not report.
                case["message"] = exc.msg if isinstance(exc, re.error) else str(exc)
        out.append(case)
    return out


# --- numbers -----------------------------------------------------------------------------------

TIES = [0.5, 1.5, 2.5, 62.5, 63.5, -0.5, -1.5, 2.675, 1e16 + 0.5, 0.125, 0.375, 1234.5, 0.05,
        0.15, 0.25, 1499.5, 0.49999999999999994, 4503599627370497.0, -2.5, 99.5]
FIXED_INPUTS = TIES + [0.0, -0.0, -0.04, 1e22, 5e-324, float("nan"), float("inf"),
                       float("-inf"), 2500 / 1000, 0.95, 9.995]
REPR_INPUTS = [1.0, 0.1, 1e16, 1e-5, 123456789012345678.0, 5e-324, -0.0, float("inf"),
               float("-inf"), float("nan"), 0.0, 1e15, 9999999999999998.0, 0.0001, 0.00012345,
               1e22, 1.5e300, -1e-7, 100.0, 2.675, 1 / 3, 1.7976931348623157e308, 2.0**53]

# tests/test_typed_settings.py JUNK, then the int()/float() edges.
JUNK = [None, True, False, 0, 1, -3, 7, 100, 3.7, "", " ", "yes", "no", "0", "12", "auto",
        "  EN ", "garbage", [], ["a"], [1, 2], {}, {"a": 1}, {"claude-code": "m"}]
NUMBER_EDGES = ["1_000", " 12 ", "\u0661\u0662", "+3", "3.7", 3.7, -3.7, True, 1e300,
                float("inf"), float("-inf"), float("nan"), "-0", "0012", "1__0", "_1", "1_",
                "+ 1", "\u00a012\u2003", "\uff11\uff12", "\x1c7\x1f", "9223372036854775807",
                "-9223372036854775808", "9223372036854775808", "99999999999999999999", "1e5",
                " -1.5e-3 ", "inf", "-Infinity", "nAn", "1_0.5_0", "1_.5", ".5", "5.", "0x10",
                "1e", "--1", "\u0663.\u0661\u0664", "1e400", "-1e-400", 2**63 - 1, -(2**63),
                "\u00b2", "\u2167",
                # Below U+007F nothing is transformed, so U+001C..U+001F never strip, even in
                # text that also holds non-ASCII whitespace.
                "\x1c7\u00a0", "\u00a07\x1c", "\x857\u00a0", "\x7f7\u00a0", "\u20287\u2029",
                "\x0b7\x0c\u00a0", "\u0661_\u0662"]


def numbers() -> dict:
    return {
        "round": cases(round, [[x] for x in TIES]),
        "round_ndigits": cases(round, [[x, n] for x in TIES for n in (1, 2)]
                               + [[1234.5, -1], [1250.0, -2], [1350.0, -2], [0.5, 0],
                                  [-0.4, 0], [9.995, 2], [99.95, 1], [5e-324, 323],
                                  [1.7976931348623157e308, -308], [123.456, 400],
                                  [123.456, -400], [float("inf"), 2], [float("nan"), 2]]),
        "format": cases(format, [[x, spec] for x in FIXED_INPUTS
                                 for spec in (".0f", ".1f", ".2f")]),
        "repr_float": cases(repr, [[x] for x in REPR_INPUTS]),
        "int": cases(int, [[v] for v in JUNK + NUMBER_EDGES]),
        "float": cases(float, [[v] for v in JUNK + NUMBER_EDGES]),
    }


# --- str ---------------------------------------------------------------------------------------

WORDS = ["stra\u00dfe", "Stra\u00dfe", "\u01c5", "\u01c5ungla", "\u0130", "\u0130stanbul",
         "\ufb01", "\ufb00 \ufb01x", "\u03a3\u03c3\u03c2", "\u0391\u03a3", "\u0391\u03a3 \u0392",
         "\u0391\u03a3.", "\u03a3\u0391\u03a3", "a\u03a3b", "'\u03a3'", "\u0391'\u03a3",
         "\u00a0x\u2003", "\U0001F600", "a\U0001F600b", "x\u200by", "", " ", "Hello World",
         "hello wORLD", "\u0149", "abc123", "123abc", "\u0661\u0662\u0663", "\u00b2", "\u2167",
         "\x1f", "Caf\u00e9", "e\u0301", "\t\n\x0b\x0c\r\x1c\x1d\x1e\x1f\x85\u2028",
         "  padded  ", "_name", "na-me", "\u00df\u1e9e", "\u0345", "\u1f80", "ABC", "aBC",
         "Title Case", "\u00c9cole \u00e9t\u00e9", "\u4e2d\u6587", "\u0130I\u0131i"]
STR_METHODS = ["casefold", "lower", "upper", "title", "capitalize", "strip", "split", "isspace",
               "isalnum", "isalpha", "isdigit", "isdecimal", "isnumeric", "isupper", "islower",
               "istitle", "isprintable", "isidentifier"]


def strings() -> dict:
    out = {name: cases(lambda s, n=name: getattr(s, n)(), [[w] for w in WORDS])
           for name in STR_METHODS}
    out["len"] = cases(len, [[w] for w in WORDS] + [["\u00e9" * 200]])
    out["repr"] = cases(repr, [[w] for w in WORDS] + [["it's"], ["say \"hi\""],
                                                      ["both ' and \""], ["back\\slash"],
                                                      ["\x7f\x80\u00ad\0378"]])
    out["split_maxsplit"] = cases(lambda s, n: s.split(None, n),
                                  [["  a b  c  ", 0], ["  a b  c  ", 1], ["a\u2003b c", 1],
                                   ["  a b  c  ", -1], ["", 1]])
    out["split_sep"] = cases(lambda s, sep, n: s.split(sep, n),
                             [["a,b,,c", ",", -1], ["a,b,,c", ",", 1], [",", ",", -1],
                              ["a::b::", "::", -1], ["abc", "", -1], ["", ",", -1]])
    out["strip_chars"] = cases(lambda s, chars: s.strip(chars),
                               [["xxhixx", "x"], ["\U0001F600a\U0001F600", "\U0001F600"],
                                ["abcba", "ab"], ["", "a"]])
    out["slice"] = cases(lambda s, a, b: s[a:b],
                         [["a\U0001F600b\u00e9c", 1, 3], ["a\U0001F600b\u00e9c", -2, None],
                          ["a\U0001F600b\u00e9c", None, -1], ["abc", 5, 9], ["abc", 2, 1],
                          ["\u00e9" * 5, -100, 100]])
    out["html.escape"] = cases(html.escape, [["<a href=\"x\">'&'</a>"], ["plain"],
                                             ["<a href=\"x\">'&'</a>", False],
                                             ["&amp; \u00e9"]])
    out["unicodedata.category"] = cases(
        unicodedata.category,
        [[c] for c in ["A", "a", "\u01c5", "\u02b0", "\u4e2d", "\u0301", "\u0903", "\u20dd",
                       "5", "\u2167", "\u00bd", "_", "-", "(", ")", "\u00ab", "\u00bb", "!", "+",
                       "$", "^", "\u00a9", " ", "\u2028", "\u2029", "\x00", "\u00ad", "\ue000",
                       "\U0001F600", "\U0001FAE8", "\U00031350", "\u0378"]])
    return out


# --- re ----------------------------------------------------------------------------------------

FONT = r"(font-size:\s*)(\d+(?:\.\d+)?)(px|pt)"
TOKEN = r"@([a-z][a-z0-9]*(?:-[a-z0-9]+)*)"
TEXTS = ["Whisper large-v3", "nvidia/parakeet-tdt-0.6b-v3", "caf\u00e9 cr\u00e8me br\u00fbl\u00e9e",
         "cafe\u0301 au lait", "\u65e5\u672c\u8a9e\u306e\u30c6\u30ad\u30b9\u30c8 \u4e2d\u6587",
         "\u0661\u0662_x \u00b2 \u2167\u00bd", "", "  ", "a\u200bb\u2060c\u00b7d",
         "\U0001F600x\U0001F600"]
CSS = ["QLabel { font-size: 13px; } QPushButton{font-size:9.5PT}",
       "font-size:\u00a012px; FONT-SIZE: 14pt", "font-size: \u0661\u0662px",
       "@bg-primary and @text_2 @accent-blue-2 @9x foo@bar caf\u00e9@ok",
       "@\u00e9l\u00e8ve @a-b-"]


def regexes() -> dict:
    def sub(pattern, flags, repl, text, count=0):
        return re.sub(pattern, repl, text, count=count, flags=flags)

    def findall(pattern, flags, text):
        return re.findall(pattern, text, flags=flags)

    def split(pattern, flags, text, maxsplit=0):
        return re.split(pattern, text, maxsplit=maxsplit, flags=flags)

    def span(pattern, flags, text):
        m = re.search(pattern, text, flags=flags)
        return None if m is None else [m.start(), m.end(), m.group(0)]

    ignore = int(re.IGNORECASE)
    multi = int(re.MULTILINE)
    verbose = int(re.VERBOSE)
    dotall = int(re.DOTALL)
    sub_args = ([[r"[^\w]+", 0, " ", t.casefold()] for t in TEXTS]
                + [[FONT, ignore, r"\g<1>X\3", t] for t in CSS]
                + [[TOKEN, 0, r"<\1>", t] for t in CSS]
                + [["x*", 0, "-", "abxd"], ["", 0, "-", "abc"], [r"\b", 0, "|", "ab cd"],
                   [r"\B", 0, "|", "ab cd"], [r"\B", 0, "|", ""], [r"\s+", 0, "_", "a\x1cb\x85c"],
                   [r"\d", 0, "#", "1\u0661\u00b2\u2167"], [r"(a)|b", 0, r"[\1]", "ab"],
                   [r"(?P<w>\w+)", 0, r"\g<w>\g<0>", "hi there"], ["a", 0, r"\n\t\\\&", "a"],
                   ["a", 0, r"\q", "a"], ["a", 0, r"\2", "a"], ["(a)", 0, r"\g<x>", "a"],
                   ["(a)", 0, r"\g<1a>", "a"], ["a", 0, "\\", "a"], [r"(a)", 0, r"\101", "a"],
                   [r"\w+$", multi, "!", "one\ntwo\n"], ["^", multi, ">", "a\nb\n"],
                   [r"\Z", 0, "!", "a\n"], ["$", 0, "!", "a\n"], [r"\u00e9", 0, "e", "caf\u00e9"],
                   [r"\x41", ignore, "b", "aA"], ["\u00c9", ignore, "e", "\u00e9\u00c9"],
                   ["stra\u00dfe", ignore, "x", "STRASSE stra\u00dfe STRA\u1e9eE"],
                   ["k", ignore, "x", "k\u212aK"], [".", 0, "x", "a\nb"], [".", dotall, "x", "a\nb"],
                   [r"a # comment" "\n" r" b [ #]", verbose, "x", "ab #ab "],
                   [r"[\w-]+", 0, "x", "a-b c\u0301d"], [r"[^\W\d_]+", 0, "x", "ab12_cd \u00e9"],
                   [r"(?<!\w)x(?!\w)", 0, "y", "x ax x\u0301 x"], [r"[]a]", 0, "x", "]a"],
                   [r"[[:alpha:]]", 0, "x", "[:a]"], [r"\v", 0, "x", "\x0b\n"],
                   [r"(?i)AB", 0, "x", "ab"], ["(?x) a b ", 0, "x", "ab"],
                   # IGNORECASE does not extend \w and \W to case partners (U+0345 folds to
                   # iota): sre tests each character's own category.
                   [r"\w", ignore, "x", "ͅιιΙ"],
                   [r"\W", ignore, "x", "ͅⒶι"],
                   # Comments, and in verbose mode whitespace, may precede the global flags.
                   ["(?x) # lead\n (?i)A", 0, "x", "a"], ["(?#c)(?i)a", 0, "x", "A"],
                   [r"a(?#[\d)b", 0, "x", "ab"],
                   # PCRE2 syntax Python refuses.
                   ["a(?i)b", 0, "x", "ab"], ["(a|(?i)b)", 0, "x", "ab"], ["(?<n>a)", 0, "x", "a"],
                   ["(?P>n)", 0, "x", "a"], ["(?C1)a", 0, "x", "a"], ["(*FAIL)", 0, "x", "a"],
                   ["(?|a)", 0, "x", "a"], ["(?R)", 0, "x", "a"], ["(?1)(a)", 0, "x", "a"],
                   ["(?'n'a)", 0, "x", "a"], ["(?-i)a", 0, "x", "a"], ["(?i-s)a", 0, "x", "a"],
                   ["(?(?=a)a|b)", 0, "x", "a"], ["(?#unterminated", 0, "x", "a"],
                   [r"(a)(?(1)b|c)", 0, "x", "ab ac"], ["(?i:A)(?-i:b)", 0, "x", "aB ab"]])
    return {
        "re.sub": cases(sub, sub_args, message=True),
        "re.sub_count": cases(sub, [["a", 0, "b", "aaaa", 2], [r"\w", 0, "", "abc", 1]]),
        "re.findall": cases(findall, [[r"[^\w]+", 0, t] for t in TEXTS]
                            + [[FONT, ignore, t] for t in CSS] + [[TOKEN, 0, t] for t in CSS]
                            + [[r"\w+", 0, t] for t in TEXTS]
                            + [["x*", 0, "abxd"], [r"(a)(b)?", 0, "aab"], ["(?m)^", 0, "a\n"],
                               [r"\b\w", 0, "\u00e9t\u00e9 x\u0301y"]]),
        "re.split": cases(split, [[r"[^\w]+", 0, t.casefold()] for t in TEXTS]
                          + [[r"(-)|(_)", 0, "a-b_c"], [r"\b", 0, "ab cd"], ["x*", 0, "axbc"],
                             [r",", 0, "a,b,c", 1], [FONT, ignore, CSS[0]]]),
        "re.search_span": cases(span, [[r"\w+", 0, "\U0001F600 \u00e9t\u00e9"],
                                       [r"(?<=\U0001F600)x", 0, "\U0001F600x"],
                                       [r"\d+", 0, "abc \u0663\u0664"], ["z", 0, "abc"]]),
    }


# --- json --------------------------------------------------------------------------------------

DOCUMENT = {"b": 1, "a": [1, 2.5, -0.0, 1e16, 1e-7, True, False, None],
            "nested": {"empty_list": [], "empty_dict": {}, "deep": [[[]], [{}]]},
            "text": "caf\u00e9 \U0001F600 \"quoted\" \\ /\n\t\x7f\x01\u2028",
            "\u00e9": "key", "Z": 0}
DUMPS_OPTIONS = [{}, {"indent": 2}, {"indent": 0}, {"separators": [",", ":"]},
                 {"ensure_ascii": False}, {"sort_keys": True}, {"indent": 2, "sort_keys": True},
                 {"indent": 4, "ensure_ascii": False, "separators": [", ", ": "]}]
LOADS = ["NaN", "Infinity", "-Infinity", "[NaN, Infinity, -Infinity]",
         '{"a": 1, "b": 2, "a": 3}', "[1, 2.0, -0, -0.0, 1e5, 1E-2, 0.5e+1]",
         '"\\ud83d\\ude00"', '"\\u00e9\\/\\b\\f\\n\\r\\t"', '"\\ud83d"', '"\\ud83dx"',
         '"\\ud83d\\u0041"', " \t\n[ ]\r\n", "{}", '{"a": {"b": [null, true, false]}}',
         "18446744073709551615", "-9223372036854775808", "1.7976931348623157e308",
         "1e400", "-1e-400", '"\u00e9\U0001F600"',
         "", " ", "[1,]", '{"a":1,}', "{'a':1}", "[1 2]", '{"a" 1}', '{"a":1 "b":2}', '"abc',
         '"a\\qb"', '"\\u12"', '"\\u12x4"', '"\\ud83d\\u12x4"', '"a\nb"', "nul", "01", "1.",
         "-", "[1] x", "\ufeff[]", "\n\n  [1,\n 2,]", "tru", "[", "{", '{"a":', "1e", "-a",
         "[-]", '["\u00e9", x]', "NaNx", "-Inf"]


def json_cases() -> dict:
    def dumps(value, options):
        kwargs = dict(options)
        if "separators" in kwargs:
            kwargs["separators"] = tuple(kwargs["separators"])
        return json.dumps(value, **kwargs)

    def loads_depth(n, kind):
        text = ("[" * n + "]" * n) if kind == "array" else ('{"a":' * n + "1" + "}" * n)
        value = json.loads(text)
        depth = 0
        while isinstance(value, (list, dict)) and value:
            value = value[0] if isinstance(value, list) else value["a"]
            depth += 1
        return depth + (1 if kind == "array" else 0)

    values = [DOCUMENT, [], {}, "\U0001F600", 1.0, -0.0, float("nan"), float("inf"),
              [float("-inf")], 12345678901234567890, "\x00\x1f", [[1, [2, {"k": []}]]]]
    return {
        "json.dumps": cases(dumps, [[v, o] for v in values for o in DUMPS_OPTIONS]),
        "json.loads": cases(json.loads, [[t] for t in LOADS], message=True),
        "json.loads_depth": cases(loads_depth, [[1000, "array"], [1100, "array"],
                                                [2997, "array"], [2998, "array"],
                                                [2997, "object"], [2998, "object"]],
                                  message=True),
        "py_equal": cases(lambda a, b: a == b,
                          [[1, 1.0], [1, True], [0, False], [[1, 2], [1.0, 2]],
                           [{"a": 1, "b": 2}, {"b": 2, "a": 1}], ["1", 1], [None, False],
                           [9007199254740993, 9007199254740992.0], [float("nan"), float("nan")],
                           [1e300, 1e300], [{"a": [1]}, {"a": [True]}], [[], {}], [None, None],
                           [2**63 - 1, 9223372036854775807.0], [-0.0, 0], ["a", "a"],
                           [{"a": 1}, {"a": 1, "b": 2}]]),
    }


# --- datetime ----------------------------------------------------------------------------------

DATETIMES = ["2026-11-14", "2026-11-14T10:20:30", "2026-11-14T10:20:30.5",
             "2026-11-14T10:20:30.123456+00:00", "2026-11-14T10:20:30Z", "20261114T102030",
             "bad", "2026-11-14 10:20", "2026-11-14T10", "2026-11-14T1020",
             "2026-11-14T10:20:30.1234567", "2026-11-14T10:20:30,5", "2026-11-14T10:20:30+05:30",
             "2026-11-14T10:20:30-0800", "2026-11-14T10:20:30+05",
             "2026-11-14T10:20:30.5-03:30:15", "2026-11-14T10:20:30+05:30:15.25",
             "2026-11-14T10:20:30+24:00", "2026-11-14T10:20:30-25:00",
             "2026-11-14T10:20:30+00:00:00.5", "2026-W46-6", "2026W466T08", "2026-W53",
             "2020-W53-7T12:00", "2026-13-01", "2026-02-30", "2024-02-29T23:59:59",
             "2026-11-14T24:00:00", "2026-11-14T10:20:30:40", "2026-11-14T10.5", "2026-11-14T",
             "2026-11-1410:20", "2026-11-14X10:20", "2026-11-14\u00e910:20",
             "2026-11-14\U0001F60010:20", "2026-11-14T10:20:30Zx", "0000-01-01", "2026-1-14",
             "2026-11-14T10:60", "2026-11-14T25:00", "", "2026-11-14T10:20:30.",
             "2026-11-14T10:20:30.12a", "2026-11-14T10:20:30+5", "2026-11-14T10:20Z",
             "2026-11-14T102030.25", "2026-11-14T10:2030", "2026-11-14T1020:30",
             "\uff12\uff10\uff12\uff16-11-14", "2026-11-14T10:20:30 ", "20261114", "2026W46",
             "2026-W46-6T10:20", "2026-W46T10"]
DATES = ["2026-11-14", "20261114", "2026-W46", "2026-W46-6", "2026W46", "2026W466",
         "2026-11-14T00", "2026-02-29", "2024-02-29", "20261114ab", "2026-W53-1", "2020-W53-7",
         "2026-11-1", "2026/11/14", "", "0000-01-01", "2026-W00", "2026-W46-8", "2026-W4606"]


def datetime_cases() -> dict:
    def fromiso(text):
        return datetime.datetime.fromisoformat(text).isoformat()

    def iso(text, sep, timespec):
        return datetime.datetime.fromisoformat(text).isoformat(sep=sep, timespec=timespec)

    return {
        "datetime.fromisoformat": cases(fromiso, [[t] for t in DATETIMES], message=True),
        "datetime.isoformat": cases(iso, [[t, sep, spec]
                                          for t in ["2026-11-14T10:20:30.123456+05:30",
                                                    "2026-11-14T10:20:30"]
                                          for sep, spec in [("T", "auto"), (" ", "hours"),
                                                            ("T", "minutes"), ("T", "seconds"),
                                                            ("T", "milliseconds"),
                                                            ("_", "microseconds"),
                                                            ("T", "nanoseconds")]],
                                    message=True),
        "date.fromisoformat": cases(lambda t: datetime.date.fromisoformat(t).isoformat(),
                                    [[t] for t in DATES], message=True),
    }


# --- difflib -----------------------------------------------------------------------------------

LONG_A = "ab" * 150 + "xyz" + "q" * 20
LONG_B = "ab" * 140 + "xyz" + "c" * 70 + "q" * 20
PAIRS = [["abcd", "bcde"], ["qabxcd", "abycdf"], ["", ""], ["", "abc"], ["abc", ""],
         ["stra\u00dfe", "strasse"], ["private Thread currentThread;",
                                      "private volatile Thread currentThread;"],
         ["kitten", "sitting"], ["\U0001F600ab", "ab\U0001F600"], [LONG_A, LONG_B],
         ["the quick brown fox jumps".split(), "the quick red fox jumped over".split()],
         ["a b c a b c".split(), "c b a".split()],
         [("w " * 120 + "end").split(), ("w " * 100 + "x end").split()]]


def difflib_cases() -> dict:
    def ratio(a, b, autojunk):
        return difflib.SequenceMatcher(None, a, b, autojunk=autojunk).ratio()

    def opcodes(a, b, autojunk):
        return [list(op) for op in
                difflib.SequenceMatcher(None, a, b, autojunk=autojunk).get_opcodes()]

    def blocks(a, b, autojunk):
        return [list(m) for m in
                difflib.SequenceMatcher(None, a, b, autojunk=autojunk).get_matching_blocks()]

    def quick(a, b, autojunk):
        s = difflib.SequenceMatcher(None, a, b, autojunk=autojunk)
        return [s.quick_ratio(), s.real_quick_ratio()]

    args = [[a, b, junk] for a, b in PAIRS for junk in (True, False)]
    words = ["ape", "apple", "peach", "puppy", "appel", "applet", "Apple", "\u00e1pple"]
    return {
        "SequenceMatcher.ratio": cases(ratio, args),
        "SequenceMatcher.get_opcodes": cases(opcodes, args),
        "SequenceMatcher.get_matching_blocks": cases(blocks, args),
        "SequenceMatcher.quick_ratio": cases(quick, args),
        "get_close_matches": cases(difflib.get_close_matches,
                                   [["appel", words, 2, 0.5], ["appel", words, 3, 0.6],
                                    ["wheel", ["while", "whale", "wheels", "heel"], 3, 0.6],
                                    ["ab", ["ab", "ba", "ac", "ca", "ab"], 4, 0.0],
                                    ["parakeet", ["parakeet-v3", "parakeet-tdt", "whisper"], 2,
                                     0.5],
                                    ["x", ["y"], 0, 0.5], ["x", ["y"], 1, 1.5]],
                                   message=True),
    }


def source_commit() -> str:
    candidates = []
    if os.environ.get("FIORIVOCE_OW_ROOT"):
        candidates.append(Path(os.environ["FIORIVOCE_OW_ROOT"]))
    candidates.append(Path(sys.prefix).parent)
    for checkout in candidates:
        if (checkout / ".git").exists():
            try:
                return subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"],
                                      capture_output=True, text=True, check=True).stdout.strip()
            except (OSError, subprocess.CalledProcessError):
                pass
    return "unknown"


def main() -> int:
    if sys.version_info[:2] != (3, 12) or unicodedata.unidata_version != "15.0.0":
        print("run this with Python 3.12 (FIORIVOCE_OW_PYTHON)", file=sys.stderr)
        return 1
    # re warns about "[[" (a possible future set syntax); the vector records today's meaning.
    warnings.simplefilter("ignore", FutureWarning)
    functions = {}
    for group in (numbers, strings, regexes, json_cases, datetime_cases, difflib_cases):
        functions.update(group())
    record = {
        "module": "builtins",
        "source_commit": source_commit(),
        "pins": {"python": sys.version.split()[0], "platform": sys.platform,
                 "unidata_version": unicodedata.unidata_version},
        "functions": functions,
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes((json.dumps(record, indent=1, ensure_ascii=False) + "\n").encode("utf-8"))
    total = sum(len(c) for c in functions.values())
    print(f"wrote {OUTPUT.relative_to(ROOT)}: {total} cases over {len(functions)} functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
