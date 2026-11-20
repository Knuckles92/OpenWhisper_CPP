"""Record OpenWhisper's dashboard WebSocket hub (meeting/web/ws.py) for C++ replay.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/transcripts/meeting_ws.py

The hub runs inside OpenWhisper's FastAPI app (create_app) behind Starlette's TestClient (no
socket), with the lifespan started so every socket shares one event loop, as under uvicorn. The
engine is a real MeetingStateStore with its "me" participant and no repository (the test file's
AuthzEngine, with engine.py's add_guest); the fixture repository answers get_meeting and
get_last_segments. Ids and time are pinned as in tools/parity/cases/meeting.state.patches.py
(new_id counts from 1, now is 2026-01-01T00:00:00Z), so nothing in the transcript varies from
run to run, and the hub's guest link is the fixed GUEST_URL.

Each step opens a socket, sends a frame on one, or closes one; afterwards every open socket is
read until it stays quiet for QUIET_S, and what each received is recorded in order (a frame's
JSON, or {"closed": code} when the server closed it). Output:
tests/fixtures/transcripts/meeting_ws.json

    {"scenario", "source_commit", "context": {meeting, segments, guest_url, initial_ops},
     "steps": [{"open" | "send" | "close": socket, ...,
                "received": {socket: [{"message": {...}} | {"closed": code}]}}]}

A "send" step carries "text", or "repeat": [unit, count] for a long frame, or "binary_base64".
"""
from __future__ import annotations

import base64
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import golden_format  # noqa: E402
import ow_isolation  # noqa: E402

OUT = ow_isolation.REPO / "tests" / "fixtures" / "transcripts" / "meeting_ws.json"
QUIET_S = 1.0

HOST_TOKEN = "host-ws-token-cccccccccccccccccccc"
GUEST_TOKEN = "guest-ws-token-dddddddddddddddddddd"
GUEST_URL = "http://127.0.0.1:9/m/" + GUEST_TOKEN
NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)

MEETING = {
    "id": "m_ws",
    "title": "WS Meeting",
    "status": "active",
    "started_at": "2026-01-01T12:00:00Z",
    "host_token": HOST_TOKEN,
    "guest_token": GUEST_TOKEN,
    "state_json": "{}",
}
SEGMENTS = [
    {"id": "sg_1", "start_s": 1.0, "end_s": 2.0, "text": "hi", "channel": "mic",
     "speaker_participant_id": None},
    {"id": "sg_2", "start_s": 2.5, "end_s": 4.0, "text": "caf\u00e9 \u65e5", "channel": "loopback",
     "speaker_participant_id": None},
]
INITIAL_OPS = [{"op": "upsert_participant", "display_name": "Me", "kind": "me",
                "is_provisional": False}]


def _action(client_action_id, op):
    message = {"type": "action", "op": op}
    if client_action_id is not None:
        message = {"type": "action", "client_action_id": client_action_id, "op": op}
    return json.dumps(message)


def _undo(client_action_id, seq):
    return json.dumps({"type": "undo", "client_action_id": client_action_id, "seq": seq})


STEPS = [
    {"open": "host", "query": [["token", HOST_TOKEN]]},
    {"open": "guest", "query": [["token", GUEST_TOKEN], ["name", "Sam"], ["guest_id", "g_1"]]},
    {"send": "guest", "text": _action("ca_1", {"op": "add_item", "card": "user_notes",
                                               "text": "Guest note"})},
    {"send": "guest", "text": _action("ca_2", {"op": "set_title", "text": "Guest rename"})},
    {"send": "guest", "text": _action("ca_3", {"op": "set_cloud_enabled", "enabled": True})},
    {"send": "guest", "text": _undo("ca_4", 1)},
    {"send": "host", "text": _action("ca_5", {"op": "set_title", "text": "Host title"})},
    {"send": "host", "text": _undo("ca_6", 2)},
    {"send": "host", "text": '{"type": "ping"}'},
    {"send": "guest", "binary_base64": base64.b64encode(b"\x00\x01\x02not json").decode()},
    {"send": "guest", "repeat": ["[", 20000]},
    {"send": "guest", "repeat": ["x", 64 * 1024 + 1]},
    {"send": "guest", "repeat": ["\u00e9", 64 * 1024]},
    {"send": "host", "text": '{"type": "undo", "client_action_id": "ca_x", "seq": 1e400}'},
    {"send": "host", "text": _undo("ca_y", 10 ** 40)},
    {"send": "host", "text": _undo("ca_z", "2")},
    {"send": "host", "text": _undo(["not", "an", "id"], 3.7)},
    {"send": "host", "text": "[1, 2, 3]"},
    {"send": "host", "text": '"just text"'},
    {"send": "host", "text": "{not json"},
    {"send": "host", "text": _action("ca_m", "not-a-dict")},
    {"send": "host", "text": '{"type": "nonsense"}'},
    {"send": "host", "text": '{"client_action_id": "x"}'},
    {"send": "host", "text": '{"type": 5}'},
    {"send": "host", "text": '{"type": ["a", null, true, 1.5]}'},
    {"send": "host", "text": _action(None, {"op": "add_item", "card": "user_notes",
                                            "text": "Host note \u00e9\u65e5"})},
    {"send": "host", "text": _action(7, {"op": "frobnicate"})},
    {"send": "guest", "text": '{"type": "ping", "extra": 1}'},
    {"close": "guest"},
    {"open": "guest", "query": [["token", GUEST_TOKEN], ["name", "Sam"], ["guest_id", "g_1"]]},
    {"open": "trudy", "query": [["token", GUEST_TOKEN], ["name", "Trudy"], ["guest_id", "g_1"]]},
    {"open": "anon", "query": [["token", GUEST_TOKEN], ["name", "  Ann  "]]},
    {"open": "bad", "query": [["token", "nope"]]},
    {"open": "noname", "query": [["token", GUEST_TOKEN]]},
    {"open": "blank", "query": [["token", GUEST_TOKEN], ["name", "   "]]},
    {"open": "notoken", "query": []},
    {"open": "host2", "query": [["token", "wrong"], ["token", HOST_TOKEN]]},
    {"close": "anon"},
    {"close": "trudy"},
    {"close": "host2"},
    {"close": "guest"},
    {"close": "host"},
]


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


