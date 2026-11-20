"""The in-app MCP listener (services/agent_mcp/app.py) over tests/fixtures/agentapi/history.db.

Covers both MCP wires (the 2025-11-25 initialize handshake and the 2026-07-28 per-request
envelope with server/discover), tools/list, a tools/call of each of the 22 tools, argument
validation failures reported as tool errors, the permission refusals and grants of the control
and live tools, the transport's own refusals (Accept, Content-Type, Host allowlist, methods, a
body over 64 KiB), the History API gate in front of /mcp, the disabled 503 and the Tailscale
listener's private-peer check. tests/agentapi/test_mcp_protocol.cpp replays it against
agentapi::McpApp::handle.

The app is built several times (plain loopback, and with Tailscale's allowed hosts), behind a
small ASGI switch that also stands in for what OpenWhisper's tests change between calls. A
POST to /__scenario__ (answered 204, never reaching the app) carries a JSON object whose keys
replace parts of that state before the next request, and the replay applies the same keys:

    app       "main" (allowed_hosts=()) or "tailscale" (allowed_hosts=TAILSCALE_HOSTS)
    enabled   what the app's enabled() answers (MCP turned off while draining)
    peer      the client address requests come from
    settings  the saved settings, replaced whole (the in-memory settings store)
    control   true: the fake app control is registered; false: none is
    fail      null, or what every control call but state() and list_actions() raises:
              {"code", "message"} (a ControlError) or {"permission": key} (PermissionDenied)

The fake app control answers state() with LiveState(True, True, False, "action", "summarize",
"job_rec", "9.9.9"), list_actions() with ACTIONS, run_action with "job_run" and
start_recording with "job_start". The event bus is a fresh EventBus() with nothing published.
"""
from __future__ import annotations

import contextlib
import json
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tests" / "fixtures" / "agentapi" / "history.db"
TOKEN = "mcp-test-token-" + "a" * 32
AUTH = f"Bearer {TOKEN}"
ACCEPT = "application/json, text/event-stream"
MODERN = "2026-07-28"
PROTOCOL_KEY = "io.modelcontextprotocol/protocolVersion"
CAPABILITIES_KEY = "io.modelcontextprotocol/clientCapabilities"
TAILSCALE_HOSTS = ["100.82.22.3", "jed.example.ts.net"]
ACTIONS = [{"id": "summarize", "name": "Summarize", "kind": "webhook"}]

CONTEXT = {
    "token": TOKEN,
    "fixture": "tests/fixtures/agentapi/history.db",
    "database_name": "mcp history.db",
    "tailscale_hosts": TAILSCALE_HOSTS,
    "live_state": [True, True, False, "action", "summarize", "job_rec", "9.9.9"],
    "actions": ACTIONS,
    "run_job": "job_run",
    "start_job": "job_start",
    "initial": {"app": "main", "enabled": True, "peer": "127.0.0.1",
                "settings": {"mcp_enabled": True}, "control": True, "fail": None},
}


class _Control:
    def __init__(self):
        self.fail = None

    def _check(self):
        if self.fail is not None:
            raise self.fail

    def state(self):
        from services.automation.control import LiveState

        return LiveState(*CONTEXT["live_state"])

    def list_actions(self):
        return [dict(action) for action in ACTIONS]

    def run_action(self, action_id, text, *, source):
        self._check()
        return CONTEXT["run_job"]

    def start_recording(self, *, action_id="", source):
        self._check()
        return CONTEXT["start_job"]

    def stop_recording(self):
        self._check()

    def cancel(self):
        self._check()

    def notify(self, text, *, title=""):
        self._check()


class _Switch:
    """Routes each request to the chosen app, as the chosen peer, and applies /__scenario__."""

    def __init__(self, apps, state, settings, control):
        self.apps = apps
        self.state = state
        self.settings = settings
        self.control = control

    async def _lifespan(self, receive, send):
        async with contextlib.AsyncExitStack() as stack:
            await receive()
            for app in self.apps.values():
                await stack.enter_async_context(app.router.lifespan_context(app))
            await send({"type": "lifespan.startup.complete"})
            await receive()
        await send({"type": "lifespan.shutdown.complete"})

    def _apply(self, change):
        from services.automation import permissions
        from services.automation.control import ControlError, set_app_control

        for key in ("app", "enabled", "peer"):
            if key in change:
                self.state[key] = change[key]
        if "settings" in change:
            self.settings.values.clear()
            self.settings.values.update(change["settings"])
        if "control" in change:
            set_app_control(self.control if change["control"] else None)
        if "fail" in change:
            fail = change["fail"]
            if fail is None:
                self.control.fail = None
            elif "permission" in fail:
                self.control.fail = permissions.PermissionDenied(fail["permission"])
            else:
                self.control.fail = ControlError(fail["code"], fail["message"])

    async def __call__(self, scope, receive, send):
        from starlette.responses import Response

        if scope["type"] == "lifespan":
            await self._lifespan(receive, send)
            return
        if scope["path"] == "/__scenario__":
            body = b""
            while True:
                message = await receive()
                body += message.get("body", b"")
                if not message.get("more_body"):
                    break
            self._apply(json.loads(body))
            await Response(status_code=204)(scope, receive, send)
            return
        scope = dict(scope, client=(self.state["peer"], 50000))
        await self.apps[self.state["app"]](scope, receive, send)


