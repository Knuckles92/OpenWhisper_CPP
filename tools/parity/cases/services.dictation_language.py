"""Curated inputs for services/dictation_language.py (fv_speech dictation_language.h).

Every function over settings dicts whose selected_model is one OpenWhisper_CPP has ("parakeet",
"api", "remote"), a pre-2.0 API name, junk, or nothing. The paired host's languages are never
noted (note_remote_runtime(None) before each call: that branch arrives with the remote engine),
so "remote" reads as a host that has not reported in. Mutators are recorded as the dict they
leave behind.
"""

import copy

_ENGINES = [None, "parakeet", "api", "remote", "api_whisper", "nonsense"]
_CHOSEN = [
    None,
    [],
    ["en"],
    ["en", "es"],
    ["en", "es", "de"],
    ["es", "en", "fr"],
    ["fr", "xx", "auto", "en", "fr", 3, None],
    ["de", "zh", "ja"],
    "en",
]
_ACTIVE = [None, "", "en", "es", "de", "fr", "auto", 5]
_LANGUAGE = [None, "en", "fr", "zh", "auto", "de"]


def _settings(engine, chosen, active, language):
    settings = {}
    if engine is not None:
        settings["selected_model"] = engine
    if chosen is not None:
        settings["dictation_languages"] = copy.deepcopy(chosen)
    if active is not None:
        settings["dictation_active_language"] = active
    if language is not None:
        settings["local_asr_language"] = language
    return settings


def _grid():
    """Every engine with every saved list, each with two of the active languages and one engine
    language: a spread, not the full product (2,592 dicts per function)."""
    dicts = []
    for e, engine in enumerate(_ENGINES):
        for c, chosen in enumerate(_CHOSEN):
            for a in sorted({(e + c) % len(_ACTIVE), (e * 3 + c * 5 + 1) % len(_ACTIVE)}):
                language = _LANGUAGE[(e + c + a) % len(_LANGUAGE)]
                dicts.append(_settings(engine, chosen, _ACTIVE[a], language))
    return dicts


_READERS = ["engine", "accepted_languages", "single_language_reason", "chosen_languages",
            "language_choices", "job_language", "engine_language", "current_language"]


def _reader(name):
    def call(module, settings):
        module.note_remote_runtime(None)
        result = getattr(module, name)(copy.deepcopy(settings))
        return list(result) if isinstance(result, tuple) else result
    return call


def _mutator(name):
    def call(module, settings, *args):
        module.note_remote_runtime(None)
        changed = copy.deepcopy(settings)
        getattr(module, name)(changed, *args)
        return changed
    return call


WRAPPERS = {name: _reader(name) for name in _READERS}
WRAPPERS.update({name: _mutator(name) for name in ("set_active", "set_chosen", "add_chosen",
                                                    "cycle")})

_CODES = ["en", "es", "de", "fr", "ru", "auto", "xx", ""]

CASES = {name: [[settings] for settings in _grid()] for name in _READERS}
CASES["cycle"] = [[settings] for settings in _grid()]
CASES["set_active"] = [[settings, _CODES[i % len(_CODES)]] for i, settings in enumerate(_grid())]
CASES["add_chosen"] = [[settings, _CODES[(i * 3 + 1) % len(_CODES)]]
                       for i, settings in enumerate(_grid())]
_LISTS = ([], ["fr", "es", "fr", "auto"], ["de", "en"], ["xx", "en"], ["es"])
CASES["set_chosen"] = [[settings, _LISTS[i % len(_LISTS)]] for i, settings in enumerate(_grid())]
CASES["label"] = [[code] for code in ["en", "es", "zh", "auto", "de", "ru", "pt", "xx", "",
                                      "EN", "uk", "he"]]
CASES["short_label"] = [[code] for code in ["en", "es", "auto", "zh", "", "pt-BR", "AUTO",
                                            "ß"]]
