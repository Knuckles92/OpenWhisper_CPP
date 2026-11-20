"""The live API (services/automation/api.py) as OpenWhisper's in-app MCP listener mounts it.

services/agent_mcp/app.py create_app over tests/fixtures/agentapi/history.db, with its live router,
its History API gate and its MCP-off check, answers every live route: the token gate, the
permission refusals, request bodies (422 texts, 413 before reading, lone surrogates), path ids over
64 characters, FastAPI's parameter checks of /v1/events, the control errors, filter leases
(create, renew, delete, the peer check), notices, event streams with their replay and gaps, the
405s of live paths and the MCP app's /openapi.json. tests/agentapi/test_live_api.cpp replays it
against agentapi::McpApp::serve with agentapi::LiveApi installed.

An event stream runs until MCP is switched off, so the live router is given an enabled() of its
own that also answers False while "streams" is False (the default here): a stream then sends
"retry", its replay and its gap, and ends, which TestClient can record. The middleware's enabled()
is the scenario's "enabled" alone, as create_app passes it.

A POST to /__scenario__ (answered 204, never reaching the app) carries a JSON object whose keys
change the state before the next request; the replay applies the same keys:

    app       "main" (allowed_hosts=()) or "tailscale" (allowed_hosts=TAILSCALE_HOSTS)
    enabled   what the app's enabled() answers
    streams   False: an event stream ends after its replay
    peer      the client address requests come from
    settings  the saved settings, replaced whole
    control   true: the fake app control is registered; false: none is
    fail      null, or what every control call but state() and list_actions() raises:
              {"code", "message"} (a ControlError), {"permission": key} (PermissionDenied) or
              {"error": "RuntimeError"} (RuntimeError("the secret words"))
    bus       a new event bus: {"buffer_size", "seq_base"}
    publish   events to publish on the bus, in order: [type, data] or [type, data, replay]
    leases    "clear": drop every filter lease

The fake app control answers state() with CONTEXT["live_state"] and list_actions() with
ACTIONS, run_action with "job-run" and start_recording with "job-rec". Filter leases are the real
filters module with its clock and ids pinned: time.time() is LEASE_NOW and lease n (from 1) is
"flt_" followed by n in 16 hex digits.
"""
from __future__ import annotations

import contextlib
import json
import shutil
import time as real_time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tests" / "fixtures" / "agentapi" / "history.db"
TOKEN = "live-api-test-token-" + "b" * 32
AUTH = f"Bearer {TOKEN}"
TAILSCALE_HOSTS = ["100.82.22.3", "jed.example.ts.net"]
ACTIONS = [{"id": "a1", "name": "Ask Claude", "kind": "open_link", "enabled": True}]
LEASE_NOW = 1_800_000_000.0