def build(tmp: Path):
    from services.agent_mcp.app import create_app
    from services.automation import events
    from services.automation.control import set_app_control
    from tests.fakes.settings import InMemorySettings

    database = tmp / CONTEXT["database_name"]
    shutil.copyfile(FIXTURE, database)
    initial = CONTEXT["initial"]
    state = {key: initial[key] for key in ("app", "enabled", "peer")}
    settings = InMemorySettings(dict(initial["settings"]))
    control = _Control()
    set_app_control(control)
    events.set_bus(events.EventBus())

    def enabled():
        return state["enabled"]

    apps = {
        "main": create_app(database, TOKEN, enabled=enabled, settings=settings),
        "tailscale": create_app(database, TOKEN, enabled=enabled, settings=settings,
                                allowed_hosts=tuple(TAILSCALE_HOSTS)),
    }
    return _Switch(apps, state, settings, control)


# --- Requests ---------------------------------------------------------------------------------


def scenario(**change):
    return {"method": "POST", "path": "/__scenario__",
            "headers": {"Host": "127.0.0.1", "Content-Type": "application/json"},
            "body": json.dumps(change)}


def post(body, headers=None, path="/mcp", **extra):
    sent = {"Host": "127.0.0.1", "Authorization": AUTH, "Accept": ACCEPT,
            "Content-Type": "application/json"}
    sent.update(headers or {})
    request = {"method": "POST", "path": path, "headers": sent, "body": body}
    request.update(extra)
    return request


def message(method, params=None, msg_id=1, *, notification=False):
    out = {"jsonrpc": "2.0"}
    if not notification:
        out["id"] = msg_id
    out["method"] = method
    if params is not None:
        out["params"] = params
    return out


def legacy(method, params=None, msg_id=1, headers=None, **extra):
    return post(json.dumps(message(method, params, msg_id)), headers, **extra)


def modern(method, params=None, msg_id=1, headers=None, meta=None, **extra):
    params = dict(params or {})
    params["_meta"] = meta if meta is not None else {PROTOCOL_KEY: MODERN, CAPABILITIES_KEY: {}}
    sent = {"MCP-Protocol-Version": MODERN, "Mcp-Method": method}
    if method == "tools/call" and isinstance(params.get("name"), str):
        sent["Mcp-Name"] = params["name"]
    sent.update(headers or {})
    return post(json.dumps(message(method, params, msg_id)), sent, **extra)


def call(name, arguments=None, *, wire="legacy", msg_id=1):
    params = {"name": name, "arguments": arguments if arguments is not None else {}}
    if wire == "modern":
        return modern("tools/call", params, msg_id)
    return legacy("tools/call", params, msg_id)


def both(name, arguments=None):
    return [call(name, arguments), call(name, arguments, wire="modern")]


INITIALIZE = {"protocolVersion": "2025-11-25", "capabilities": {},
              "clientInfo": {"name": "test", "version": "1"}}

REQUESTS = [scenario(**CONTEXT["initial"])]

