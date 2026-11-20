"""Op scripts for meeting/state/custom_reports.py (M7.11 meeting-state-extras).

    script(initial, batches)
        As meeting.state.patches's script: initial is a MeetingState.to_dict() document, each
        batch [actor_type, actor_id, known, pinned, ops] goes through patches.apply_ops (whose
        handler table holds CUSTOM_REPORT_HANDLERS); the result is {"results": [...],
        "state": ...}.

Ids and time are pinned as in meeting.state.patches: new_id counts "<prefix>_000000000001", ...
per script, and now is 2026-01-01T00:00:00Z.
"""

import copy
from datetime import datetime, timezone

NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)
WORKER = "custom-report"


class _Pinned(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW_UTC if tz is not None else NOW_UTC.replace(tzinfo=None)


class _Uuid:
    def __init__(self, n):
        self.hex = f"{n:012x}" + "0" * 20


class _FixedUuid:
    def __init__(self):
        self.next = 1

    def uuid4(self):
        value = _Uuid(self.next)
        self.next += 1
        return value


def _result(r):
    return copy.deepcopy({
        "ok": r.ok, "op": r.op, "reason": r.reason, "target_id": r.target_id,
        "effect": r.effect, "inverse": r.inverse, "current_revision": r.current_revision,
        "seq": r.seq,
    })


def _script(module, initial, batches):
    import meeting.state.patches as patches
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils

    saved_uuid, saved_datetime = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    try:
        state = (schema.MeetingState(meeting_id="m_test") if initial is None
                 else schema.MeetingState.from_dict(initial))
        results = []
        for actor_type, actor_id, _known, _pinned, ops in batches:
            ctx = patches.OpContext(actor_type, actor_id)
            results.append([_result(r) for r in patches.apply_ops(state, ops, ctx)])
        return {"results": results, "state": state.to_dict()}
    finally:
        schema.uuid, time_utils.datetime = saved_uuid, saved_datetime


def _state(**extra):
    doc = {"meeting_id": "m_test", "status": "ended", "cloud_enabled": True,
           "finalization": {"status": "completed"}}
    doc.update(extra)
    return doc


def _report(report_id, status="ready", run_id="run_old", **extra):
    report = {"id": report_id, "request": "Earlier ask", "status": status, "title": "Earlier",
              "markdown": "# Earlier", "message": "", "run_id": run_id, "sources": {},
              "requested_by": "p_host", "created_at": "2025-12-31T00:00:00Z",
              "updated_at": "2025-12-31T00:00:00Z"}
    report.update(extra)
    return report


def _batch(actor_type, actor_id, *ops):
    return [actor_type, actor_id, None, None, list(ops)]


def _ask(run_id="run_1", **extra):
    op = {"op": "request_custom_report", "request": "  the commitments  ", "run_id": run_id}
    op.update(extra)
    return op


def _finish(report_id, run_id="run_1", **extra):
    op = {"op": "finish_custom_report", "report_id": report_id, "run_id": run_id,
          "status": "ready", "markdown": "# Commitments\n\n- Priya by Friday"}
    op.update(extra)
    return op


SCRIPTS = [
    [_state(), [
        _batch("user", "p_guest", _ask()),
        _batch("agent", "agent", dict(_ask(), evidence=["sg_1"])),
        _batch("host", "p_host", _ask(request=""), _ask(request="   "), _ask(request=42),
               _ask(request=None), _ask(request="x" * 2001), _ask(request="é" * 2000),
               _ask(run_id=""), _ask(run_id=7)),
    ]],
    [_state(status="active"), [_batch("host", "p_host", _ask())]],
    [_state(finalization={"status": "running"}), [_batch("host", "p_host", _ask())]],
    [_state(finalization={"status": "pending"}), [_batch("system", None, _ask())]],
    [_state(), [
        _batch("host", "p_host", _ask(), _ask(run_id="run_2")),
        _batch("system", "other", _finish("rep_000000000001")),
        _batch("user", "p_guest", _finish("rep_000000000001")),
        _batch("system", WORKER, _finish("rep_missing"), _finish(None), _finish(""),
               _finish("rep_000000000001", run_id="run_x"),
               _finish("rep_000000000001", status="running"),
               _finish("rep_000000000001", status="bogus"),
               _finish("rep_000000000001", status=None),
               _finish("rep_000000000001", markdown="   "),
               _finish("rep_000000000001", markdown=5)),
        _batch("system", WORKER, _finish("rep_000000000001", title="  " + "t" * 205,
                                         message="m" * 505, markdown="r" * 60010,
                                         sources={"transcript_lines": 4})),
        _batch("system", WORKER, _finish("rep_000000000001")),
        _batch("host", "p_host", _ask(run_id="run_2")),
        _batch("system", WORKER, _finish("rep_000000000002", run_id="run_2", status="failed",
                                         markdown=None, message="No API key.",
                                         sources=["not", "a", "dict"])),
        _batch("user", "p_guest", {"op": "remove_custom_report",
                                   "report_id": "rep_000000000001"}),
        _batch("host", "p_host", {"op": "remove_custom_report", "report_id": "rep_000000000001"},
               {"op": "remove_custom_report", "report_id": "rep_000000000001"},
               {"op": "remove_custom_report"}),
        _batch("system", None, {"op": "remove_custom_report", "report_id": "rep_000000000002"}),
    ]],
    [_state(custom_reports=[_report(f"rep_old{n}") for n in range(20)]),
     [_batch("host", "p_host", _ask())]],
    [_state(custom_reports=[_report("rep_stuck", status="running", run_id="run_dead")]),
     [_batch("host", "p_host", _ask(),
             {"op": "remove_custom_report", "report_id": "rep_stuck"}, _ask()),
      _batch("system", WORKER, _finish("rep_stuck", run_id="run_dead"))]],
]

CASES = {"script": [[initial, batches] for initial, batches in SCRIPTS]}

WRAPPERS = {"script": _script}

PINS = {
    "now_utc": NOW_UTC.isoformat(),
    "ids": "new_id gives <prefix>_<n as 12 hex digits>, n counting from 1 per script",
}