CONTEXT = {
    "token": TOKEN,
    "fixture": "tests/fixtures/agentapi/history.db",
    "database_name": "live history.db",
    "tailscale_hosts": TAILSCALE_HOSTS,
    "live_state": [True, False, False, "action", "a1", "job-1", "9.9.9"],
    "actions": ACTIONS,
    "run_job": "job-run",
    "start_job": "job-rec",
    "lease_now": LEASE_NOW,
    "initial": {"app": "main", "enabled": True, "streams": False, "peer": "127.0.0.1",
                "settings": {}, "control": True, "fail": None,
                "bus": {"buffer_size": 500, "seq_base": 0}},
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


class _PinnedTime:
    """filters.time: wall-clock time pinned, monotonic time real (leases must not lapse)."""

    @staticmethod
    def time():
        return LEASE_NOW

    monotonic = staticmethod(real_time.monotonic)


class _CountingSecrets:
    """filters.secrets: lease ids numbered from 1."""

    def __init__(self):
        self.count = 0

    def token_hex(self, nbytes):
        self.count += 1
        return format(self.count, f"0{2 * nbytes}x")


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
        from services.automation import events, filters, permissions
        from services.automation.control import ControlError, set_app_control

        for key in ("app", "enabled", "streams", "peer"):
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
            elif "error" in fail:
                self.control.fail = RuntimeError("the secret words")
            else:
                self.control.fail = ControlError(fail["code"], fail["message"])
        if "bus" in change:
            previous = events.set_bus(events.EventBus(**change["bus"]))
            if previous is not None:
                previous.close()
        for item in change.get("publish", []):
            events.bus().publish(item[0], item[1], replay=item[2] if len(item) > 2 else True)
        if change.get("leases") == "clear":
            filters.drop_all_leases()

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
    from services.automation import api, events, filters
    from services.automation.control import set_app_control
    from tests.fakes.settings import InMemorySettings

    database = tmp / CONTEXT["database_name"]
    shutil.copyfile(FIXTURE, database)
    initial = CONTEXT["initial"]
    state = {key: initial[key] for key in ("app", "enabled", "streams", "peer")}
    settings = InMemorySettings(dict(initial["settings"]))
    control = _Control()
    set_app_control(control)
    events.set_bus(events.EventBus(**initial["bus"]))
    filters.drop_all_leases()
    filters.time = _PinnedTime
    filters.secrets = _CountingSecrets()

    def enabled():
        return state["enabled"]

    def streams_enabled():
        return state["enabled"] and state["streams"]

    real_router = api.create_live_router

    def live_router(router_settings, *, enabled=None):
        return real_router(router_settings, enabled=streams_enabled)

    api.create_live_router = live_router
    try:
        apps = {
            "main": create_app(database, TOKEN, enabled=enabled, settings=settings),
            "tailscale": create_app(database, TOKEN, enabled=enabled, settings=settings,
                                    allowed_hosts=tuple(TAILSCALE_HOSTS)),
        }
    finally:
        api.create_live_router = real_router
    return _Switch(apps, state, settings, control)


# --- Requests ---------------------------------------------------------------------------------


def scenario(**change):
    return {"method": "POST", "path": "/__scenario__",
            "headers": {"Host": "127.0.0.1", "Content-Type": "application/json"},
            "body": json.dumps(change)}


def call(method, path, body=None, headers=None, query=None, raw=None):
    sent = {"Host": "127.0.0.1", "Authorization": AUTH}
    if body is not None or raw is not None:
        sent["Content-Type"] = "application/json"
    sent.update(headers or {})
    request = {"method": method, "path": path, "headers": sent}
    if raw is not None:
        request["body"] = raw
    elif body is not None:
        request["body"] = json.dumps(body)
    if query is not None:
        request["query"] = query
    return request


def get(path, **extra):
    return call("GET", path, **extra)


def post(path, body=None, **extra):
    return call("POST", path, body, **extra)


ROUTES = [
    ("GET", "/v1/live/state"),
    ("GET", "/v1/events"),
    ("GET", "/v1/actions"),
    ("POST", "/v1/actions/a1/run"),
    ("POST", "/v1/recording/start"),
    ("POST", "/v1/recording/stop"),
    ("POST", "/v1/recording/cancel"),
    ("POST", "/v1/filters"),
    ("POST", "/v1/filters/l1/renew"),
    ("DELETE", "/v1/filters/l1"),
    ("POST", "/v1/notify"),
]
LOCAL_FILTER = {"url": "http://127.0.0.1:9/f", "stage": "after_cleanup"}
HALF_EMOJI = '"fix this \\ud83d"'

REQUESTS = [scenario(**CONTEXT["initial"])]

# --- The token gate: every live route answers 401 without it, and 403 for another Host --------
for method, path in ROUTES:
    REQUESTS.append(call(method, path, headers={"Authorization": None}))
REQUESTS += [
    get("/v1/live/state", headers={"Authorization": "Bearer wrong"}),
    get("/v1/live/state", headers={"Host": "evil.example"}),
    get("/v1/actions", headers={"Origin": "https://example.com"}),
]

# --- Defaults: state and actions with the token alone, every powerful route refused ------------
REQUESTS += [
    get("/v1/live/state"),
    get("/v1/actions"),
    post("/v1/actions/a1/run", {"text": "hello"}),
    post("/v1/notify", {"text": "hello"}),
    post("/v1/recording/start", {}),
    post("/v1/recording/start"),
    post("/v1/recording/stop"),
    post("/v1/recording/cancel"),
    post("/v1/filters", LOCAL_FILTER),
    post("/v1/filters/l1/renew"),
    call("DELETE", "/v1/filters/l1"),
    # FastAPI checks a path id before the handler, so an id over 64 characters is a 422 first.
    post("/v1/actions/" + "a" * 65 + "/run", {"text": "hi"}),
    post("/v1/filters/" + "l" * 65 + "/renew"),
    call("DELETE", "/v1/filters/" + "l" * 65),
    # The body is read before the permission is checked.
    post("/v1/actions/a1/run", raw="not json"),
]

# --- Running actions ----------------------------------------------------------------------------
REQUESTS += [
    scenario(settings={"api_run_actions": True}),
    get("/v1/live/state"),
    post("/v1/actions/a1/run", {"text": "make it so"}),
    post("/v1/actions/" + "a" * 64 + "/run", {"text": "hi"}),
    post("/v1/actions/a%20b/run", {"text": "hi"}),
    post("/v1/actions/a1/run", {"text": "x" * 20000}),
    post("/v1/actions/a1/run", raw='{"text": "' + "\\u00e9" * 300 + '"}'),
]
for body in [{}, {"text": ""}, {"text": 5}, {"text": ["hi"]}, {"text": None}, {"text": True},
             {"text": "x" * 20001}, {"other": "x"}]:
    REQUESTS.append(post("/v1/actions/a1/run", body))
for raw in ["not json", "[1, 2]", '"text"', "5", "null", "", "   ", " \r\n\t ", "{", '{"text": "a",}',
            '{"text": NaN}', '{"text": "a", "text": "b"}', '﻿{"text": "bom"}',
            '{"text": %s}' % HALF_EMOJI, '{"text": "\\udc00"}',
            '{"text": "fix this \\ud83d\\ude00 caf\\u00e9"}', '{"text": "ok"} trailing']:
    REQUESTS.append(post("/v1/actions/a1/run", raw=raw))
REQUESTS += [
    post("/v1/actions/a1/run", raw='{"text": "hi"}', headers={"Content-Type": None}),
    post("/v1/actions/a1/run", raw='{"text": "hi"}', headers={"Content-Type": "text/plain"}),
    post("/v1/notify", {"text": "Build passed", "title": "CI"}),
    post("/v1/notify", {"text": "Done"}),
    post("/v1/notify", {"text": "Done", "title": None}),
    post("/v1/notify", {"text": "Done", "title": ""}),
]
for body in [{"text": "   "}, {"text": " "}, {"text": "x" * 501}, {"text": "x" * 500},
             {"text": "ok", "title": "t" * 101}, {"text": "ok", "title": "t" * 100},
             {"text": "ok", "title": 5}, {}, {"title": "x"}]:
    REQUESTS.append(post("/v1/notify", body))
REQUESTS += [
    post("/v1/notify", raw='{"text": %s}' % HALF_EMOJI),
    post("/v1/notify", raw='{"text": "ok", "title": %s}' % HALF_EMOJI),
    # Content-Length over the limit is refused before anything is read.
    post("/v1/notify", raw="{" + " " * (16 * 1024) + "}"),
    post("/v1/notify", raw="{" + " " * (16 * 1024 - 2) + "}"),
]

# --- Recording -------------------------------------------------------------------------------
REQUESTS += [
    scenario(settings={"api_control_recording": True}),
    post("/v1/recording/start"),
    post("/v1/recording/start", {}),
    post("/v1/recording/start", {"action_id": "a1"}),
    post("/v1/recording/start", {"action_id": ""}),
    post("/v1/recording/start", {"action_id": None}),
    post("/v1/recording/start", {"action_id": 3}),
    post("/v1/recording/start", {"action_id": "a" * 65}),
    post("/v1/recording/start", {"action_id": "a" * 64}),
    post("/v1/recording/start", raw='{"action_id": %s}' % HALF_EMOJI),
    post("/v1/recording/start", raw="[]"),
    post("/v1/recording/stop"),
    post("/v1/recording/stop", {"ignored": True}),
    post("/v1/recording/cancel"),
    post("/v1/actions/a1/run", {"text": "hi"}),
    post("/v1/notify", {"text": "hi"}),
]

# --- What the app answers: control errors, a permission it checks, an unexpected failure ------
REQUESTS.append(scenario(settings={"api_run_actions": True, "api_control_recording": True}))
for code, message in [("busy", "OpenWhisper is busy with another recording."),
                      ("not_found", "No enabled action has that id."),
                      ("refused", "Speech engine is still loading"),
                      ("unavailable", "OpenWhisper didn't respond in time. Try again."),
                      ("odd", "An unknown code.")]:
    REQUESTS += [
        scenario(fail={"code": code, "message": message}),
        post("/v1/actions/a1/run", {"text": "hi"}),
        post("/v1/recording/start", {"action_id": "a1"}),
        post("/v1/recording/stop"),
        post("/v1/recording/cancel"),
        post("/v1/notify", {"text": "hi"}),
        get("/v1/live/state"),
        get("/v1/actions"),
    ]
REQUESTS += [
    scenario(fail={"permission": "api_agent_prompts"}),
    post("/v1/actions/agent/run", {"text": "delete the tests"}),
    post("/v1/recording/start", {"action_id": "agent"}),
    scenario(fail={"error": "RuntimeError"}),
    post("/v1/actions/a1/run", {"text": "the secret words"}),
    post("/v1/recording/stop"),
    scenario(fail=None, control=False),
    get("/v1/live/state"),
    get("/v1/actions"),
    post("/v1/actions/a1/run", {"text": "hi"}),
    post("/v1/recording/start"),
    post("/v1/notify", {"text": "hi"}),
    scenario(control=True),
]

# --- Filter leases --------------------------------------------------------------------------
REQUESTS += [
    scenario(settings={"api_text_filters": True}, leases="clear"),
    post("/v1/filters", dict(LOCAL_FILTER, timeout_ms=900)),
    post("/v1/filters", LOCAL_FILTER),
    post("/v1/filters", dict(LOCAL_FILTER, timeout_ms=None)),
    post("/v1/filters/flt_0000000000000001/renew"),
    post("/v1/filters/flt_0000000000000001/renew", {"ignored": True}),
    post("/v1/filters/other/renew"),
    call("DELETE", "/v1/filters/flt_0000000000000002"),
    call("DELETE", "/v1/filters/flt_0000000000000002"),
    post("/v1/filters/flt_0000000000000002/renew"),
]
for body in [
    {"url": "http://x/f", "stage": "bad"},
    {"url": "http://192.168.1.10:9/f", "stage": "after_cleanup"},
    {"url": "ftp://127.0.0.1/f", "stage": "after_cleanup"},
    {"url": " http://localhost:9/f ", "stage": "before_cleanup"},
    {"url": "http://[::1]:9/f", "stage": "after_cleanup"},
    {"url": "http://127.0.0.1:9/f", "stage": "during"},
    {"url": "http://127.0.0.1:9/f", "stage": ""},
    {"stage": "after_cleanup"},
    {"url": "http://127.0.0.1:9/f"},
    {"url": "", "stage": "after_cleanup"},
    {"url": 5, "stage": "after_cleanup"},
    {"url": "http://127.0.0.1:9/f", "stage": 5},
    {"url": "http://127.0.0.1:9/f", "stage": "s" * 65},
    {"url": "http://127.0.0.1:9/" + "x" * 2030, "stage": "after_cleanup"},
    {"url": "x" * 2049, "stage": "after_cleanup"},
    dict(LOCAL_FILTER, timeout_ms=True),
    dict(LOCAL_FILTER, timeout_ms="900"),
    dict(LOCAL_FILTER, timeout_ms=900.0),
    dict(LOCAL_FILTER, timeout_ms=99),
    dict(LOCAL_FILTER, timeout_ms=5001),
    dict(LOCAL_FILTER, timeout_ms=5000),
    dict(LOCAL_FILTER, timeout_ms=-1),
    dict(LOCAL_FILTER, modes=["action"]),
]:
    REQUESTS.append(post("/v1/filters", body))
REQUESTS += [
    post("/v1/filters", raw='{"url": "http://127.0.0.1:9/\\udc00", "stage": "after_cleanup"}'),
    post("/v1/filters", raw="[]"),
    scenario(leases="clear"),
]
REQUESTS += [post("/v1/filters", LOCAL_FILTER) for _ in range(11)]
REQUESTS += [
    scenario(leases="clear", peer="192.168.1.10"),
    post("/v1/filters", LOCAL_FILTER),
    post("/v1/filters", raw="not json"),
    post("/v1/filters/flt_0000000000000003/renew"),
    scenario(peer="::1"),
    post("/v1/filters", LOCAL_FILTER),
    scenario(peer="::ffff:127.0.0.1"),
    post("/v1/filters", LOCAL_FILTER),
    scenario(peer="127.8.9.10"),
    post("/v1/filters", LOCAL_FILTER),
    scenario(peer="testclient"),
    post("/v1/filters", LOCAL_FILTER),
    scenario(app="tailscale", peer="100.65.148.53", leases="clear"),
    post("/v1/filters", LOCAL_FILTER, headers={"Host": "100.82.22.3"}),
    get("/v1/live/state", headers={"Host": "100.82.22.3"}),
    scenario(peer="192.168.1.10"),
    get("/v1/live/state", headers={"Host": "100.82.22.3"}),
    scenario(app="main", peer="127.0.0.1", settings={}),
    post("/v1/filters", {"stage": "x"}),
]

# --- Event streams: checks before streaming ----------------------------------------------------
REQUESTS += [
    get("/v1/events", query=[["types", "recording.started,not.a.type"]]),
    get("/v1/events", query=[["types", "events.gap"]]),
    get("/v1/events", query=[["types", "x" * 2001]]),
    get("/v1/events", headers={"Last-Event-ID": "evt_1"}),
    get("/v1/events", headers={"Last-Event-ID": "-1"}),
    get("/v1/events", headers={"Last-Event-ID": "1.5"}),
    get("/v1/events", headers={"Last-Event-ID": "9" * 19}),
    get("/v1/events", headers={"Last-Event-ID": "9" * 65}),
    get("/v1/events", headers={"X-OpenWhisper-Client": "c" * 65}),
    get("/v1/events", query=[["since", "-1"]]),
    get("/v1/events", query=[["since", "abc"]]),
    get("/v1/events", query=[["since", "1.5"]]),
    get("/v1/events", query=[["since", ""]]),
    get("/v1/events", query=[["types", "nope"]], headers={"Last-Event-ID": "x"}),
    scenario(settings={"api_live_events": False}),
    get("/v1/events"),
    get("/v1/events", query=[["types", "nope"]]),
    scenario(settings={"api_live_events": "yes"}),
    get("/v1/events"),
    scenario(settings={}),
]

# --- Event streams: what a stream sends before its live events --------------------------------
_started = ["recording.started", {"job_id": "j", "mode": "dictation", "action_id": None,
                                  "source": "hotkey"}]
_stopped = ["recording.stopped", {"job_id": "j", "duration_s": 1.5}]
_saved = ["history.saved", {"job_id": "j", "record_id": "1", "kind": "dictation"}]
_text = ["transcription.completed",
         {"job_id": "j", "text": "two\nlines   café \U0001f600 \"quoted\" </script>"}]
_preview = ["transcription.preview", {"job_id": "j", "text": "hel", "final": False}, False]
REQUESTS += [
    get("/v1/events"),
    scenario(publish=[_started, _stopped, _preview, _saved, _text]),
    get("/v1/events"),
    get("/v1/events", query=[["since", "0"]]),
    get("/v1/events", query=[["since", " 2 "]]),
    get("/v1/events", query=[["since", "2"], ["since", "3"]]),
    get("/v1/events", query=[["since", "1"]], headers={"Last-Event-ID": " 3 "}),
    get("/v1/events", query=[["since", "0"]], headers={"Last-Event-ID": ""}),
    get("/v1/events", query=[["since", "0"], ["types", "recording.started, history.saved"]]),
    get("/v1/events", query=[["since", "0"], ["types", " , "]]),
    get("/v1/events", query=[["since", "0"], ["types", "transcription.preview"]]),
    get("/v1/events", query=[["since", "5"]]),
    get("/v1/events", query=[["since", "900"]]),
    get("/v1/events", headers={"Last-Event-ID": "900"}),
    get("/v1/events", headers={"Last-Event-ID": "9" * 18}),
    get("/v1/events", query=[["since", "0"]], headers={"X-OpenWhisper-Client": "claude-channel"}),
    scenario(bus={"buffer_size": 3, "seq_base": 0}),
    scenario(publish=[["recording.started", {"job_id": str(n)}] for n in range(6)]),
    get("/v1/events", query=[["since", "1"]]),
    get("/v1/events", query=[["since", "0"]]),
    get("/v1/events", query=[["since", "3"]]),
    get("/v1/events", query=[["since", "1"], ["types", "history.saved"]]),
    scenario(bus={"buffer_size": 500, "seq_base": 100}),
    scenario(publish=[["recording.started", {"job_id": "new0"}],
                      ["recording.started", {"job_id": "new1"}]]),
    get("/v1/events", headers={"Last-Event-ID": "3"}),
    get("/v1/events", query=[["since", "100"]]),
    get("/v1/events", query=[["since", "101"]]),
    scenario(bus={"buffer_size": 500, "seq_base": 0}),
]

# --- The listener around the live routes -------------------------------------------------------
REQUESTS += [
    call("POST", "/v1/actions"),
    call("GET", "/v1/actions/a1/run"),
    call("PUT", "/v1/filters/l1"),
    call("GET", "/v1/filters"),
    call("DELETE", "/v1/notify"),
    call("GET", "/v1/recording/stop"),
    call("HEAD", "/v1/notify"),
    get("/v1/actions/"),
    get("/v1/live/state/"),
    post("/v1/notify/", {"text": "hi"}),
    get("/v1/actions/a1"),
    get("/v1/filters/a/b/renew"),
    get("/openapi.json"),
    scenario(enabled=False),
    get("/v1/live/state"),
    get("/v1/events"),
    post("/v1/filters", LOCAL_FILTER),
    scenario(enabled=True),
]
