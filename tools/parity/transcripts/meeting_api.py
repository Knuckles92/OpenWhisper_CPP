"""The meeting dashboard's REST routes (meeting/web/api.py) over tests/fixtures/meeting_web/meetings.db.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/record_transcripts.py --scenario meeting_api

OpenWhisper's create_app over a copy of the fixture database (SqlMeetingRepository) and an
engine whose store is a write-through store over the current meeting, as the live engine's is.
Ids and time are pinned as the fixture pins them (new_id counts from 1, now is NOW_UTC); the
dashboard bundle is tests/fixtures/meeting_web/dist; mimetypes reads no Windows registry (the C++
server's table is Python's built-in one). The post-meeting passes are canned per meeting, as the
C++ side injects them: m_busy is busy elsewhere, m_novalue has no transcript, every other meeting
returns RERUN_RESULT; the speaker gate refuses with the saved settings' reason. What needs the
meeting intelligence (a valid review op, a report request that passes validation, the AI toggle
with consent) is left to M8's transcript; here those routes are recorded up to their
validation, plus the note request the archive refuses.

tests/meeting_web/test_api_transcript.cpp replays it against the C++ routes on a loopback server.
"""
from __future__ import annotations

import json
import shutil
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tests" / "fixtures" / "meeting_web" / "meetings.db"
DIST = REPO / "tests" / "fixtures" / "meeting_web" / "dist"
NOW_UTC = datetime(2026, 9, 19, 9, 30, tzinfo=timezone.utc)
HOST = "host-api-token-eeeeeeeeeeeeeeeeeeee"
GUEST = "guest-api-token-ffffffffffffffffffff"
CURRENT = "m_current"
RERUN_RESULT = {"ok": True, "state": {"meeting_id": "m_past", "seq": 12}, "applied": 2,
                "error": None}
BUSY = "post-meeting steps are already running for this meeting"
NO_TRANSCRIPT = "meeting has no transcript"
ROTATED = {"host_url": "http://127.0.0.1:9/m/rotated-host",
           "guest_url": "http://127.0.0.1:9/m/rotated-guest"}

CONTEXT = {
    "fixture": "tests/fixtures/meeting_web/meetings.db",
    "dist": "tests/fixtures/meeting_web/dist",
    "current_meeting": CURRENT,
    "host_token": HOST,
    "guest_token": GUEST,
    "now_utc": NOW_UTC.isoformat(),
    "ids": "new_id gives <prefix>_<n as 12 hex digits>, n counting from 1",
    "rerun_result": RERUN_RESULT,
    "rerun_busy": {"m_busy": BUSY},
    "rerun_value_error": {"m_novalue": NO_TRANSCRIPT},
    "regenerate_tokens": ROTATED,
    "speaker_gate_refusal": "Speaker identification is not set to OpenAI.",
}
NORMALIZE = ["etag", "last-modified"]


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


class _Engine:
    """The engine surface the routes call, over a live write-through store."""

    def __init__(self, repo, spool_root: Path):
        from meeting.stored import open_store

        self.meeting_id = CURRENT
        self.store = open_store(repo, CURRENT, repo.get_meeting(CURRENT), historical=False)
        self.options = SimpleNamespace(spool_root=str(spool_root))
        self.model_lease = None

    def is_active(self):
        return True

    def apply_client_action(self, actor_type, actor_id, op):
        return self.store.apply(actor_type, actor_id, [dict(op)])

    def undo(self, seq, actor_id):
        return self.store.undo(seq, actor_id)

    def end(self):
        pass

    def pause(self):
        pass

    def resume(self):
        pass

    def set_cloud_enabled(self, enabled):
        pass

    def regenerate_tokens(self):
        return dict(ROTATED)

    def request_note_adjustment(self, text):
        raise RuntimeError("Turn on AI insights in an active meeting first.")


