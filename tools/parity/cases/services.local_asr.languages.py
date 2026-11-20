"""Curated inputs for services/local_asr/languages.py (fv_speech languages.h).

Only the backend keys OpenWhisper_CPP has ("parakeet", and "api" and "remote", which have no table)
and unknown keys: the tables of the dropped engines (Qwen, Nemotron, Moonshine, MLX, Unified)
are not ported.
"""

_BACKENDS = ["parakeet", "api", "remote", "", "Parakeet", "nonsense"]
_STORED = ["en", "ru", "es", "fr", "pt", "zh", "auto", "de", "", None, "EN", " en", "en-US"]

CASES = {
    "language_choices": [[backend] for backend in _BACKENDS],
    "selected_language": [[backend, stored] for backend in _BACKENDS for stored in _STORED],
}


def _labels(module):
    return list(module.LANGUAGE_LABELS.items())


WRAPPERS = {"LANGUAGE_LABELS": _labels}
CASES["LANGUAGE_LABELS"] = [[]]
