"""Write src/pyport/data/unicode_tables.inc: the Unicode tables of Python 3.12's str methods.

    python -I -B tools/gen/unicode_tables.py [--check]

Run it with Python 3.12 (FIORIVOCE_OW_PYTHON): every table is read back from the running
interpreter's own str methods and unicodedata, so the C++ side reproduces exactly what
OpenWhisper's Python does, including where it differs from newer Unicode versions. The
interpreter must report unicodedata.unidata_version 15.0.0; another version is refused.

Per code point the tables hold: the general category; the str.isalpha, isdecimal, isdigit,
isnumeric, isspace and isprintable results; the Lowercase, Uppercase, Lt, Cased and
Case_Ignorable properties CPython uses for islower, isupper, istitle and the final-sigma rule;
XID_Start and XID_Continue (str.isidentifier); the decimal digit runs (int() and float() accept
any Nd digit); and the full lower, upper, title and case-fold mappings (up to three code points).

--check compares with the committed file instead of writing it, and fails on a difference.
Only the standard library is used; nothing from the OpenWhisper checkout is imported.
"""

from __future__ import annotations

import argparse
import sys
import unicodedata
from pathlib import Path

EXPECTED_UNIDATA = "15.0.0"
ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "src" / "pyport" / "data" / "unicode_tables.inc"
MAX_CODE_POINT = 0x10FFFF

# Bits of the flags word; src/pyport/detail/unicode_data.h names the same values.
FLAGS = (
    "ALPHA", "DECIMAL", "DIGIT", "NUMERIC", "SPACE", "LOWER", "UPPER", "TITLE", "CASED",
    "CASE_IGNORABLE", "PRINTABLE", "XID_START", "XID_CONTINUE",
)
BIT = {name: 1 << index for index, name in enumerate(FLAGS)}

# unicodedata's general categories, in the order of the C++ enum.
CATEGORIES = (
    "Cn", "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps",
    "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co",
)

SIGMA = "Σ"
FINAL_SIGMA = "ς"


def final_sigma(text: str) -> bool:
    return text.lower()[-1] == FINAL_SIGMA


def properties(cp: int) -> int:
    ch = chr(cp)
    flags = 0
    for name, test in (("ALPHA", ch.isalpha), ("DECIMAL", ch.isdecimal),
                       ("DIGIT", ch.isdigit), ("NUMERIC", ch.isnumeric),
                       ("SPACE", ch.isspace), ("LOWER", ch.islower), ("UPPER", ch.isupper),
                       ("PRINTABLE", ch.isprintable)):
        if test():
            flags |= BIT[name]
    if unicodedata.category(ch) == "Lt":
        flags |= BIT["TITLE"]
    # Unicode's Cased is Lowercase or Uppercase or Lt; one-character islower and isupper are
    # exactly CPython's Lowercase and Uppercase flags.
    cased = bool(flags & (BIT["LOWER"] | BIT["UPPER"] | BIT["TITLE"]))
    if cased:
        flags |= BIT["CASED"]
    # Case_Ignorable is not exposed, so it is read back through the final-sigma rule of
    # str.lower: "c" + SIGMA is final exactly when c is cased and not case-ignorable, and
    # "A" + c + SIGMA is final exactly when c is case-ignorable or cased.
    if ch != SIGMA:
        alone = final_sigma(ch + SIGMA)
        after_cased = final_sigma("A" + ch + SIGMA)
        if alone and not cased:
            raise SystemExit(f"U+{cp:04X}: final sigma after a character that is not cased")
        ignorable = (not alone) if cased else after_cased
        if ignorable:
            flags |= BIT["CASE_IGNORABLE"]
    if ch.isidentifier():
        flags |= BIT["XID_START"]
    if ("a" + ch).isidentifier():
        flags |= BIT["XID_CONTINUE"]
    return flags


def mapping(cp: int, method: str) -> tuple[int, ...] | None:
    ch = chr(cp)
    mapped = getattr(ch, method)()
    if mapped == ch:
        return None
    if len(mapped) > 3:
        raise SystemExit(f"U+{cp:04X}.{method}() maps to more than three code points")
    return tuple(ord(m) for m in mapped)