# --- The 2025-11-25 handshake wire ----------------------------------------------------------
REQUESTS += [
    legacy("initialize", INITIALIZE),
    legacy("initialize", dict(INITIALIZE, protocolVersion="2025-06-18"), 2),
    legacy("initialize", dict(INITIALIZE, protocolVersion="2025-03-26"), 3),
    legacy("initialize", dict(INITIALIZE, protocolVersion="2024-11-05"), "four"),
    legacy("initialize", dict(INITIALIZE, protocolVersion="1999-01-01"), 5),
    legacy("initialize", {"protocolVersion": "2025-11-25"}, 6),
    legacy("initialize", None, 7),
    post(json.dumps(message("notifications/initialized", notification=True))),
    post(json.dumps(message("notifications/cancelled", {"requestId": 1}, notification=True))),
    legacy("ping", None, 8),
    legacy("ping", {}, 9),
    legacy("tools/list", None, 10),
    legacy("tools/list", {"cursor": "x"}, 11),
    legacy("resources/list", None, 12),
    legacy("prompts/list", None, 13),
    legacy("server/discover", None, 14),
    legacy("no/such/method", None, 15),
    legacy("resources/templates/list", None, 150),
    legacy("resources/read", {"uri": "file:///x"}, 151),
    legacy("resources/subscribe", {"uri": "file:///x"}, 152),
    legacy("prompts/get", {"name": "x"}, 153),
    legacy("completion/complete", {"ref": {"type": "ref/prompt", "name": "x"},
                                   "argument": {"name": "a", "value": "b"}}, 154),
    legacy("logging/setLevel", {"level": "info"}, 155),
    legacy("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}}, 156),
    legacy("initialize", dict(INITIALIZE, protocolVersion=5), 157),
    legacy("tools/list", {"cursor": 5}, 158),
    legacy("tools/list", {"_meta": {"progressToken": 1}}, 159),
    legacy("ping", {"_meta": {"progressToken": "p"}}, 160),
    legacy("ping", {"x": 1}, 161),
    post(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized", "params": []})),
    post(json.dumps({"jsonrpc": "2.0", "method": "notifications/unknown"})),
    post(json.dumps({"jsonrpc": "2.0", "id": 162, "error": {"code": 1, "message": "x"}})),
    post(json.dumps({"jsonrpc": "2.0", "id": "", "method": "ping"})),
    post(json.dumps({"jsonrpc": "2.0", "id": -5, "method": "ping"})),
    post(json.dumps({"jsonrpc": "2.0", "id": 1.0, "method": "ping"})),
    post('{"jsonrpc": "2.0", "id": 163, "method": "ping"} x'),
    post('{"jsonrpc": "2.0", "id": 164, "method": "ping",}'),
    post('{"jsonrpc": "2.0", "id": 165, "method": "ping", "params": {"a": "\\x"}}'),
    post('{"jsonrpc": "2.0", "id": 166, "method": "pi\nng"}'),
    post('{"jsonrpc": "2.0", "id": 167, "method": "ping", "params": {"a": tru}}'),
    post('{"jsonrpc": "2.0", "id": 168, "method": "ping", "params": {"a": 01}}'),
    post('{"jsonrpc": "2.0", "id": 169, "method": "ping", "params": {"a": [1 2]}}'),
    post('{"jsonrpc": "2.0", "id": 170, "method": "ping", "params": {1: 2}}'),
    post('{"jsonrpc": "2.0", "id": 171, "method": "ping", "params": {"a": -}}'),
    post('{"jsonrpc": "2.0", "id": 172, "method": "ping", "params": {"a": 1.}}'),
    post('{"jsonrpc": "2.0", "id": 173, "method": "ping", "params": {"a": Infinity}}'),
    post('{"jsonrpc": "2.0", "id": 174, "method": "ping", "params": {"a": "\\ud83d\\ude00"}}'),
    post('  {"jsonrpc": "2.0", "id": 175, "method": "ping"}\n'),
    post('{"jsonrpc": "2.0", "id": 176, "method": "ping", "method": "tools/list"}'),
    post(json.dumps({"jsonrpc": "2.0", "id": None, "method": "ping"})),
    post(json.dumps({"jsonrpc": "2.0", "id": 17})),
    post(json.dumps({"jsonrpc": "2.0", "id": 18, "result": {}})),
    post(json.dumps({"jsonrpc": "1.0", "id": 19, "method": "ping"})),
    post(json.dumps({"id": 20, "method": "ping"})),
    post(json.dumps({"jsonrpc": "2.0", "id": 21.5, "method": "ping"})),
    post(json.dumps({"jsonrpc": "2.0", "id": True, "method": "ping"})),
    post(json.dumps({"jsonrpc": "2.0", "id": 22, "method": 5})),
    post(json.dumps({"jsonrpc": "2.0", "id": 23, "method": "ping", "params": []})),
    post(json.dumps([message("ping", None, 24), message("tools/list", None, 25)])),
    post(json.dumps([])),
    post("5"),
    post("{"),
    post(""),
    post(chr(0xFEFF) + json.dumps(message("ping", None, 26))),
    post('{"jsonrpc": "2.0", "id": 27, "method": "ping", "x": NaN}'),
    post(json.dumps(message("ping", None, 28)), {"Accept": "application/json"}),
    post(json.dumps(message("ping", None, 29)), {"Accept": "*/*"}),
    post(json.dumps(message("ping", None, 30)), {"Accept": "application/*"}),
    post(json.dumps(message("ping", None, 31)), {"Accept": "text/event-stream"}),
    post(json.dumps(message("ping", None, 32)), {"Accept": None}),
    post(json.dumps(message("ping", None, 33)), {"Content-Type": "text/plain"}),
    post(json.dumps(message("ping", None, 34)), {"Content-Type": None}),
    post(json.dumps(message("ping", None, 35)), {"Content-Type": "application/json; charset=utf-8"}),
    post(json.dumps(message("ping", None, 36)), {"Content-Type": "Application/JSON"}),
    post(json.dumps(message("ping", None, 37)), {"Mcp-Session-Id": "abc"}),
    {"method": "GET", "path": "/mcp", "headers": {"Host": "127.0.0.1", "Authorization": AUTH,
                                                 "Accept": "application/json"}},
    {"method": "DELETE", "path": "/mcp", "headers": {"Host": "127.0.0.1", "Authorization": AUTH,
                                                    "Accept": ACCEPT}},
    {"method": "PUT", "path": "/mcp", "headers": {"Host": "127.0.0.1", "Authorization": AUTH,
                                                 "Accept": ACCEPT,
                                                 "Content-Type": "application/json"},
     "body": json.dumps(message("ping", None, 38))},
    post(json.dumps(message("ping", None, 39)), path="/mcp/"),
    post(json.dumps(message("ping", None, 40)), path="/mcp/extra"),
    post(json.dumps(message("ping", None, 41)), path="/MCP"),
    {**post(json.dumps(message("ping", None, 42))), "query": [["x", "1"]]},
]

# --- The History API gate and the transport's Host allowlist in front of /mcp ----------------
REQUESTS += [
    legacy("ping", None, 50, {"Authorization": None}),
    legacy("ping", None, 51, {"Authorization": "Bearer wrong"}),
    legacy("ping", None, 52, {"Authorization": f"bearer {TOKEN}"}),
    legacy("ping", None, 53, {"Origin": "https://example.com"}),
    legacy("ping", None, 54, {"Host": "evil.example"}),
    legacy("ping", None, 55, {"Host": "localhost"}),
    legacy("ping", None, 56, {"Host": "localhost:8767"}),
    legacy("ping", None, 57, {"Host": "127.0.0.1:8767"}),
    legacy("ping", None, 58, {"Host": "LOCALHOST"}),
    legacy("ping", None, 59, {"Host": "Localhost:8767"}),
    legacy("ping", None, 60, {"Host": "127.0.0.1:"}),
    legacy("tools/list", None, 61, {"Host": "evil.example", "Authorization": None}),
    {"method": "GET", "path": "/mcp", "headers": {"Host": "127.0.0.1", "Accept": ACCEPT}},
    {"method": "GET", "path": "/v1/nope", "headers": {"Host": "127.0.0.1"}},
]

# --- Bodies over 64 KiB are refused before they are read --------------------------------------
_pad = " " * (64 * 1024 - len(json.dumps(message("ping", None, 62))))
REQUESTS += [
    post(json.dumps(message("ping", None, 62)) + _pad),
    post(json.dumps(message("ping", None, 63)) + _pad + "  "),
]

# --- The 2026-07-28 per-request envelope wire -------------------------------------------------
_meta = {PROTOCOL_KEY: MODERN, CAPABILITIES_KEY: {}}
REQUESTS += [
    modern("server/discover", None, 100),
    modern("server/discover", None, 101,
           meta=dict(_meta, **{"io.modelcontextprotocol/clientInfo": {"name": "t", "version": "1"}})),
    modern("ping", None, 102),
    modern("tools/list", None, 103),
    modern("initialize", INITIALIZE, 104),
    modern("resources/list", None, 105),
    modern("resources/templates/list", None, 180),
    modern("resources/read", {"uri": "file:///x"}, 181),
    modern("prompts/list", None, 182),
    modern("prompts/get", {"name": "x"}, 183),
    modern("tools/list", {"cursor": "x"}, 184),
    modern("tools/call", {"name": "get_status", "arguments": {},
                          "_meta": {PROTOCOL_KEY: MODERN, CAPABILITIES_KEY: {},
                                    "progressToken": 7}}, 185),
    modern("server/discover", None, "s186"),
    modern("logging/setLevel", {"level": "info"}, 187),
    post(json.dumps({"jsonrpc": "2.0", "id": 188, "method": "server/discover",
                     "params": {"_meta": _meta}, "extra": 1}),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}),
    post(json.dumps({"jsonrpc": "1.0", "id": 189, "method": "server/discover",
                     "params": {"_meta": _meta}}),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}),
    post(json.dumps({"jsonrpc": "2.0", "id": 190, "method": "server/discover", "params": []}),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}),
    post(json.dumps({"jsonrpc": "2.0", "id": 191, "method": "server/discover"}),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}),
    post(json.dumps({"jsonrpc": "2.0", "id": True, "method": "server/discover",
                     "params": {"_meta": _meta}}),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}),
    post(chr(0xFEFF) + json.dumps(message("server/discover", {"_meta": _meta}, 192)),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}),
    post('{"jsonrpc": "2.0", "id": 193, "method": "server/discover", "params": {"_meta": '
         '{"io.modelcontextprotocol/protocolVersion": "2026-07-28", '
         '"io.modelcontextprotocol/clientCapabilities": {}}, "x": NaN}}',
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "server/discover"}),
    post(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized", "params": 5}),
         {"MCP-Protocol-Version": MODERN}),
    post(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
         {"MCP-Protocol-Version": MODERN, "Accept": "text/event-stream"}),
    modern("no/such/method", None, 106),
    modern("server/discover", None, 107, meta={}),
    modern("server/discover", None, 108, meta={PROTOCOL_KEY: MODERN}),
    modern("server/discover", None, 109, meta={CAPABILITIES_KEY: {}}),
    modern("server/discover", None, 110, meta=[]),
    modern("server/discover", None, 111, meta={PROTOCOL_KEY: 5, CAPABILITIES_KEY: {}},
           headers={"MCP-Protocol-Version": "5"}),
    modern("server/discover", None, 112, meta={PROTOCOL_KEY: "2027-01-01", CAPABILITIES_KEY: {}},
           headers={"MCP-Protocol-Version": "2027-01-01"}),
    modern("server/discover", None, 113, meta={PROTOCOL_KEY: "2027-01-01", CAPABILITIES_KEY: {}}),
    modern("server/discover", None, 114, headers={"Mcp-Method": "ping"}),
    modern("server/discover", None, 115, headers={"Mcp-Method": None}),
    modern("tools/call", {"name": "get_status", "arguments": {}}, 116,
           headers={"Mcp-Name": "list_actions"}),
    modern("tools/call", {"name": "get_status", "arguments": {}}, 117, headers={"Mcp-Name": None}),
    modern("tools/call", {"name": "get_status", "arguments": {}}, 118,
           headers={"Mcp-Name": "=?base64?Z2V0X3N0YXR1cw==?="}),
    modern("ping", None, 119, headers={"MCP-Protocol-Version": "2025-11-25"}),
    modern("ping", None, 120, headers={"MCP-Protocol-Version": "2027-01-01"}),
    post(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
         {"MCP-Protocol-Version": MODERN}),
    post(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
         {"MCP-Protocol-Version": "2027-01-01"}),
    post(json.dumps({"jsonrpc": "2.0", "method": 5}), {"MCP-Protocol-Version": MODERN}),
    post("{", {"MCP-Protocol-Version": MODERN}),
    post("", {"MCP-Protocol-Version": MODERN}),
    post(json.dumps([message("ping", None, 121)]), {"MCP-Protocol-Version": MODERN}),
    post(json.dumps({"jsonrpc": "2.0", "id": 122}), {"MCP-Protocol-Version": MODERN}),
    post(json.dumps({"jsonrpc": "2.0", "id": None, "method": "ping",
                     "params": {"_meta": _meta}}),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "ping"}),
    post(json.dumps(message("ping", {"_meta": _meta}, 123)),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "ping", "Accept": "text/event-stream"}),
    post(json.dumps(message("ping", {"_meta": _meta}, 124)),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "ping", "Accept": "application/json"}),
    post(json.dumps(message("ping", {"_meta": _meta}, 125)),
         {"MCP-Protocol-Version": MODERN, "Mcp-Method": "ping", "Content-Type": "text/plain"}),
    {"method": "GET", "path": "/mcp", "headers": {"Host": "127.0.0.1", "Authorization": AUTH,
                                                 "Accept": ACCEPT,
                                                 "MCP-Protocol-Version": MODERN}},
    {"method": "DELETE", "path": "/mcp", "headers": {"Host": "127.0.0.1", "Authorization": AUTH,
                                                    "Accept": ACCEPT,
                                                    "MCP-Protocol-Version": MODERN}},
    modern("ping", None, 126, headers={"Host": "LOCALHOST"}),
    modern("ping", None, 127, headers={"Origin": "null"}),
]

