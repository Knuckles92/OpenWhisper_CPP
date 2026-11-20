"""Curated inputs for services/titles.py (M1.13 history-core): the title contract.

Lengths count code points (an emoji outside the BMP is one), blank means str.strip() leaves
nothing (Unicode whitespace), and control characters are below U+0020 or U+007F only.
"""

_INPUTS = [
    [None], [5], [["a list"]], [""], ["   "], [" 　"], ["ok"], [" padded "],
    ["a" * 200], ["a" * 201], [" " + "a" * 199 + " "], ["é" * 200], ["é" * 201],
    ["\U0001F600" * 200], ["\U0001F600" * 201], ["tab\there"], ["line\nbreak"], ["del\x7f"],
    ["unit\x1fsep"], ["nbsp inside"], ["next line\u0085"], ["zero​width"],
    ["  \x01  "], ["Weekly sync — notes"],
]

CASES = {
    "title_error": _INPUTS,
    "normalize_title": _INPUTS,
}
