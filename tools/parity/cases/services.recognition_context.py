"""Curated inputs for services/recognition_context.py (M4.4 text-prompts): the engine support
of every dictation engine FioriVoce has, language codes, OpenAI keywords and the vocabulary
prompt. Local Whisper and Nemotron are not FioriVoce engines, and hotwords (faster-whisper) are
not ported, so neither has cases.
"""

_PHRASES = [
    [],
    ["Ksenia", "Kubernetes"],
    ["  spaced   out ", "x" * 65, "x" * 64, "", "   "],
    [f"w{i}" for i in range(60)],
    [f"Word{i:03d}" for i in range(200)],
    ["é" * 64, "é" * 65, "İstanbul"],
    ["a\tb\nc", "d　e"],
]

CASES = {
    "engine_support": [[engine] for engine in ("api", "parakeet", "remote", "", "bogus")],
    "language_code": [[language] for language in (
        "", None, "auto", "AUTO", "Auto", "en", "pt-BR", "zh_CN", "EN-us", "sr-Latn-RS", "-x",
        "İT",
    )],
    "keywords": [[phrases] for phrases in _PHRASES],
    "vocabulary_prompt": [[phrases] for phrases in _PHRASES]
    + [[phrases, limit] for phrases in _PHRASES[1:3] for limit in (0, 1, 7, 8, 18, 19)],
}