# --- Every tool, on both wires, with its default permissions ----------------------------------
for name, arguments in [
    ("get_capabilities", {}),
    ("get_settings", {}),
    ("update_settings", {"changes": {"auto_paste": False}}),
    ("retitle_transcription", {"record_id": "h_a", "title": "Blocked"}),
    ("retitle_meeting", {"meeting_id": "m_a", "title": "Blocked"}),
    ("get_live_state", {}),
    ("list_actions", {}),
    ("run_action", {"action_id": "summarize", "text": "hello"}),
    ("start_dictation", {}),
    ("stop_dictation", {}),
    ("cancel_dictation", {}),
    ("recent_events", {}),
    ("recent_events", {"since_seq": 5, "types": ["recording.started"], "limit": 3}),
    ("notify_user", {"text": "Done"}),
    ("get_status", {}),
    ("search_history", {"q": "launch", "limit": 2}),
    ("list_transcriptions", {}),
    ("list_transcriptions", {"q": "launch", "limit": 3, "since": "2026-08-01T00:00:00Z"}),
    ("get_transcription", {"record_id": "h_a"}),
    ("get_transcription", {"record_id": "h space " + chr(0xE9)}),
    ("get_transcription", {"record_id": "h_remote"}),
    ("get_transcription", {"record_id": "h_remote", "include_remote": True}),
    ("list_meetings", {}),
    ("get_meeting", {"meeting_id": "m_a"}),
    ("list_meeting_segments", {"meeting_id": "m_a", "start_s": 0, "end_s": 10}),
    ("get_meeting_segment", {"meeting_id": "m_a", "segment_id": "sg_a"}),
    ("get_meeting_insights", {"meeting_id": "m_a"}),
    ("get_meeting_insights", {"meeting_id": "m_corrupt"}),
]:
    REQUESTS += both(name, arguments)