def _fake_rerun(repository, meeting_id, **kwargs):
    from meeting.refinalize import FinalizationBusyError

    if meeting_id == "m_busy":
        raise FinalizationBusyError(BUSY)
    if meeting_id == "m_novalue":
        raise ValueError(NO_TRANSCRIPT)
    return json.loads(json.dumps(RERUN_RESULT))


def build(tmp: Path):
    import mimetypes

    # Python's built-in table only, as the C++ server's content_type_for.
    mimetypes.inited = True
    mimetypes._db = mimetypes.MimeTypes()

    import meeting.state.schema as schema
    import meeting.time_utils as time_utils
    import meeting.web.api as api
    import services.meeting_rerun as meeting_rerun
    from meeting.persist.repository import SqlMeetingRepository
    from meeting.web.ws import WsHub
    from services.database import DatabaseManager

    # Pinned for the whole recording (the process records one scenario and exits).
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    database = tmp / "meetings.db"
    shutil.copyfile(FIXTURE, database)
    (tmp / "meetings").mkdir()
    repo = SqlMeetingRepository(db=DatabaseManager(str(database)))
    api._webui_dist_dir = lambda: str(DIST)
    api.rerun_finalization = _fake_rerun
    meeting_rerun.rerun_options = lambda meeting: {}
    engine = _Engine(repo, tmp / "meetings")
    return api.create_app(engine, repo, WsHub(engine, repo))


def get(path, query=(), **extra):
    request = {"method": "GET", "path": path, "query": [list(pair) for pair in query]}
    request.update(extra)
    return request


def post(path, query=(), body=None, **extra):
    request = get(path, query, **extra)
    request["method"] = "POST"
    if body is not None:
        request["body"] = body if isinstance(body, str) else json.dumps(body)
        request["headers"] = {"Content-Type": "application/json"}
    return request


def delete(path, query=(), **extra):
    request = get(path, query, **extra)
    request["method"] = "DELETE"
    return request


H = [("token", HOST)]
G = [("token", GUEST)]

