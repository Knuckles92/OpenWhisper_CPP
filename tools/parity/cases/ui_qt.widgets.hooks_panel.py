"""Curated inputs for ui_qt/widgets/hooks_panel.py (M5.17 hooks-page): the words the Hooks page
uses for events, deliveries, runs and addresses.

    event_label(event_type)
    event_types()
    events_summary(events)
    clock(at)                 the zone pinned to UTC, as the reference screenshots pin it
    delivery_result(record)   record: the DeliveryRecord fields as a dict
    run_result(record)        record: the RunRecord fields as a dict
    unencrypted_note(url)
"""
from __future__ import annotations

from datetime import datetime, timezone


class _UtcDatetime(datetime):
    """datetime whose astimezone() with no zone converts to UTC instead of the machine's."""

    def astimezone(self, tz=None):
        return super().astimezone(tz or timezone.utc)


def _clock(module, at):
    real = module.datetime
    module.datetime = _UtcDatetime
    try:
        return module.clock(at)
    finally:
        module.datetime = real


def _delivery(module, spec):
    from services.automation import webhooks

    return module.delivery_result(webhooks.DeliveryRecord(**spec))


def _run(module, spec):
    from services.automation import programs

    return module.run_result(programs.RunRecord(**spec))


def _record(status=200, elapsed_ms=120, ok=True, error="", attempt=1):
    return {"event_id": "evt_1", "event_type": "webhook.test", "at": "2026-10-10T18:02:11.120Z",
            "ok": ok, "status": status, "elapsed_ms": elapsed_ms, "error": error,
            "attempt": attempt}


def _run_record(exit_code=0, elapsed_ms=340, ok=True, error=""):
    return {"event_id": "evt_1", "at": "2026-10-10T18:03:00Z", "ok": ok, "exit_code": exit_code,
            "elapsed_ms": elapsed_ms, "error": error}


CASES = {
    "event_label": [[t] for t in (
        "transcription.completed", "history.saved", "action.ran", "recording.started",
        "recording.stopped", "recording.canceled", "transcription.failed",
        "transcription.preview", "route.decided", "filter.failed", "channel.message",
        "meeting.started", "meeting.ended", "meeting.segments", "meeting.voice_command",
        "webhook.test", "program.test", "x.test", ".test", "events.gap", "a.newer.type", "",
    )],
    "event_types": [[]],
    "events_summary": [[events] for events in (
        [], ["action.ran"], ["action.ran", "history.saved"],
        ["transcription.completed", "action.ran", "history.saved"], ["webhook.test"],
        ["a.newer.type", "action.ran"], ("history.saved",),
    )],
    "clock": [[at] for at in (
        "2026-10-10T18:02:11.120Z", "2026-10-10T18:03:00Z", "2026-03-15T15:00:00+00:00",
        "2026-03-15T15:00:00+05:30", "2026-03-15T23:59:59-07:00", "2026-03-15T01:02:03",
        "2026-03-15 04:05:06", "2026-01-01T00:00:00Z", "2026-12-31T23:59:59.999999Z",
        "not a time", "", "2026-02-30T10:00:00Z", "20261010T180211Z", "2026-10-10",
    )],
    "delivery_result": [[spec] for spec in (
        _record(), _record(200, 118), _record(502, 1430, False, "Bad gateway", 3),
        _record(None, 0, False, ""), _record(None, 5000, False, "Timed out"),
        _record(404, 80, False), _record(200, 95, True, "", 2), _record(201, 0),
        _record(None, 12, False, "Couldn't connect", 4),
    )],
    "run_result": [[spec] for spec in (
        _run_record(), _run_record(2, 80, False), _run_record(None, 30000, False,
                                                              "Timed out after 30 s"),
        _run_record(None, 12, False, ""), _run_record(0, 0), _run_record(1, 5000, False),
        _run_record(-1, 7, False),
    )],
    "unencrypted_note": [[url] for url in (
        "http://93.184.216.34/hook", "http://hooks.example.com/in", "https://hooks.example.com/in",
        "http://127.0.0.1:8080/in", "http://192.168.1.20/in", "http://homeassistant.local:8123/api",
        "  http://hooks.example.com/in  ", "HTTP://HOOKS.EXAMPLE.COM/IN", "ftp://example.com",
        "http://", "http:///path", "", "not a url", "http://[::1]:8080/x", "http://[2001:db8::1]/x",
        "http://[bad/x", "http://user@hooks.example.com:8443/in", "http://10.0.0.5/in",
        "http://localhost:3000", "http://host.lan/in", "http://169.254.1.1/in",
        # urlsplit's own refusals: bracketed hosts and NFKC separators in a non-ASCII netloc.
        "http://[vzz]/x", "http://[v1.fe]/x", "http://[127.0.0.1]/x", "http://[::1]x/in",
        "http://x[::1]/in", "http://[:::::]/in", "http://[fe80::1%25eth0]/in",
        "http://h℀s.example.com/in", "http://hooks.exämple.com/in",
        "http://user@[2001:db8::1]:8080/x",
    )],
}

WRAPPERS = {
    "clock": _clock,
    "delivery_result": _delivery,
    "run_result": _run,
}

PINS = {
    "timezone_note": "clock converts aware values with astimezone(); the cases pin the zone to "
                     "UTC (the wrapper), so replay them with a clock whose zone is UTC",
}
