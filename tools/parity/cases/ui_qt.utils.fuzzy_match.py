"""Curated inputs for ui_qt/utils/fuzzy_match.py (fv_uimath fuzzy_score, fuzzy_match)."""

CASES = {
    # casefold() is full folding (straße == STRASSE, the ligature fi == fi) and the token split is
    # Unicode \w, so accented and CJK text count as word characters.
    "fuzzy_score": [
        ["", "anything"], ["whisper", "whisper"], ["Whisper", "whisper large"],
        ["large", "Whisper Large v3"], ["lv3", "large-v3"], ["large v3", "large-v3"],
        ["v3 large", "Whisper large-v3"], ["wlv", "whisper large v3"],
        ["parakeet", "nvidia/parakeet-tdt-0.6b-v3"], ["tdt06", "parakeet-tdt-0.6b"],
        ["straße", "STRASSE"], ["é", "Élan"], ["zzz", "whisper"], ["en", "English"],
        ["eng", "English (United States)"], ["us", "English (United States)"],
        ["ﬁle", "FILE"], ["café", "CAFÉ au lait"], ["日本", "日本語モデル"],
        ["  padded  ", "padded"], ["x", "   "], ["a.b", "a-b"], ["(", "a(b"],
        # Each strategy on its own, and the edges of its bonuses and penalties.
        ["model", "my/model"], ["model", "my model"], ["model", "my_model"],
        ["model", "my:model"], ["model", "mymodel"], ["del", "a-very-long-prefix-model"],
        ["x", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaax"],
        ["gpt 4o", "openai gpt-4o"], ["my model", "xmy-modelx"],
        ["4o gpt", "openai/gpt-4o"], ["gpt 4o", "gpt-4o gpt-4o"],
        ["o3 mini", "openai/o3-mini"], ["o mi", "openai/o3-mini"],
        ["ope min", "openai/o3-mini"], ["pen ini", "openai/o3-mini"],
        ["a a", "a b"], ["a a", "a a"], ["b a", "a b"],
        ["g37", "google/gemini-3.7-flash"], ["gf", "google/gemini-3.7-flash"],
        ["ab", "a" + "x" * 100 + "b"], ["abc", "abc-def"], ["ac", "abc"],
        ["--", "a--b"], ["!!", "!!"], ["!!", "x!!y"], ["  ", ""], ["", ""],
        ["A", "a"], ["İ", "i̇"], ["Σ", "σς"], ["ǅ", "ǆ"],
        # Code points, not UTF-16 units: an astral character counts once in the penalties.
        ["model", "😀😀😀 model"], ["mdl", "😀/model"], ["😀", "a😀b"],
        ["naïve", "NAÏVE approach"], ["ﬀ", "FF"], ["x", " x "],
        ["tab\tsep", "tab sep"], ["line\nbreak", "line-break"],
    ],
    "fuzzy_match": [["lv3", "large-v3"], ["zzz", "whisper"], ["", "x"], ["x", ""],
                    ["claude sonnet", "anthropic/claude-3.5-sonnet"], ["???", "gemini"]],
}

TRACE = {
    "tests": ["tests/test_fuzzy_match.py"],
    "functions": ["fuzzy_score", "fuzzy_match"],
}
