"""Curated inputs for services/incremental_dictation.py (fv_speech context_key).

A case is [language, phrases, none]: the wrapper builds a RecognitionContext from the language
and phrases, or passes None when `none` is true.
"""


def _context_key(module, language, phrases, none):
    from services.recognition_context import RecognitionContext

    recognition = None if none else RecognitionContext(language=language, phrases=tuple(phrases))
    return module._context_key(recognition)


WRAPPERS = {"_context_key": _context_key}

CASES = {
    "_context_key": [
        ["", [], True],
        ["fr", ["Ksenia"], True],
        ["", [], False],
        ["fr", ["Ksenia"], False],
        ["fr", ["Ksenia", "Olu"], False],
        ["de", ["Ksenia"], False],
        ["de", [], False],
        ["", ["Ksenia"], False],
        ["auto", ["Émilie", "Köln"], False],
    ],
}
