"""Curated inputs for services/hf_access.py (M2.5): the consent decision grid, the download error
text over exception chains, and the bundled download sizes.

evaluate_access(cached, env, grant, policy, advisory): a fresh coordinator whose cache answer is
`cached`, with HF_HUB_OFFLINE set to `env` (None: unset), a one-time grant for the model when
`grant`, and the saved policy `policy`; the first evaluation is advisory when `advisory`, and a
second, consuming one follows. The result is both decisions.

describe_model_download_error(model, chain): `chain` lists exceptions as [type name, constructor
arguments...], each the __cause__ of the one before. The result is the facts the function reads
of each link (str, an OSError's errno and winerror, whether it is a socket.gaierror,
ConnectionError or TimeoutError) and the message. Only speech models are named: FioriVoce has no
Whisper sizes.
"""

import os
import socket

MODEL = "parakeet-v3"

_TYPES = {
    "RuntimeError": RuntimeError,
    "ValueError": ValueError,
    "OSError": OSError,
    "PermissionError": PermissionError,
    "ConnectionError": ConnectionError,
    "ConnectionRefusedError": ConnectionRefusedError,
    "TimeoutError": TimeoutError,
    "gaierror": socket.gaierror,
}


def _evaluate_access(module, cached, env, grant, policy, advisory):
    saved_cached = module.is_model_cached
    saved_env = os.environ.get("HF_HUB_OFFLINE")
    module.is_model_cached = lambda name: cached
    if env is None:
        os.environ.pop("HF_HUB_OFFLINE", None)
    else:
        os.environ["HF_HUB_OFFLINE"] = env
    try:
        coordinator = module.HuggingFaceAccessCoordinator()
        coordinator.get_policy = lambda: policy
        if grant:
            coordinator.grant_once(MODEL)
        first = coordinator.evaluate_access(MODEL, consume_grant=not advisory)
        second = coordinator.evaluate_access(MODEL)
        return [first, second]
    finally:
        module.is_model_cached = saved_cached
        if saved_env is None:
            os.environ.pop("HF_HUB_OFFLINE", None)
        else:
            os.environ["HF_HUB_OFFLINE"] = saved_env


def _chain(spec):
    errors = [_TYPES[name](*args) for name, *args in spec]
    for error, cause in zip(errors, errors[1:]):
        error.__cause__ = cause
    return errors


def _facts(error):
    os_error = isinstance(error, OSError)
    return {
        "text": str(error),
        "errno": error.errno if os_error else None,
        "winerror": getattr(error, "winerror", None) if os_error else None,
        "network": isinstance(error, (socket.gaierror, ConnectionError, TimeoutError)),
    }


def _describe(module, model, spec):
    errors = _chain(spec)
    return {
        "chain": [_facts(error) for error in errors],
        "message": module.describe_model_download_error(model, errors[0]),
    }


_POLICIES = ["ask", "always", "never"]
_ENVS = [None, "", "0", "1", " TRUE ", "on", "Yes", "false", "offline"]

CASES = {
    "evaluate_access": [
        [cached, env, grant, policy, advisory]
        for cached in (False, True)
        for env in _ENVS
        for grant in (False, True)
        for policy in _POLICIES
        for advisory in (False, True)
    ],
    "describe_model_download_error": [
        [MODEL, [["RuntimeError", "[Errno -3] Temporary failure in name resolution"]]],
        [MODEL, [["RuntimeError", "[WinError 11001] getaddrinfo failed"]]],
        [MODEL, [["RuntimeError", "Could not reach the download server (timed out)."]]],
        [MODEL, [["OSError", 101, "Network is unreachable"]]],
        [MODEL, [["RuntimeError", "[Errno 28] No space left on device"]]],
        [MODEL, [["OSError", 28, "No space left on device"]]],
        [MODEL, [["OSError", None, "There is not enough space on the disk", None, 112]]],
        [MODEL, [["RuntimeError", "HTTP 500"]]],
        [MODEL, [["RuntimeError", "Model download canceled"]]],
        [MODEL, [["RuntimeError", "Download cancelled by the user"]]],
        [MODEL, [["RuntimeError", "wrapper"], ["ConnectionRefusedError", 10061, "refused"]]],
        [MODEL, [["RuntimeError", "wrapper"], ["TimeoutError", "timed out"]]],
        [MODEL, [["RuntimeError", "wrapper"], ["gaierror", 11001, "getaddrinfo failed"]]],
        [MODEL, [["RuntimeError", "wrapper"], ["ConnectionError", "reset"]]],
        [MODEL, [["RuntimeError", "canceled"], ["OSError", 28, "No space left on device"]]],
        [MODEL, [["OSError", 28, "No space left on device"], ["TimeoutError", "timed out"]]],
        [MODEL, [["ValueError", "Max retries exceeded with url"]]],
        [MODEL, [["PermissionError", 13, "Permission denied"]]],
        [MODEL, [["RuntimeError", "link"]] * 8 + [["RuntimeError", "canceled"]]],
        [MODEL, [["RuntimeError", "link"]] * 7 + [["RuntimeError", "canceled"]]],
        ["orukeet-v0.1", [["OSError", 28, "No space left on device"]]],
        ["unknown-model", [["OSError", 28, "No space left on device"]]],
        ["unknown-model", [["RuntimeError", "boom"]]],
    ],
    "format_download_size": [
        ["parakeet-v3"],
        ["orukeet-v0.1"],
        ["some/custom-repo"],
        ["unknown-model"],
        [""],
    ],
}

WRAPPERS = {
    "evaluate_access": _evaluate_access,
    "describe_model_download_error": _describe,
}

PINS = {
    "evaluate_access_note": "evaluate_access(cached, HF_HUB_OFFLINE or None, grant, policy, "
                            "advisory) -> [first decision, second consuming decision] for "
                            "parakeet-v3",
    "describe_note": "describe_model_download_error(model, [[type, ctor args...], ...]) -> "
                     "{chain: [{text, errno, winerror, network}], message}",
}
