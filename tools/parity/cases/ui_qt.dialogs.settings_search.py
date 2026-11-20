"""Curated inputs for ui_qt/dialogs/settings_search.py (M2.2 appcore/settings_search.h).

match_entries runs over one fixed entry set (built here as SearchEntry objects without widget
targets) and returns the indexes of the entries it finds, in its order; the C++ replay builds the
same entries. The set has more matching Settings entries than the section cap, ties on
(score, title length) to pin the stable sort, titles whose casefold is longer than they are
("ß"), and non-ASCII whitespace in queries.
"""
from __future__ import annotations

SETTING, MODEL, HELP = "setting", "model", "help"

ENTRIES = [
    [SETTING, "Recording", "Dictation · Microphone, retention and live preview", "recording", ""],
    [SETTING, "Preferred microphone", "Dictation › Recording", "recording",
     "Used for dictation and meetings."],
    [SETTING, "If it's unplugged, use", "Dictation › Recording", "recording",
     "OpenWhisper_CPP moves down this list, even in the middle of a dictation."],
    [SETTING, "Microphone", "Dictation › Recording", "recording", ""],
    [SETTING, "Microphone check", "App › Hotkeys", "hotkeys", "mic test level"],
    [SETTING, "Keep recordings", "Dictation › Recording", "recording", "retention delete audio"],
    [SETTING, "Recording overlay", "App › Appearance", "appearance",
     "Ribbon Bars Mirror wave Aurora Pulse Dots"],
    [SETTING, "Record hotkey", "App › Hotkeys", "hotkeys", "Start recording when idle"],
    [SETTING, "Live preview", "Dictation › Recording", "recording", "Real-time transcription preview"],
    [SETTING, "Theme", "App › Appearance", "appearance", "Dark Light Match system"],
    [SETTING, "Font size", "App › Appearance", "appearance", "Small Default Large Extra large"],
    [SETTING, "Paste into the active window", "App › General", "general",
     "Insert the transcript where you were typing"],
    [SETTING, "Straße und Größe", "App › General", "general", "street size"],
    [SETTING, "STRASSE", "App › General", "general", ""],
    [SETTING, "Developer mode", "App › Developer options", "advanced", "logs diagnostics"],
    [SETTING, "Recording a", "Dictation › Recording", "recording", ""],
    [SETTING, "Recording b", "Dictation › Recording", "recording", ""],
    [SETTING, "Recording c", "Dictation › Recording", "recording", ""],
    [MODEL, "Parakeet TDT 0.6B v3", "Downloads · 640 MB · nvidia/parakeet", "downloads",
     "parakeet-v3 Parakeet model"],
    [MODEL, "Parakeet TDT 0.6B v2", "Downloads · 600 MB", "downloads", "parakeet-v2 Parakeet model"],
    [MODEL, "CUDA runtime", "Downloads › Components · GPU speech", "downloads",
     "component runtime add-on"],
    [MODEL, "CPU runtime", "Downloads › Components · CPU speech", "downloads",
     "component runtime add-on"],
    [MODEL, "Recording model", "Downloads", "downloads", "parakeet model"],
    [MODEL, "Model a", "Downloads", "downloads", "parakeet model"],
    [MODEL, "Model b", "Downloads", "downloads", "parakeet model"],
    [MODEL, "Model c", "Downloads", "downloads", "parakeet model"],
    [HELP, "Voice model", "Parakeet transcribes the whole recording after you stop; live preview "
     "decodes short windows while you speak, so the preview can change as more audio arrives.",
     "voice_model", ""],
    [HELP, "Recording", "Recordings are kept on this computer only.", "recording", ""],
    [HELP, "Downloads", "Models download from Hugging Face over HTTPS.", "downloads", ""],
    [HELP, "Hotkeys", "A recording hotkey works in every app.", "hotkeys", ""],
    [HELP, "Appearance", "Recording overlay looks are drawn live.", "appearance", ""],
]

QUERIES = [
    "", "   ", "rec", "Recording", "recording", "RECORDING", "mic", "microphone",
    "  preferred   MICROPHONE ", "record hotkey", "hotkey record", "parakeet", "parakeet model",
    "model", "runtime", "strasse", "STRASSE", "ß", "größe", "straße größe", "theme dark",
    "dark theme", "live\tpreview", "nothing matches this", "a", "e", "recording a",
    "computer", "deve",
]

