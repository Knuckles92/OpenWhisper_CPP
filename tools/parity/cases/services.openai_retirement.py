"""Curated inputs for services/openai_retirement.py (fv_speech speech/api_models.h).

retired takes an ISO day around the shutdown day. is_model_gone_error takes (status_code, code):
the wrapper raises nothing, it builds the RuntimeError an isolated upload worker returns with
those two SDK fields, as speech::SpeechApiError carries them. The C++ replay is
tests/speech/test_api_models.cpp.
"""
from datetime import date


def _retired(module, day):
    return module.retired(date.fromisoformat(day))


def _is_model_gone_error(module, status_code, code):
    error = RuntimeError("error")
    error.status_code = status_code
    error.code = code
    return module.is_model_gone_error(error)


WRAPPERS = {
    "retired": _retired,
    "is_model_gone_error": _is_model_gone_error,
}

CASES = {
    "retired": [[day] for day in (
        "2026-08-26", "2026-11-14", "2027-02-25", "2027-02-26", "2027-02-27", "2028-01-01",
        "2000-01-01")],
    "is_model_gone_error": [
        [404, None], [404, "model_not_found"], [400, "model_not_found"], [500, "model_not_found"],
        [None, "model_not_found"], [400, "unsupported_value"], [400, None], [401, None],
        [None, None], [422, "invalid_value"], [403, "model_not_found_x"],
    ],
}
