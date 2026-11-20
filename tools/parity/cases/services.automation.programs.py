"""Curated inputs for services/automation/programs.py (M5.11 hook-engines).

    run_event(code, error, event_type, event_id, seq)
        _run_event of a saved program ("C:\\hook.exe", args ["--a", "b c"], 7 s) on an event
        {"x": 1} whose run_once is stubbed to answer (code, b"", error): the record's event_id,
        ok, exit_code and error, and what run_once was handed (argv, stdin, timeout_s and the
        event variables of its environment)

The record's time and duration are left out: they are the clock's.
"""


def _run_event(module, code, error, event_type, event_id, seq):
    from services.automation.events import Event
    from services.automation.models import Program

    seen = {}
    real = module.run_once

    def fake(argv, stdin, *, timeout_s, cwd="", env=None, cancel=None):
        seen.update(
            argv=list(argv), stdin=stdin.decode("utf-8"), timeout_s=timeout_s,
            env={k: v for k, v in sorted((env or {}).items()) if "_EVENT_" in k},
        )
        return code, b"", error

    module.run_once = fake
    try:
        program = Program("p", "Hook", "C:\\hook.exe", ["--a", "b c"], ["recording.started"],
                          True, 7)
        event = Event(event_id, seq, event_type, "2026-01-01T00:00:00.000Z", {"x": 1})
        record = module._run_event(program, event)
    finally:
        module.run_once = real
    return {"event_id": record.event_id, "ok": record.ok, "exit_code": record.exit_code,
            "error": record.error, **seen}


WRAPPERS = {"run_event": _run_event}

CASES = {
    "run_event": [
        [0, "", "recording.started", "evt_0123456789abcdef01234567", 7],
        [2, "", "recording.started", "evt_1", 8],
        [-1, "", "recording.stopped", "evt_2", 9],
        [255, "", "program.test", "evt_3", 0],
        [None, "Timed out after 30 s", "recording.started", "evt_4", 10],
        [None, "Canceled", "recording.started", "evt_5", 11],
        [None, "The program wasn't found.", "recording.started", "evt_6", 12],
        [None, "The program isn't allowed to run (permission denied).", "history.saved",
         "evt_7", 1784000000000],
    ],
}