# --- Argument validation: the SDK's pydantic errors, as tool errors ---------------------------
for name, arguments in [
    ("search_history", {"q": " "}),
    ("search_history", {"q": ""}),
    ("search_history", {}),
    ("search_history", {"q": "x" * 501}),
    ("search_history", {"q": "launch", "limit": 101}),
    ("search_history", {"q": "launch", "limit": 0}),
    ("search_history", {"q": "launch", "limit": "5"}),
    ("search_history", {"q": "launch", "limit": "abc"}),
    ("search_history", {"q": "launch", "limit": 5.0}),
    ("search_history", {"q": "launch", "limit": 5.5}),
    ("search_history", {"q": "launch", "limit": True}),
    ("search_history", {"q": "launch", "limit": None}),
    ("search_history", {"q": "launch", "kind": "bad"}),
    ("search_history", {"q": "launch", "kind": "meeting"}),
    ("search_history", {"q": "launch", "since": "2026-09-01T00:00:00"}),
    ("search_history", {"q": "launch", "since": "not a date"}),
    ("search_history", {"q": "launch", "since": "2026-09-01T00:00:00+02:00"}),
    ("search_history", {"q": "launch", "since": 1756728000}),
    ("search_history", {"q": "launch", "cursor": "broken"}),
    ("search_history", {"q": "launch", "cursor": "c" * 2049}),
    ("search_history", {"q": "launch", "include_remote": "yes"}),
    ("search_history", {"q": "launch", "include_remote": "maybe"}),
    ("search_history", {"q": "launch", "include_clients": True}),
    ("search_history", {"q": "launch", "device_id": "device_a", "include_remote": True}),
    ("search_history", {"q": 5}),
    ("search_history", {"q": "launch", "extra": 1}),
    ("search_history", {"q": "launch", "limit": 101, "kind": "bad", "cursor": 5}),
    ("list_meeting_segments", {"meeting_id": "m_a", "start_s": 10, "end_s": 1}),
    ("list_meeting_segments", {"meeting_id": "m_a", "start_s": -1}),
    ("list_meeting_segments", {"meeting_id": "m_a", "start_s": "nan"}),
    ("list_meeting_segments", {"meeting_id": "m_a", "end_s": "inf"}),
    ("list_meeting_segments", {"meeting_id": "m_a", "start_s": 2.5}),
    ("list_meeting_segments", {"meeting_id": "m_a", "start_s": "3"}),
    ("get_meeting", {"meeting_id": "missing"}),
    ("get_meeting", {"meeting_id": "../status"}),
    ("get_meeting", {"meeting_id": "a?b"}),
    ("get_meeting", {"meeting_id": "a#b"}),
    ("get_meeting", {"meeting_id": "a\\b"}),
    ("get_meeting", {"meeting_id": "a\nb"}),
    ("get_meeting", {"meeting_id": ""}),
    ("get_meeting", {"meeting_id": "m" * 201}),
    ("get_meeting", {"meeting_id": 5}),
    ("get_meeting", {}),
    ("get_transcription", {"record_id": "h_a", "device_id": "x/y"}),
    ("list_transcriptions", {"q": None}),
    ("list_transcriptions", {"q": ""}),
    ("run_action", {"action_id": "summarize", "text": ""}),
    ("run_action", {"action_id": "summarize", "text": "x" * 20001}),
    ("run_action", {"action_id": "", "text": "x"}),
    ("run_action", {"text": "x"}),
    ("notify_user", {"text": "x" * 501}),
    ("notify_user", {"text": "Done", "title": "t" * 101}),
    ("recent_events", {"types": ["events.gap"]}),
    ("recent_events", {"types": []}),
    ("recent_events", {"types": "recording.started"}),
    ("recent_events", {"types": '["recording.started"]'}),
    ("recent_events", {"limit": 101}),
    ("recent_events", {"since_seq": -1}),
    ("recent_events", {"since_seq": 10 ** 18 - 1}),
    ("start_dictation", {"action_id": None}),
    ("start_dictation", {"action_id": 5}),
    ("update_settings", {}),
    ("update_settings", {"changes": "x"}),
    ("update_settings", {"changes": '{"auto_paste": false}'}),
    ("update_settings", {"changes": {"auto_paste": [1]}}),
    ("retitle_transcription", {"record_id": "h_a"}),
    ("retitle_transcription", {"record_id": "h_a", "title": "x" * 201}),
    ("retitle_transcription", {"record_id": "h_a", "title": ""}),
    ("get_capabilities", {"x": 1}),
    ("nope", {}),
]:
    REQUESTS.append(call(name, arguments))
