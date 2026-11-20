"""Curated inputs for the speaker-model half of services/components.py (M7.5): the download error
text over each error class.

The functions live in services.components, whose own case file (services.components.py) belongs
to another package; golden_vectors.py imports the module named by this file's stem, so this file
registers services.components under that name and its vectors go to
tests/golden/services.components_speaker.json.

_describe_speaker_download_error(kind, wrapped, *args): the exception the C++ side classifies as
`kind`, built from `args`, optionally inside the FileNotFoundError hf_hub_download wraps transport
failures in. Kinds: "timeout" [httpx class, message], "transport" [httpx class, message],
"http_status" [status], "os_error" [OSError constructor arguments], "other" [class, message]. The
result is the sentence and the facts the C++ replay builds its failure from: str(), the type
name and an OSError's strerror.
"""

import importlib
import sys

import httpx

sys.modules["services.components_speaker"] = importlib.import_module("services.components")

_OTHER = {"RuntimeError": RuntimeError, "ValueError": ValueError}


def _build(kind, args):
    request = httpx.Request("GET", "https://huggingface.co/x")
    if kind in ("timeout", "transport"):
        name, message = args
        return getattr(httpx, name)(message, request=request)
    if kind == "http_status":
        response = httpx.Response(args[0], request=request)
        return httpx.HTTPStatusError("refused", request=request, response=response)
    if kind == "os_error":
        return OSError(*args)
    name, message = args
    return _OTHER[name](message)


def _hf_wrapped(inner):
    try:
        try:
            raise inner
        except BaseException as exc:
            raise FileNotFoundError("cannot find the requested files") from exc
    except FileNotFoundError as outer:
        return outer


def _describe(module, kind, wrapped, *args):
    error = _build(kind, list(args))
    if wrapped:
        error = _hf_wrapped(error)
    return {
        "text": module._describe_speaker_download_error(error),
        "str": str(error),
        "type": type(error).__name__,
        "strerror": error.strerror if isinstance(error, OSError) else None,
    }


_LONG = "x" * 200
_ACCENTED = "é" * 170

CASES = {
    "_describe_speaker_download_error": [
        ["timeout", False, "ReadTimeout", "slow"],
        ["timeout", True, "ReadTimeout", "slow"],
        ["timeout", True, "ConnectTimeout", "timed out"],
        ["timeout", True, "WriteTimeout", ""],
        ["timeout", True, "PoolTimeout", "pool"],
        ["transport", False, "ConnectError", "refused"],
        ["transport", True, "ConnectError", "refused"],
        ["transport", True, "ReadError", "reset"],
        ["transport", True, "RemoteProtocolError", "peer closed connection"],
        ["transport", True, "ProxyError", "proxy refused"],
        ["http_status", False, 403],
        ["http_status", True, 401],
        ["http_status", True, 403],
        ["http_status", True, 404],
        ["http_status", True, 429],
        ["http_status", True, 500],
        ["http_status", True, 503],
        ["os_error", False, 28, "No space left on device"],
        ["os_error", False, 13, "Permission denied"],
        ["os_error", False, None, "There is not enough space on the disk", None, 112],
        ["os_error", False, 28, ""],
        ["os_error", False, "plain message"],
        ["other", False, "RuntimeError", "boom\ntraceback"],
        ["other", False, "RuntimeError", ""],
        ["other", False, "RuntimeError", "\nafter a blank line"],
        ["other", False, "RuntimeError", "first\r\nsecond"],
        ["other", False, "RuntimeError", "first\rsecond"],
        ["other", False, "RuntimeError", "first second"],
        ["other", False, "RuntimeError", "first\x85second"],
        ["other", False, "RuntimeError", "first\x0bsecond"],
        ["other", False, "RuntimeError", "first\x1esecond"],
        ["other", False, "ValueError", _LONG],
        ["other", False, "ValueError", _ACCENTED],
        ["other", True, "RuntimeError", "boom"],
    ],
}

WRAPPERS = {"_describe_speaker_download_error": _describe}

PINS = {
    "describe_note": "_describe_speaker_download_error(kind, wrapped, *args) -> {text, str, "
                     "type, strerror}; kinds timeout, transport, http_status, os_error, other",
}