REQUESTS = [
    # --- The page and the bundle (api.py _register_page_routes, the /assets mount) ---
    get(f"/m/{HOST}"),
    get(f"/m/{GUEST}"),
    get("/m/not-a-token"),
    get("/assets/app.js"),
    get("/assets/style.css"),
    get("/assets/nested/deep.js"),
    get("/assets/missing.js"),
    get("/assets/nested"),
    # --- Session and live transcript ---
    get("/api/session"),
    get("/api/session", [("token", "wrong")]),
    get("/api/session", H),
    get("/api/session", G),
    get("/api/session", [("token", "wrong"), ("token", GUEST)]),
    get("/api/transcript", H + [("limit", "2")], paginate=True),
    get("/api/transcript", G),
    get("/api/transcript", H + [("cursor", "bad")]),
    get("/api/transcript", H + [("limit", "abc")]),
    get("/api/transcript", [("limit", "x")]),
    get("/api/transcript", H + [("limit", "0")]),
    get("/api/transcript", H + [("limit", " 3 ")]),
    get("/api/transcript", H + [("limit", "1_0")]),
    get("/api/transcript", H + [("limit", "2.0")]),
    get("/api/transcript", H + [("limit", "2.5")]),
    get("/api/transcript", H + [("limit", "")]),
    get("/api/transcript", H + [("limit", "99999999999999999999999")]),
    # --- History (api.py _register_history_routes) ---
    get("/api/meetings", G),
    get("/api/meetings", [("token", "wrong")]),
    get("/api/meetings", H + [("limit", "3")], paginate=True),
    get("/api/meetings", H + [("limit", "1"), ("has_decisions", "true")], paginate=True),
    get("/api/meetings", H + [("q", "100%_")]),
    get("/api/meetings", H + [("q", "renewal")]),
    get("/api/meetings", H + [("since", "2026-09-17T00:00:00")]),
    get("/api/meetings", H + [("has_actions", "1")]),
    get("/api/meetings", H + [("needs_attention", "yes")]),
    get("/api/meetings", H + [("has_decisions", "TRUE"), ("has_actions", "off")]),
    get("/api/meetings", H + [("has_decisions", "maybe"), ("limit", "two")]),
    get("/api/meetings", H + [("needs_attention", " true")]),
    get("/api/meetings", H + [("cursor", "bad")]),
    get("/api/meetings", H + [("cursor", "x" * 1100)]),
    get("/api/meetings", H + [("limit", "-5")]),
    get("/api/meetings/m_past", H),
    get("/api/meetings/m_past", H + [("include_transcript", "0")]),
    get("/api/meetings/m_current", H),
    get("/api/meetings/m_corrupt", H + [("include_transcript", "false")]),
    get("/api/meetings/m_untitled", H + [("include_transcript", "false")]),
    get("/api/meetings/m_attention", H + [("include_transcript", "no")]),
    get("/api/meetings/m_missing", H),
    get("/api/meetings/m_past", G),
    get("/api/meetings/m_past", H + [("include_transcript", "sometimes")]),
    get("/api/meetings/m_past/state", H),
    get("/api/meetings/m_missing/state", H),
    get("/api/meetings/m_current/state", G),
    # --- One meeting's transcript, segments and recording ---
    get("/api/meetings/m_past/transcript", H),
    get("/api/meetings/m_current/transcript", G + [("limit", "3")], paginate=True),
    get("/api/meetings/m_past/transcript", G),
    get("/api/meetings/m_missing/transcript", H),
    get("/api/meetings/m_current/segments/sg_c3", G),
    get("/api/meetings/m_past/segments/sg_p1", G),
    get("/api/meetings/m_past/segments/sg_p1", H),
    get("/api/meetings/m_past/segments/sg_nope", H),
    get("/api/meetings/m_current/audio", G),
    get("/api/meetings/m_past/audio", G),
    get("/api/meetings/m_missing/audio", H),
    # --- Search and the audit trail ---
    get("/api/search", H + [("q", "renewal")]),
    get("/api/search", H + [("q", "")]),
    get("/api/search", G + [("q", "renewal")]),
    get("/api/search", H + [("q", "renewal"), ("mode", "fuzzy")]),
    get("/api/search", H + [("q", "renewal"), ("mode", "semantic")]),
    get("/api/search", H + [("q", "   "), ("mode", "semantic")]),
    get("/api/events", H),
    get("/api/events", H + [("before_seq", "3")]),
    get("/api/events", H + [("limit", "1")]),
    get("/api/events", H + [("before_seq", "x")]),
    get("/api/events", H + [("before_seq", "")]),
    get("/api/events", G),
    # --- Exports ---
    get("/api/export/md", H + [("meeting_id", "m_past")]),
    get("/api/export/json", H + [("meeting_id", "m_past")]),
    get("/api/export/txt", H + [("meeting_id", "m_past")]),
    get("/api/export/txt", H),
    get("/api/export/pdf", H),
    get("/api/export/md", G),
    get("/api/export/md", H + [("meeting_id", "m_missing")]),
    # --- Reports: download and delete (POST is M8's) ---
    get("/api/meetings/m_past/reports/r_ready/download", H),
    get("/api/meetings/m_past/reports/r_dead/download", H),
    get("/api/meetings/m_past/reports/r_nope/download", H),
    get("/api/meetings/m_past/reports/r_ready/download"),
    get("/api/meetings/m_missing/reports/r_ready/download", H),
    delete("/api/meetings/m_past/reports/r_spare", G),
    delete("/api/meetings/m_past/reports/r_spare", H),
    delete("/api/meetings/m_past/reports/r_spare", H),
    get("/api/meetings/m_past/state", H),
    post("/api/meetings/m_past/reports", H, {"request": ""}),
    post("/api/meetings/m_past/reports", H, {"request": "   "}),
    post("/api/meetings/m_past/reports", H, {"request": 42}),
    post("/api/meetings/m_past/reports", H, {"request": "x" * 2001}),
    post("/api/meetings/m_past/reports", H, {}),
    post("/api/meetings/m_past/reports", H, "not json"),
    post("/api/meetings/m_past/reports", H, "[1, 2]"),
    post("/api/meetings/m_past/reports", G, {"request": "x"}),
    post("/api/meetings/m_past/reports", [], {"request": "x"}),
    # --- Insight review and note requests (up to their validation) ---
    post("/api/meetings/m_past/review", H, {"op": "nope"}),
    post("/api/meetings/m_past/review", H, {}),
    post("/api/meetings/m_past/review", G, {"op": "start"}),
    post("/api/meeting/notes/request", G, {"text": "Shorter notes please"}),
    post("/api/meeting/notes/request", H, {"text": ""}),
    post("/api/meeting/notes/request", H, {"text": "   "}),
    post("/api/meeting/notes/request", H, {"text": 5}),
    post("/api/meeting/notes/request", H, {"text": "x" * 4001}),
    post("/api/meeting/notes/request", [], {"text": "x"}),
    # --- Rename: the live store, the cached review store, the repository ---
    post("/api/meetings/m_current/rename", H, {"title": "  Weekly sync, renamed  "}),
    post("/api/meetings/m_past/rename", H, {"title": "Vendor renewal v2"}),
    post("/api/meetings/m_old/rename", H, {"title": "Older, renamed"}),
    post("/api/meetings/m_missing/rename", H, {"title": "Nobody"}),
    post("/api/meetings/m_past/rename", H, {"title": ""}),
    post("/api/meetings/m_past/rename", H, {"title": 5}),
    post("/api/meetings/m_past/rename", H, {"title": "x" * 201}),
    post("/api/meetings/m_past/rename", G, {"title": "Guest"}),
    post("/api/meetings/m_past/rename", H, "{"),
    get("/api/meetings/m_old", H + [("include_transcript", "0")]),
    get("/api/meetings/m_past/state", H),
    get("/api/events", H),
    # --- Re-runs ---
    post("/api/meetings/m_current/reinsights", H),
    post("/api/meetings/m_missing/reinsights", H),
    post("/api/meetings/m_past/reinsights", G),
    post("/api/meetings/m_past/reinsights", H),
    post("/api/meetings/m_busy/reinsights", H),
    post("/api/meetings/m_novalue/reinsights", H),
    post("/api/meetings/m_current/respeakers", H),
    post("/api/meetings/m_missing/respeakers", H),
    post("/api/meetings/m_digest/respeakers", H),
    post("/api/meetings/m_past/respeakers", H),
    post("/api/meetings/m_past/respeakers", G),
    # --- Delete ---
    delete("/api/meetings/m_current", H),
    delete("/api/meetings/m_missing", H),
    delete("/api/meetings/m_delete", G),
    delete("/api/meetings/m_delete", H),
    delete("/api/meetings/m_delete", H),
    get("/api/meetings", H + [("q", "Delete me")]),
    # --- Meeting controls ---
    post("/api/meeting/end", G),
    post("/api/meeting/end", H),
    post("/api/meeting/pause", H),
    post("/api/meeting/resume", H),
    post("/api/meeting/pause", G),
    post("/api/meeting/cloud", H, {"enabled": True}),
    post("/api/meeting/cloud", H, {"enabled": "false"}),
    post("/api/meeting/cloud", H, {"enabled": False}),
    post("/api/meeting/cloud", H, {}),
    post("/api/meeting/cloud", H, "nope"),
    post("/api/meeting/cloud", G, {"enabled": False}),
    post("/api/meeting/tokens/regenerate", G),
    post("/api/meeting/tokens/regenerate", H),
    # --- FastAPI's own answers ---
    get("/api/nowhere"),
    get("/docs"),
    get("/openapi.json"),
    get("/api/meeting/end"),
    get("/api/meetings/m_past/reports/r_ready"),
]