# pydantic's lax coercion of strings, numbers and booleans.
for name, arguments in [
    ("list_transcriptions", {"since": value}) for value in [
        "2026-09-01", "2026-09-01T00:00Z", "2026-09-01 00:00:00Z", "2026-09-01t00:00:00z",
        "1756728000", 1756728000123, 1756728000.5, -1, "2026-13-01T00:00:00Z",
        "2026-02-30T00:00:00Z", "2026-09-01T25:00:00Z", "2026-09-01T00:00:00.123456789Z",
        "2026-09-01T00:00:00.5+05:30", "2026-09-01T00:00:00-0800", "2026-09-01T00:00:00+24:00",
        "", " 2026-09-01T00:00:00Z", True, [], {}, "2026-9-1", "20260901T000000Z",
        "2026-09-01T00:00:60Z", "2026-09-01T00:00:00 ", "1e9", "nan"]
] + [
    ("list_transcriptions", {"include_remote": value}) for value in [
        "true", "TRUE", "True", "1", "0", "off", "OFF", "y", "N", "t", "f", 1, 0, 2, 1.0, 0.5,
        "", None, " true", [], "yes "]
] + [
    ("list_transcriptions", {"limit": value}) for value in [
        " 5 ", "5.0", "1e1", 10.0, "+5", "-0", "0x5", "1_0", "", 1e20, -5.5, "5.5", [], {}]
] + [
    ("list_meeting_segments", {"meeting_id": "m_a", "start_s": value}) for value in [
        "1e1", True, None, " 2 ", "", "-0", "1e400", [], "0x1"]
] + [
    ("list_transcriptions", {"q": value}) for value in [True, 1.5, None, ["a"], {"a": 1}]
] + [
    ("recent_events", {"since_seq": value}) for value in ["5", 5.0, True, "x", None]
] + [
    ("recent_events", {"types": value}) for value in [
        ["recording.started", "recording.started"], [5], ["recording.started"] * 16, None,
        ("recording.started",), "x"]
] + [
    ("update_settings", {"changes": value}) for value in [
        {"auto_paste": {"a": 1}}, {"auto_paste": None, "ui_theme": [1, 2]}, None, [], 5]
] + [
    ("notify_user", {"text": value}) for value in [5, None, True]
]:
    REQUESTS.append(call(name, arguments))
