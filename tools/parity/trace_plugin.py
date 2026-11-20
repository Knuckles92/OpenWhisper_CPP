"""pytest plugin: record every call OpenWhisper's own tests make to chosen functions.

The second source of golden-vector inputs: instead of curating them, run the existing test files
and keep the inputs they already exercise. golden_vectors.py starts it; by hand:

    python -B -m pytest -p trace_plugin -p no:cacheprovider --basetemp <scratch>/pytest-tmp
        --trace-golden services.format_utils:format_audio_duration,format_timestamp
        --trace-out <scratch>/trace/services.format_utils.json <checkout>/tests/test_format_utils.py

with PYTHONPATH=tools/parity and PYTHONPYCACHEPREFIX=<scratch>/pycache (the suite's conftest
turns bytecode writing back on, so the prefix is what keeps __pycache__ out of the checkout).

The functions are wrapped in pytest_sessionstart, after the suite's conftest has moved the data
root and before test modules are collected, so a test's ``from module import name`` binds the
wrapper. Calls made through a reference taken earlier (a decorator, a default argument, a module
imported by conftest that bound the name at its own import) are missed; this is a limit of the
method, not of one module. Output is written in pytest_sessionfinish, because the suite's conftest
ends the process with os._exit in pytest_unconfigure.
"""
from __future__ import annotations

import functools
import importlib
import json
from pathlib import Path

from golden_format import encode

_calls: dict[str, list[dict]] = {}


def pytest_addoption(parser):
    parser.addoption("--trace-golden", action="append", default=[],
                     help="module:func[,func...] to record")
    parser.addoption("--trace-out", default="traced.json")


def _wrap(module, name):
    original = getattr(module, name)
    key = f"{module.__name__}.{name}"
    seen = _calls.setdefault(key, [])

    def remember(case):
        if case not in seen:
            seen.append(case)

    @functools.wraps(original)
    def recorder(*args, **kwargs):
        case = {"args": encode(list(args))}
        if kwargs:
            case["kwargs"] = encode(kwargs)
        try:
            result = original(*args, **kwargs)
        except Exception as exc:
            case["raises"] = type(exc).__name__
            remember(case)
            raise
        case["result"] = encode(result)
        remember(case)
        return result

    setattr(module, name, recorder)


def pytest_sessionstart(session):
    for spec in session.config.getoption("--trace-golden"):
        module_name, names = spec.split(":", 1)
        module = importlib.import_module(module_name)
        for name in names.split(","):
            _wrap(module, name)


def pytest_sessionfinish(session, exitstatus):
    out = Path(session.config.getoption("--trace-out"))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes((json.dumps({"source": "traced from OpenWhisper tests",
                                 "exitstatus": int(exitstatus), "functions": _calls},
                                indent=1, ensure_ascii=False) + "\n").encode("utf-8"))
    print(f"\ntrace_plugin: {sum(len(v) for v in _calls.values())} distinct calls -> {out}")