LONG_HELP = (
    "Parakeet transcribes the whole recording after you stop. Live preview decodes short "
    "windows while you speak, so the preview can change as more audio arrives; the final "
    "transcript replaces it. Straße and GRÖSSE fold differently, and the window around a match "
    "moves with them when the folded text is longer than the original."
)

SNIPPET_CASES = [
    ["short text", ["text"]],
    [LONG_HELP, ["preview"]],
    [LONG_HELP, ["parakeet"]],
    [LONG_HELP, ["original"]],
    [LONG_HELP, ["missing"]],
    [LONG_HELP, ["final", "preview"]],
    [LONG_HELP, ["größe"]],
    [LONG_HELP, ["grösse", "window"]],
    [LONG_HELP, ["strasse"]],
    [LONG_HELP, []],
    [LONG_HELP, [""]],
    [LONG_HELP, ["preview"], 40],
    [LONG_HELP, ["moves"], 40],
    ["x" * 110, ["x"]],
    ["y" * 111, ["y"]],
]

ACCENT = "#7cc4ff"

HIGHLIGHT_CASES = [
    ["Preferred microphone", ["micro"], ACCENT],
    ["Preferred microphone", ["pre", "mic"], ACCENT],
    ["Straße", ["ss"], ACCENT],
    ["Straße", ["s"], ACCENT],
    ["Straße", ["strasse"], ACCENT],
    ["STRASSE", ["straße"], ACCENT],
    ["Größe & <Maß>", ["ss", "&"], ACCENT],
    ["\"quotes\" and 'apostrophes'", ["quotes", "'"], ACCENT],
    ["aaaa", ["aa"], ACCENT],
    ["aaa", ["aa"], ACCENT],
    ["abc", [""], ACCENT],
    ["abc", [], ACCENT],
    ["", ["a"], ACCENT],
    ["İstanbul", ["i"], ACCENT],
    ["İstanbul", ["i̇"], ACCENT],
    ["ﬁle ﬂow", ["fi", "flo"], "red"],
    ["Record hotkey", ["record", "hotkey"], ACCENT],
    ["Recording overlay", ["rec", "ording o"], ACCENT],
]


def _entries(module):
    return [
        module.SearchEntry(kind, title, detail, destination, keywords=keywords)
        for kind, title, detail, destination, keywords in ENTRIES
    ]


def _match_entries(module, query):
    entries = _entries(module)
    found = module.match_entries(entries, query)
    return [next(i for i, entry in enumerate(entries) if entry is hit) for hit in found]


def _haystack(module, title, detail, keywords):
    return module.SearchEntry(SETTING, title, detail, "", keywords=keywords).haystack()


def _score(module, title, query):
    tokens = module._tokens(query)
    query = " ".join(query.split()).casefold()
    return module._score(module.SearchEntry(SETTING, title, "", ""), query, tokens)


WRAPPERS = {
    "match_entries": _match_entries,
    "SearchEntry.haystack": _haystack,
    "_score": _score,
}

CASES = {
    "match_entries": [[query] for query in QUERIES],
    "SearchEntry.haystack": [
        ["Straße", "App › General", "street"],
        ["Recording", "", ""],
        ["ǅungla", "ΣΑΣ", "ﬁ"],
    ],
    "_score": [
        ["Recording", "rec"], ["Preferred microphone", "microphone"],
        ["Record hotkey", "hotkey record"], ["Microphone check", "check mic"],
        ["Straße", "strasse"], ["Theme", "theme dark"], ["Recording", "  rec  "],
    ],
    "_tokens": [[""], ["  a  b "], ["Straße GRÖSSE"], ["a\tb\nc d"], ["İ"]],
    "_snippet": SNIPPET_CASES,
    "highlight": HIGHLIGHT_CASES,
}

PINS = {
    "entries": ENTRIES,
    "note": "match_entries returns indexes into pins.entries ([kind, title, detail, destination, "
            "keywords]); _score is called as match_entries calls it (query joined and folded)",
}