REQUESTS += [
    legacy("tools/call", {"name": "get_status"}, 200),
    legacy("tools/call", {"name": "get_status", "arguments": None}, 201),
    legacy("tools/call", {"name": "get_status", "arguments": [1]}, 202),
    legacy("tools/call", {"arguments": {}}, 203),
    legacy("tools/call", {"name": 5, "arguments": {}}, 204),
    legacy("tools/call", None, 205),
    modern("tools/call", {"name": "nope", "arguments": {}}, 206),
    modern("tools/call", {"name": "search_history", "arguments": {"q": ""}}, 207),
    modern("tools/call", {"arguments": {}}, 208),
]
# Text that is not valid Unicode (test_automation_fix_mcp_unicode.py): the handshake wire's
# parser refuses the body, the per-request wire's argument validation the value.
for name, raw in [
    ("run_action", '{"action_id": "a1", "text": "fix this \\ud83d"}'),
    ("run_action", '{"action_id": "a\\udc00", "text": "fine"}'),
    ("start_dictation", '{"action_id": "a\\ud800"}'),
    ("notify_user", '{"text": "done \\ud83d"}'),
    ("notify_user", '{"text": "done", "title": "\\ud83d"}'),
    ("search_history", '{"q": "\\udc00"}'),
]:
    REQUESTS.append(post(
        '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"%s","arguments":%s}}'
        % (name, raw)))
    REQUESTS.append(post(
        '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"%s","arguments":%s,'
        '"_meta":{"%s":"%s","%s":{}}}}' % (name, raw, PROTOCOL_KEY, MODERN, CAPABILITIES_KEY),
        {"MCP-Protocol-Version": MODERN, "Mcp-Method": "tools/call", "Mcp-Name": name}))

# --- Permissions: settings, retitles and live tools once granted ------------------------------
_granted = {
    "mcp_enabled": True,
    "mcp_retitle_transcriptions": True,
    "mcp_retitle_meetings": True,
    "mcp_settings_access": True,
    "mcp_writable_settings": {"auto_paste": True, "ui_font_scale": True, "ui_theme": True,
                              "streaming_enabled": True, "mcp_enabled": True, "nope": True,
                              "minimize_tray": "yes"},
    "api_key": "private-marker",
    "streaming_overlay_enabled": True,
    "recording_overlay_look": "orb",
    "streaming_overlay_font_size": "big",
}
REQUESTS.append(scenario(settings=_granted))
for name, arguments in [
    ("get_capabilities", {}),
    ("get_status", {}),
    ("get_settings", {}),
    ("update_settings", {"changes": {"auto_paste": False, "ui_font_scale": 115}}),
    ("update_settings", {"changes": {"auto_paste": True, "ui_theme": "light"}}),
    ("update_settings", {"changes": {"ui_theme": None}}),
    ("update_settings", {"changes": {"ui_theme": "light"}}),
    ("update_settings", {"changes": {"streaming_enabled": True}}),
    ("update_settings", {"changes": {"auto_paste": True, "ui_font_scale": 101}}),
    ("update_settings", {"changes": {"auto_paste": "true"}}),
    ("update_settings", {"changes": {"auto_paste": 1}}),
    ("update_settings", {"changes": {"ui_font_scale": True}}),
    ("update_settings", {"changes": {"ui_font_scale": "100"}}),
    ("update_settings", {"changes": {"ui_font_scale": 100.0}}),
    ("update_settings", {"changes": {"auto_paste": None}}),
    ("update_settings", {"changes": {"minimize_tray": False}}),
    ("update_settings", {"changes": {"copy_clipboard": False}}),
    ("update_settings", {"changes": {"mcp_writable_settings": "grant me everything"}}),
    ("update_settings", {"changes": {"mcp_enabled": False}}),
    ("update_settings", {"changes": {}}),
    ("update_settings", {"changes": {f"k{i}": True for i in range(27)}}),
    ("update_settings", {"changes": {"ui_theme": "x" * 21}}),
    ("update_settings", {"changes": {"ui_theme": "neon"}}),
    ("get_settings", {}),
    ("retitle_transcription", {"record_id": "h_a", "title": "  Project plan  "}),
    ("get_transcription", {"record_id": "h_a"}),
    ("search_history", {"q": "Project plan"}),
    ("retitle_transcription", {"record_id": "h_remote", "title": "Remote"}),
    ("retitle_transcription", {"record_id": "missing", "title": "Missing"}),
    ("retitle_transcription", {"record_id": "../status", "title": "Invalid id"}),
    ("retitle_transcription", {"record_id": "h_a", "title": " "}),
    ("retitle_transcription", {"record_id": "h_a", "title": "New\nline"}),
    ("retitle_transcription", {"record_id": "h_a", "title": "Caf" + chr(0xE9) + " " + chr(0x6771) + chr(0x4EAC)}),
    ("retitle_meeting", {"meeting_id": "m_a", "title": "Project meeting"}),
    ("get_meeting", {"meeting_id": "m_a"}),
    ("get_meeting_insights", {"meeting_id": "m_a"}),
    ("retitle_meeting", {"meeting_id": "m_active", "title": "Busy"}),
    ("retitle_meeting", {"meeting_id": "m_remote", "title": "Remote"}),
    ("retitle_meeting", {"meeting_id": "m_corrupt", "title": "Corrupt"}),
    ("retitle_meeting", {"meeting_id": "m_list", "title": "List"}),
    ("retitle_meeting", {"meeting_id": "m_empty", "title": "Empty " + chr(0xE9) + "t" + chr(0xE9)}),
    ("get_meeting", {"meeting_id": "m_empty"}),
    ("retitle_meeting", {"meeting_id": "h_a", "title": "Not a meeting"}),
]:
    REQUESTS.append(call(name, arguments))