def decimal_zeros() -> list[int]:
    """Code points of digit zero for every run of ten decimal digits; refuses a layout that is
    not such runs, which the C++ lookup (value = cp - zero) relies on."""
    zeros = []
    cp = 0
    while cp <= MAX_CODE_POINT:
        value = unicodedata.decimal(chr(cp), None)
        if value is None:
            cp += 1
            continue
        if value != 0:
            raise SystemExit(f"U+{cp:04X}: a decimal run starts at value {value}")
        for offset in range(10):
            if unicodedata.decimal(chr(cp + offset), None) != offset:
                raise SystemExit(f"U+{cp:04X}: the decimal run is not 0..9")
        zeros.append(cp)
        cp += 10
    for cp in range(MAX_CODE_POINT + 1):
        value = unicodedata.decimal(chr(cp), None)
        if value is not None and not any(z <= cp < z + 10 for z in zeros):
            raise SystemExit(f"U+{cp:04X}: a decimal digit outside the runs")
        if chr(cp).isdecimal() != (value is not None):
            raise SystemExit(f"U+{cp:04X}: isdecimal disagrees with unicodedata.decimal")
    return zeros


def hexcp(cp: int) -> str:
    return f"0x{cp:04X}"


def emit_rows(items: list[str], per_line: int) -> list[str]:
    lines = []
    for start in range(0, len(items), per_line):
        lines.append("    " + " ".join(items[start:start + per_line]))
    return lines


def generate() -> str:
    if unicodedata.unidata_version != EXPECTED_UNIDATA:
        raise SystemExit(f"unicodedata {unicodedata.unidata_version} is not "
                         f"{EXPECTED_UNIDATA}: run this with Python 3.12")

    records: dict[tuple[int, int], int] = {}
    runs: list[tuple[int, int]] = []
    previous = None
    for cp in range(MAX_CODE_POINT + 1):
        key = (properties(cp), CATEGORIES.index(unicodedata.category(chr(cp))))
        index = records.setdefault(key, len(records))
        if index != previous:
            runs.append((cp, index))
            previous = index

    out = [
        "// Generated by tools/gen/unicode_tables.py from Python 3.12's str methods and",
        f"// unicodedata {EXPECTED_UNIDATA}. Do not edit.",
        "// clang-format off",
        "",
        f'#define FV_PY_UNIDATA_VERSION "{EXPECTED_UNIDATA}"',
        "",
        "// {flags, general category} of each distinct property set.",
        f"inline constexpr PropertyRecord kRecords[{len(records)}] = {{",
    ]
    out += emit_rows([f"{{0x{flags:04X}, {category}}}," for (flags, category) in records], 6)
    out += ["};", "",
            "// {first code point, record}: each run lasts until the next one starts.",
            f"inline constexpr PropertyRun kRuns[{len(runs)}] = {{"]
    out += emit_rows([f"{{{hexcp(cp)}, {index}}}," for cp, index in runs], 5)
    out += ["};", ""]

    zeros = decimal_zeros()
    out += ["// Digit zero of each run of ten decimal digits (Nd).",
            f"inline constexpr char32_t kDecimalZeros[{len(zeros)}] = {{"]
    out += emit_rows([f"{hexcp(z)}," for z in zeros], 10)
    out += ["};", ""]

    for name, method in (("kLower", "lower"), ("kUpper", "upper"), ("kTitle", "title"),
                         ("kFold", "casefold")):
        entries = []
        for cp in range(MAX_CODE_POINT + 1):
            mapped = mapping(cp, method)
            if mapped is not None:
                padded = list(mapped) + [0] * (3 - len(mapped))
                entries.append("{" + hexcp(cp) + ", {" + ", ".join(hexcp(m) for m in padded)
                               + "}},")
        out += [f"// str.{method}() of one code point, where it is not the code point itself.",
                f"inline constexpr CaseMapping {name}[{len(entries)}] = {{"]
        out += emit_rows(entries, 2)
        out += ["};", ""]

    out += ["// clang-format on", ""]
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed file instead of writing it")
    args = parser.parse_args()
    text = generate()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date", file=sys.stderr)
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is up to date")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