class _Repo:
    def get_meeting(self, meeting_id):
        return dict(MEETING) if meeting_id == MEETING["id"] else None

    def get_last_segments(self, meeting_id, n):
        return [dict(s) for s in SEGMENTS][-n:]


class _Engine:
    def __init__(self):
        from meeting.state.schema import MeetingState
        from meeting.state.store import MeetingStateStore

        self.meeting_id = MEETING["id"]
        self.store = MeetingStateStore(MeetingState(meeting_id=MEETING["id"]))
        self.store.apply("system", None, INITIAL_OPS)

    def apply_client_action(self, actor_type, actor_id, op):
        return self.store.apply(actor_type, actor_id, [op])

    def undo(self, seq, actor_id):
        return self.store.undo(seq, actor_id)

    def add_guest(self, display_name):
        name = (display_name or "").strip()[:80] or "Guest"
        result = self.store.apply("system", None, [{
            "op": "upsert_participant", "display_name": name,
            "kind": "guest", "is_provisional": False,
        }])[0]
        if not result.ok or not result.effect:
            raise ValueError(result.reason or "guest_rejected")
        return dict(result.effect["participant"])


def _receive(ws, timeout):
    """The next message the app sent this socket, or None after `timeout` seconds."""
    import anyio

    async def get():
        with anyio.move_on_after(timeout):
            return await ws._send_rx.receive()
        return None

    return ws.portal.call(get)


def _drain(sockets, closed):
    received = {}
    for name, ws in sockets.items():
        if name in closed:
            continue
        messages = []
        while True:
            message = _receive(ws, QUIET_S)
            if message is None:
                break
            if message["type"] == "websocket.close":
                messages.append({"closed": message.get("code", 1000)})
                closed.add(name)
                break
            messages.append({"message": json.loads(message["text"])})
        if messages:
            received[name] = messages
    return received


def _query_string(pairs):
    from urllib.parse import urlencode

    return urlencode([tuple(pair) for pair in pairs])


def record(commit: str) -> dict:
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils
    from fastapi.testclient import TestClient
    from meeting.web.api import create_app
    from meeting.web.ws import WsHub

    saved = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    try:
        engine = _Engine()
        repo = _Repo()
        hub = WsHub(engine, repo)
        hub.get_guest_url = lambda: GUEST_URL
        app = create_app(engine, repo, hub)
        steps = []
        with TestClient(app, base_url="http://127.0.0.1") as client:
            sockets, closed, sessions = {}, set(), []
            for step in STEPS:
                recorded = dict(step)
                if "open" in step:
                    query = _query_string(step["query"])
                    session = client.websocket_connect("/ws" + ("?" + query if query else ""))
                    ws = session.__enter__()
                    sessions.append(session)
                    sockets[step["open"]] = ws
                    closed.discard(step["open"])
                elif "close" in step:
                    sockets[step["close"]].close(1000)
                    closed.add(step["close"])
                elif "binary_base64" in step:
                    sockets[step["send"]].send_bytes(base64.b64decode(step["binary_base64"]))
                elif "repeat" in step:
                    unit, count = step["repeat"]
                    sockets[step["send"]].send_text(unit * count)
                else:
                    sockets[step["send"]].send_text(step["text"])
                recorded["received"] = _drain(sockets, closed)
                steps.append(recorded)
            for session in reversed(sessions):
                try:
                    session.__exit__(None, None, None)
                except Exception:  # a socket the server closed ends its session with the close
                    pass
    finally:
        schema.uuid, time_utils.datetime = saved
    return {
        "scenario": "meeting_ws",
        "source_commit": commit,
        "python": sys.version.split()[0],
        "context": {"meeting": MEETING, "segments": SEGMENTS, "guest_url": GUEST_URL,
                    "initial_ops": INITIAL_OPS, "now_utc": NOW_UTC.isoformat(),
                    "ids": "new_id gives <prefix>_<n as 12 hex digits>, n counting from 1"},
        "steps": steps,
    }


def main() -> int:
    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    import meeting.web.api  # noqa: F401  (imports config under the isolated folders)

    ow_isolation.after_import_guards(data)
    document = record(commit)
    golden_format.write(OUT, document)
    print(f"{OUT.name}: {len(document['steps'])} steps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