# Live tools with each permission.
REQUESTS += [
    scenario(settings={"mcp_enabled": True, "api_run_actions": True}),
    call("run_action", {"action_id": "summarize", "text": "hello"}),
    call("run_action", {"action_id": "summarize", "text": "ok \U0001f600"}, wire="modern"),
    call("notify_user", {"text": "Done", "title": "Claude"}),
    call("start_dictation", {}),
    call("get_status", {}),
    scenario(fail={"code": "busy", "message": "OpenWhisper is busy with another job."}),
    call("run_action", {"action_id": "summarize", "text": "again"}),
    scenario(fail={"permission": "api_agent_prompts"}),
    call("run_action", {"action_id": "agent", "text": "push to main"}, wire="modern"),
    scenario(fail={"code": "not_found", "message": "No action has that id."}),
    call("notify_user", {"text": "Done"}),
    scenario(fail=None, settings={"mcp_enabled": True, "api_control_recording": True}),
    call("start_dictation", {}),
    call("start_dictation", {"action_id": "summarize"}, wire="modern"),
    call("start_dictation", {"action_id": ""}),
    call("stop_dictation", {}),
    call("cancel_dictation", {}),
    call("notify_user", {"text": "Done"}),
    call("get_status", {}),
    scenario(fail={"permission": "api_agent_prompts"}),
    call("start_dictation", {"action_id": "agent"}),
    scenario(fail={"code": "refused", "message": "Recording is not possible right now."}),
    call("stop_dictation", {}),
    scenario(fail=None, settings={"mcp_enabled": True, "api_live_events": False,
                                  "api_text_filters": True}),
    call("recent_events", {}),
    call("get_live_state", {}),
    call("get_status", {}),
    scenario(settings={"mcp_enabled": True, "api_agent_prompts": True, "mcp_events_enabled": True}),
    call("get_status", {}),
    call("get_capabilities", {}, wire="modern"),
    scenario(settings={"mcp_enabled": True, "api_live_events": "yes", "api_run_actions": 1}),
    call("get_live_state", {}),
    scenario(control=False),
    call("get_live_state", {}),
    call("list_actions", {}),
    scenario(control=True, settings={"mcp_enabled": True}),
]

# --- Turned off: every request is refused first ------------------------------------------------
REQUESTS += [
    scenario(enabled=False),
    legacy("initialize", INITIALIZE, 300),
    legacy("ping", None, 301, {"Authorization": None}),
    {"method": "GET", "path": "/v1/transcriptions",
     "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    {"method": "GET", "path": "/v1/nope", "headers": {"Host": "evil.example"}},
    scenario(enabled=True),
]

# --- Tailscale: allowed hosts and the private-peer check ---------------------------------------
_status = {"method": "GET", "path": "/v1/status", "headers": {"Authorization": AUTH}}
REQUESTS += [
    scenario(app="tailscale", peer="100.65.148.53"),
    {**_status, "headers": {"Host": "100.82.22.3"}},
    {**_status, "headers": {"Host": "100.82.22.3", "Authorization": AUTH}},
    {**_status, "headers": {"Host": "100.82.22.3:8767", "Authorization": AUTH}},
    {**_status, "headers": {"Host": "jed.example.ts.net", "Authorization": AUTH}},
    {**_status, "headers": {"Host": "100.82.22.4", "Authorization": AUTH}},
    {**_status, "headers": {"Host": "100.82.22.3", "Authorization": AUTH,
                            "Origin": "http://100.82.22.3"}},
    legacy("initialize", INITIALIZE, 400, {"Host": "100.82.22.3"}),
    legacy("ping", None, 401, {"Host": "100.82.22.3:8767"}),
    legacy("ping", None, 402, {"Host": "jed.example.ts.net:8767"}),
    legacy("ping", None, 403, {"Host": "JED.example.ts.net"}),
    scenario(peer="127.0.0.1"),
    {**_status, "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    scenario(peer="::1"),
    {**_status, "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    scenario(peer="::ffff:100.65.148.53"),
    {**_status, "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    scenario(peer="fd7a:115c:a1e0::1"),
    {**_status, "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    scenario(peer="192.168.1.10"),
    {**_status, "headers": {"Host": "100.82.22.3", "Authorization": AUTH}},
    {**_status, "headers": {"Host": "evil.example"}},
    scenario(peer="testclient"),
    {**_status, "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    scenario(enabled=False),
    {**_status, "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    scenario(app="main", enabled=True, peer="192.168.1.10"),
    {**_status, "headers": {"Host": "100.82.22.3", "Authorization": AUTH}},
    {**_status, "headers": {"Host": "127.0.0.1", "Authorization": AUTH}},
    scenario(peer="127.0.0.1"),
]
