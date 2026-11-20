"""Curated inputs for services/automation/filters.py (M5.11 hook-engines).

    interpret(raw)              _interpret of the UTF-8 bytes of `raw`: [kind, text, reason]
    interpret_hex(hex)          the same for bytes given as hex (invalid UTF-8)
    lease_timeout(value)        _lease_timeout: the milliseconds, or {"refused": message}
    lease_modes(value)          _lease_modes: the modes as a list, or {"refused": message}
    register_lease(url, stage, options)
                                register_lease(url, stage, **options) on an empty registry:
                                [url, stage, timeout_ms, modes], or {"refused": message}; a
                                registry that is full first when options has "full": true

The registry is emptied before and after each call.
"""


def _verdict(verdict):
    return [verdict.kind, verdict.text, verdict.reason]


def _refused(call):
    try:
        return call()
    except ValueError as exc:
        return {"refused": str(exc)}


def _register_lease(module, url, stage, options):
    options = dict(options)
    full = options.pop("full", False)
    module.drop_all_leases()
    try:
        if full:
            for _ in range(module.MAX_LEASES):
                module.register_lease("http://127.0.0.1:9/f", "before_cleanup")

        def register():
            lease = module.register_lease(url, stage, **options)
            return [lease.url, lease.stage, lease.timeout_ms, list(lease.modes)]

        return _refused(register)
    finally:
        module.drop_all_leases()


WRAPPERS = {
    "interpret": lambda m, raw: _verdict(m._interpret(raw.encode("utf-8"))),
    "interpret_hex": lambda m, raw: _verdict(m._interpret(bytes.fromhex(raw))),
    "lease_timeout": lambda m, value: _refused(lambda: m._lease_timeout(value)),
    "lease_modes": lambda m, value: _refused(lambda: list(m._lease_modes(value))),
    "register_lease": _register_lease,
}

CASES = {
    "interpret": [[raw] for raw in [
        "", " ", " \r\n\t", "{}", "[]", "null", "5", '"text"', "not json", "{", '{"text": "x"',
        '{"text": "new words"}', '{"text": ""}', '{"text": "  "}', '{"text": 5}', '{"text": null}',
        '{"text": ["a"]}', '{"text": {"a": 1}}', '{"text": true}', '{"cancel": true}',
        '{"cancel": false}', '{"cancel": "yes"}', '{"cancel": 1}', '{"cancel": null}',
        '{"cancel": false, "text": "kept"}', '{"cancel": true, "text": "ignored"}',
        '{"cancel": false, "note": "ignored"}', '{"other": 1}', '{"text": "a", "text": "b"}',
        '{"text": "hi \\ud83d"}', '{"text": "\\ude00 there"}', '{"text": "a\\udfff\\ud800b"}',
        '{"text": "done \\ud83d\\ude00"}', '{"text": "café \U0001f600"}', ' {"text": "x"} \n',
        '{"text": "x"} trailing', '{"text": NaN}', '{"text": "x", "cancel": "no"}',
        "﻿{}",
    ]],
    "interpret_hex": [[raw] for raw in [
        "ff", "7b7d", "c3a9", "7b2274657874223a2022c3a9227d", "7b2274657874223a20228022207d",
        "eda0bd", "7b2274657874223a2022eda0bd227d", "efbbbf7b7d", "f4908080",
    ]],
    "lease_timeout": [[value] for value in [
        1500, 100, 5000, 99, 5001, 50, 6000, 0, -1, 1500.0, 1500.5, 100.0, 99.9, True, False,
        "1500", None, [1500], {"ms": 1500}, 1e300, float("nan"), float("inf"),
    ]],
    "lease_modes": [[value] for value in [
        ["dictation"], ["action"], ["dictation", "action"], ["action", "dictation"],
        ["dictation", "dictation"], ["action", "dictation", "action"], [], ["command"],
        ["file"], ["dictation", "command"], "dictation", "", None, 5, {"dictation": True},
        [5], [None], [True],
    ]],
    "register_lease": [
        ["http://127.0.0.1:8123/filter?x=1", "after_cleanup",
         {"timeout_ms": 800, "modes": ["dictation"]}],
        ["http://localhost:9/f", "before_cleanup", {}],
        ["http://[::1]:9/f", "before_cleanup", {}],
        ["https://LOCALHOST/f", "before_cleanup", {}],
        ["  http://127.0.0.1:9/f  ", "before_cleanup", {}],
        ["http://192.168.1.10/f", "before_cleanup", {}],
        ["http://example.com/f", "before_cleanup", {}],
        ["http://127.0.0.2/f", "before_cleanup", {}],
        ["http://localhost.example.com/f", "before_cleanup", {}],
        ["ftp://127.0.0.1/f", "before_cleanup", {}],
        ["127.0.0.1:9/f", "before_cleanup", {}],
        ["", "before_cleanup", {}],
        ["http://127.0.0.1/f has spaces", "before_cleanup", {}],
        ["http://[::1/f", "before_cleanup", {}],
        ["http://127.0.0.1:9/f", "during_cleanup", {}],
        ["http://127.0.0.1:9/f", "", {}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"timeout_ms": 50}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"timeout_ms": 6000}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"timeout_ms": True}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"timeout_ms": "1500"}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"timeout_ms": 2500.0}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"modes": "dictation"}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"modes": ["command"]}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"modes": []}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"modes": ["file"]}],
        ["http://127.0.0.1:9/f", "before_cleanup", {"full": True}],
        ["http://example.com/f", "during_cleanup", {"timeout_ms": 1, "modes": []}],
    ],
}
