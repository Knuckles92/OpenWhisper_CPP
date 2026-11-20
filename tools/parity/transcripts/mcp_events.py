"""MCP Events (services/agent_mcp/events_ext.py) on the in-app MCP listener.

events/list, events/subscribe and events/unsubscribe on both MCP wires, every refusal of a
subscription in the order the methods check them (switches, name, arguments, delivery, url,
secret, peer reachability, ttlMs, the 50 cap, verification), refreshes, unsubscribes, and the
`events` capability on initialize and server/discover while MCP Events is on.
tests/agentapi/test_mcp_events_protocol.cpp replays it against agentapi::McpApp::answer with
agentapi::McpEvents installed.

No request leaves the process. The callback's answer to a verification challenge is faked by
replacing events_ext._post (the one function that sends), and host names are looked up in
NAMES instead of DNS (socket.getaddrinfo is replaced while the scenario runs). Whether an
address belongs to this computer is still asked of the system, by binding it.

As in agent_mcp.py, the app is built twice (plain loopback, and with Tailscale's allowed hosts)
behind a switch that also applies POSTs to /__scenario__ (answered 204, never reaching the app):

    app       "main" (allowed_hosts=()) or "tailscale" (allowed_hosts=TAILSCALE_HOSTS)
    peer      the client address requests come from
    settings  the saved settings, replaced whole
    answer    what the callback answers a POST: "echo" (200 with the challenge), "wrong" (200
              with another one), "garbage" (200, not JSON), "number" (200, a number as the
              challenge), "timeout", "refused", "tls", or an HTTP status with the body "{}"
    clear     true: events_ext.clear_subscriptions()

GET /__calls__ (answered by the switch) gives the POSTs the fake callback received since the
last such GET, oldest first: [{"url", "connect_to", "type"}], type being the body's "type"
(a verification) or null.

Subscription ids hash the principal ("openwhisper-mcp-token"), which OpenWhisper_CPP spells after its
own name; the replay maps each recorded id to its own. refreshBefore depends on the time.
"""
from __future__ import annotations

import contextlib
import json
import shutil
import socket
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tests" / "fixtures" / "agentapi" / "history.db"
TOKEN = "mcp-events-token-" + "c" * 32
AUTH = f"Bearer {TOKEN}"
ACCEPT = "application/json, text/event-stream"
MODERN = "2026-07-28"
PROTOCOL_KEY = "io.modelcontextprotocol/protocolVersion"
CAPABILITIES_KEY = "io.modelcontextprotocol/clientCapabilities"
TAILSCALE_HOSTS = ["100.82.22.3", "jed.example.ts.net"]
PEER = "100.65.148.53"
NAMES = {
    "public.example": ["93.184.215.14"],
    "second.example": ["93.184.215.15", "93.184.215.14"],
    "rebind.example": ["127.0.0.1"],
    "mixed.example": ["93.184.215.14", "127.0.0.1"],
    "lan.example": ["192.168.1.20"],
    "tailnet.example": ["100.65.148.60"],
    "shared.example": ["100.64.0.9"],
    "service.example": ["100.100.100.100"],
    "v6.example": ["2606:2800:220:1:248:1893:25c8:1946"],
    "mapped.example": ["::ffff:127.0.0.1"],
    "ula.example": ["fd00::1"],
}
SECRET = "whsec_" + "QUJDREVGR0hJSktMTU5PUFFSU1RVVldY"  # 24 bytes
OTHER_SECRET = "whsec_" + "MDEyMzQ1Njc4OWFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6"  # 36 bytes
HOOK = "http://127.0.0.1:9/hook"

CONTEXT = {
    "token": TOKEN,
    "fixture": "tests/fixtures/agentapi/history.db",
    "database_name": "mcp history.db",
    "tailscale_hosts": TAILSCALE_HOSTS,
    "names": NAMES,
    "principal": "openwhisper-mcp-token",
    "initial": {"app": "main", "peer": "127.0.0.1", "answer": "echo",
                "settings": {"mcp_enabled": True, "mcp_events_enabled": True}},
}
NORMALIZE = [
    {"pointer": "/result/refreshBefore", "pattern": r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ"},
]


class _Callback:
    """Stands in for every callback URL: answers per `answer`, keeps what it was sent."""

    def __init__(self):
        self.answer = "echo"
        self.calls = []

    def post(self, url, body, headers, timeout_s, *, client=None, connect_to=None):
        from services.agent_mcp import events_ext

        payload = json.loads(body)
        self.calls.append({"url": url, "connect_to": connect_to,
                           "type": payload.get("type") if isinstance(payload, dict) else None})
        answer = self.answer
        failures = {"timeout": "timeout", "refused": "connection_refused", "tls": "tls_error"}
        if answer in failures:
            return events_ext._Reply(None, category=failures[answer])
        status, text = 200, "{}"
        if answer == "echo":
            text = json.dumps({"challenge": payload.get("challenge")})
        elif answer == "wrong":
            text = '{"challenge": "not it"}'
        elif answer == "garbage":
            text = "not json"
        elif answer == "number":
            text = '{"challenge": 5}'
        elif isinstance(answer, int):
            status = answer
        return events_ext._Reply(status, text.encode("utf-8"), events_ext._status_category(status),
                                 None)


def _fake_getaddrinfo(host, port, *args, **kwargs):
    host = host.decode("ascii") if isinstance(host, bytes) else host
    answers = NAMES.get(host)
    if not answers:
        raise socket.gaierror(socket.EAI_NONAME, "unknown name")
    infos = []
    for address in answers:
        if ":" in address:
            infos.append((socket.AF_INET6, socket.SOCK_STREAM, 6, "", (address, port, 0, 0)))
        else:
            infos.append((socket.AF_INET, socket.SOCK_STREAM, 6, "", (address, port)))
    return infos


class _Switch:
    def __init__(self, apps, state, settings, callback):
        self.apps = apps
        self.state = state
        self.settings = settings
        self.callback = callback

    async def _lifespan(self, receive, send):
        async with contextlib.AsyncExitStack() as stack:
            await receive()
            for app in self.apps.values():
                await stack.enter_async_context(app.router.lifespan_context(app))
            await send({"type": "lifespan.startup.complete"})
            await receive()
        await send({"type": "lifespan.shutdown.complete"})

    def _apply(self, change):
        from services.agent_mcp import events_ext

        for key in ("app", "peer"):
            if key in change:
                self.state[key] = change[key]
        if "settings" in change:
            self.settings.values.clear()
            self.settings.values.update(change["settings"])
        if "answer" in change:
            self.callback.answer = change["answer"]
        if change.get("clear"):
            events_ext.clear_subscriptions()

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
        if scope["path"] == "/__calls__":
            calls, self.callback.calls = self.callback.calls, []
            await Response(json.dumps(calls), media_type="application/json")(scope, receive, send)
            return
        scope = dict(scope, client=(self.state["peer"], 50000))
        await self.apps[self.state["app"]](scope, receive, send)


def build(tmp: Path):
    from config import config
    from services.agent_mcp import events_ext
    from services.agent_mcp.app import create_app
    from services.automation import events
    from tests.fakes.settings import InMemorySettings

    database = tmp / CONTEXT["database_name"]
    shutil.copyfile(FIXTURE, database)
    initial = CONTEXT["initial"]
    state = {key: initial[key] for key in ("app", "peer")}
    settings = InMemorySettings(dict(initial["settings"]))
    callback = _Callback()
    callback.answer = initial["answer"]

    subscriptions = Path(config.MCP_EVENT_SUBSCRIPTIONS_FILE)
    if subscriptions.exists():
        subscriptions.unlink()
    events_ext.stop_delivery()
    events_ext._registry_instance = None
    events_ext._verified.clear()
    events_ext._post = callback.post
    socket.getaddrinfo = _fake_getaddrinfo
    events.set_bus(events.EventBus())
    # Delivering, so deliveryStatus.active says what the switches allow.
    events_ext.start_delivery(settings=settings)

    apps = {
        "main": create_app(database, TOKEN, enabled=lambda: True, settings=settings),
        "tailscale": create_app(database, TOKEN, enabled=lambda: True, settings=settings,
                                allowed_hosts=tuple(TAILSCALE_HOSTS)),
    }
    return _Switch(apps, state, settings, callback)


# --- Requests ---------------------------------------------------------------------------------


def scenario(**change):
    return {"method": "POST", "path": "/__scenario__",
            "headers": {"Host": "127.0.0.1", "Content-Type": "application/json"},
            "body": json.dumps(change)}


CALLS = {"method": "GET", "path": "/__calls__", "headers": {"Host": "127.0.0.1"}}


def post(body, headers=None):
    sent = {"Host": "127.0.0.1", "Authorization": AUTH, "Accept": ACCEPT,
            "Content-Type": "application/json"}
    sent.update(headers or {})
    return {"method": "POST", "path": "/mcp", "headers": sent, "body": body}


def message(method, params=None, msg_id=1, *, omit_params=False):
    out = {"jsonrpc": "2.0", "id": msg_id, "method": method}
    if not omit_params:
        out["params"] = params
    return out


def legacy(method, params=None, msg_id=1, headers=None):
    if params is None:
        return post(json.dumps(message(method, omit_params=True, msg_id=msg_id)), headers)
    return post(json.dumps(message(method, params, msg_id)), headers)


def modern(method, params=None, msg_id=1, headers=None):
    params = dict(params or {})
    params["_meta"] = {PROTOCOL_KEY: MODERN, CAPABILITIES_KEY: {}}
    sent = {"MCP-Protocol-Version": MODERN, "Mcp-Method": method}
    sent.update(headers or {})
    return post(json.dumps(message(method, params, msg_id)), sent)


def raw_modern(method, params_text, msg_id=1):
    """A per-request-wire call whose params are given as JSON text (NaN, lone surrogates)."""
    meta = json.dumps({PROTOCOL_KEY: MODERN, CAPABILITIES_KEY: {}})
    body = ('{"jsonrpc":"2.0","id":%s,"method":"%s","params":%s}'
            % (json.dumps(msg_id), method, params_text.replace("@META@", meta)))
    return post(body, {"MCP-Protocol-Version": MODERN, "Mcp-Method": method})


def subscribe(name, url=HOOK, secret=SECRET, **extra):
    params = {"name": name, "arguments": extra.pop("arguments", {}),
              "delivery": {"mode": "webhook", "url": url, "secret": secret}, "cursor": None}
    params.update(extra)
    return params


def unsubscribe(name, url=HOOK, **extra):
    params = {"name": name, "arguments": extra.pop("arguments", {}),
              "delivery": {"mode": "webhook", "url": url}}
    params.update(extra)
    return params


INITIALIZE = {"protocolVersion": "2025-11-25", "capabilities": {},
              "clientInfo": {"name": "test", "version": "1"}}

REQUESTS = [scenario(**CONTEXT["initial"])]

# --- Advertising and listing --------------------------------------------------------------------
REQUESTS += [
    legacy("initialize", INITIALIZE, 1),
    modern("server/discover", None, 2),
    modern("events/list", None, 3),
    legacy("events/list", {}, 4),
    legacy("events/list", None, 5),
    legacy("events/list", {"cursor": "x", "extra": 1}, 6),
    modern("events/list", {"cursor": "x"}, 7),
    post(json.dumps({"jsonrpc": "2.0", "id": 8, "method": "events/list", "params": []})),
    post(json.dumps({"jsonrpc": "2.0", "id": 9, "method": "events/list", "params": 5})),
    legacy("events/list", {"_meta": {"progressToken": 1}}, 10),
    legacy("events/list", {"_meta": 5}, 11),
    legacy("events/list", {"_meta": None}, 14),
    legacy("events/list", {"_meta": []}, 15),
    legacy("events/list", {"_meta": {"progressToken": None}}, 16),
    legacy("events/list", {"_meta": {"progressToken": "p", "other": [1]}}, 17),
    legacy("events/list", {"_meta": {"progressToken": True}}, 18),
    legacy("events/list", {"_meta": {"progressToken": 1.5}}, 19),
    legacy("events/list", {"_meta": {"progressToken": 2.0}}, 1900),
    legacy("events/list", {"_meta": {"progressToken": [1]}}, 1901),
    legacy("events/list", {"_meta": {"progressToken": {}}}, 1902),
    legacy("events/subscribe", dict(subscribe("history.saved"), _meta=5), 1903),
    legacy("events/nope", {}, 12),
    modern("events/nope", None, 13),
]

# --- Subscribing and refreshing -------------------------------------------------------------------
REQUESTS += [
    modern("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": ["dictation"]}), 20),
    CALLS,
    # A refresh: the URL was verified, the argument order carries no meaning.
    modern("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": ["dictation"]}, ttlMs=1000), 21),
    legacy("events/subscribe", subscribe("recording.started",
                                         arguments={"modes": ["file", "action", "dictation"],
                                                    "action_id": "summarize"}), 22),
    legacy("events/subscribe", subscribe("recording.started",
                                         arguments={"action_id": "summarize",
                                                    "modes": ["dictation", "file", "action"]}),
           23),
    CALLS,
    legacy("events/subscribe", subscribe("history.saved", "http://localhost:8080/a?x=1#f"), 24),
    legacy("events/subscribe", subscribe("history.saved", "http://[::1]:8080/hook"), 25),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.2/hook"), 26),
    legacy("events/subscribe", subscribe("history.saved", "https://public.example/hook"), 27),
    legacy("events/subscribe", subscribe("history.saved", "HTTPS://Public.Example:8443/h"), 28),
    legacy("events/subscribe", subscribe("history.saved", "http://LOCALHOST/hook"), 29),
    legacy("events/subscribe", subscribe("history.saved", "http://[::ffff:127.0.0.1]/hook"), 30),
    CALLS,
    legacy("events/subscribe", subscribe("meeting.started", ttlMs=None), 31),
    legacy("events/subscribe", subscribe("meeting.started", ttlMs=10 ** 12), 32),
    legacy("events/subscribe", subscribe("meeting.started", ttlMs=90000.7), 33),
    legacy("events/subscribe", subscribe("meeting.started", ttlMs=60000), 34),
    legacy("events/subscribe", subscribe("meeting.started", ttlMs=1), 35),
    legacy("events/subscribe", subscribe("meeting.started", ttlMs=1e308), 36),
    legacy("events/subscribe", subscribe("meeting.started", secret=OTHER_SECRET), 37),
    legacy("events/subscribe", subscribe("action.ran", arguments=None), 38),
    legacy("events/subscribe", subscribe("action.ran", arguments={"action_id": "a" * 200}), 39),
    legacy("events/subscribe", subscribe("action.ran", arguments={"action_id": "caf\u00e9 \U0001f600"}),
           40),
    legacy("events/subscribe", {"name": "filter.failed",
                                "delivery": {"mode": "webhook", "url": HOOK, "secret": SECRET}},
           41),
    legacy("events/subscribe", {"name": "filter.failed", "extra": [1],
                                "delivery": {"mode": "webhook", "url": HOOK, "secret": SECRET,
                                             "more": True}}, 42),
    CALLS,
]
for index, name in enumerate(["recording.stopped", "recording.canceled", "transcription.preview",
                              "transcription.failed", "channel.message", "route.decided",
                              "meeting.ended", "meeting.segments", "meeting.voice_command"]):
    REQUESTS.append(modern("events/subscribe", subscribe(name), 50 + index))

# --- Refusals, in the order they are checked -------------------------------------------------------
REQUESTS += [
    # The name.
    legacy("events/subscribe", {}, 100),
    legacy("events/subscribe", None, 101),
    legacy("events/subscribe", subscribe(""), 102),
    legacy("events/subscribe", subscribe(5), 103),
    legacy("events/subscribe", subscribe(None), 104),
    legacy("events/subscribe", subscribe("history.deleted"), 105),
    legacy("events/subscribe", subscribe("events.gap"), 106),
    legacy("events/subscribe", subscribe("History.Saved"), 107),
    legacy("events/subscribe", dict(subscribe("history.deleted"), arguments=5), 108),
    # The arguments.
    legacy("events/subscribe", subscribe("history.saved", arguments=[]), 110),
    legacy("events/subscribe", subscribe("history.saved", arguments="x"), 111),
    legacy("events/subscribe", subscribe("history.saved", arguments={"action_id": "x"}), 112),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"k" * 60: 1}), 113),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"it's": 1}), 114),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"say \"hi\" it's": 1}), 115),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"caf\u00e9\n": 1}), 116),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": ["dictation"], "nope": 1}), 117),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"action_id": ""}), 118),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"action_id": "a" * 201}), 119),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"action_id": 5}), 120),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"action_id": None}), 121),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": []}), 122),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": ["dictation", "dictation"]}), 123),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": ["nope"]}), 124),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": "dictation"}), 125),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": [5]}), 126),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": None}), 127),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"modes": ["Dictation"]}), 128),
    legacy("events/subscribe", subscribe("transcription.completed",
                                         arguments={"action_id": 5, "modes": []}), 129),
    raw_modern("events/subscribe",
               '{"name":"action.ran","arguments":{"action_id":"a\\ud83d"},"delivery":'
               '{"mode":"webhook","url":"%s","secret":"%s"},"_meta":@META@}' % (HOOK, SECRET), 130),
    # The delivery.
    legacy("events/subscribe", {"name": "history.saved", "arguments": {}}, 140),
    legacy("events/subscribe", dict(subscribe("history.saved"), delivery=[]), 141),
    legacy("events/subscribe", dict(subscribe("history.saved"), delivery="webhook"), 142),
    legacy("events/subscribe", dict(subscribe("history.saved"),
                                    delivery={"mode": "poll", "url": HOOK, "secret": SECRET}), 143),
    legacy("events/subscribe", dict(subscribe("history.saved"),
                                    delivery={"mode": 5, "url": HOOK, "secret": SECRET}), 144),
    legacy("events/subscribe", dict(subscribe("history.saved"),
                                    delivery={"url": HOOK, "secret": SECRET}), 145),
    legacy("events/subscribe", dict(subscribe("history.saved"),
                                    delivery={"mode": "Webhook", "url": 5, "secret": 5}), 146),
    legacy("events/subscribe", dict(subscribe("history.saved", arguments={"x": 1}),
                                    delivery={"mode": "poll"}), 147),
    # The URL.
    legacy("events/subscribe", subscribe("history.saved", 5), 150),
    legacy("events/subscribe", subscribe("history.saved", None), 151),
    legacy("events/subscribe", subscribe("history.saved", ""), 152),
    legacy("events/subscribe", subscribe("history.saved", "ftp://127.0.0.1/hook"), 153),
    legacy("events/subscribe", subscribe("history.saved", "http://example.com/hook"), 154),
    legacy("events/subscribe", subscribe("history.saved", "http://192.168.1.20/hook"), 155),
    legacy("events/subscribe", subscribe("history.saved", "http://public.example/hook"), 156),
    legacy("events/subscribe", subscribe("history.saved", "https://user:pw@example.com/h"), 157),
    legacy("events/subscribe", subscribe("history.saved", "https://user@example.com/h"), 158),
    legacy("events/subscribe", subscribe("history.saved", "https://@example.com/h"), 159),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:99999/hook"), 160),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:8x/hook"), 161),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:/hook"), 162),
    legacy("events/subscribe", subscribe("history.saved", "https://exa mple.com/h"), 163),
    legacy("events/subscribe", subscribe("history.saved", "https:///nohost"), 164),
    legacy("events/subscribe", subscribe("history.saved", "https://[::1/hook"), 165),
    legacy("events/subscribe", subscribe("history.saved", "https://example.com/" + "a" * 2000),
           166),
    legacy("events/subscribe", subscribe("history.saved", "127.0.0.1/hook"), 167),
    legacy("events/subscribe", subscribe("history.saved", "https://\u00e9xample.com/h"), 168),
    legacy("events/subscribe", subscribe("history.saved", "https://example.com:0443/h", "x"), 169),
    raw_modern("events/subscribe",
               '{"name":"history.saved","arguments":{},"delivery":{"mode":"webhook",'
               '"url":"http://127.0.0.1/\\udc00","secret":"%s"},"_meta":@META@}' % SECRET, 170),
    # The secret.
    legacy("events/subscribe", subscribe("history.saved", secret=None), 180),
    legacy("events/subscribe", subscribe("history.saved", secret=5), 181),
    legacy("events/subscribe", subscribe("history.saved", secret="secret"), 182),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_"), 183),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 5 + "QQ=="),
           184),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 22), 185),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 21 + "QUI="),
           186),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 8), 187),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 8 + "QQ"),
           188),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 7 + "QU-_"),
           189),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 7 + " QUJD"),
           190),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 8 + "\n"),
           191),
    legacy("events/subscribe", subscribe("history.saved", secret="WHSEC_" + "QUJD" * 8), 192),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 8 + "===="),
           193),
    legacy("events/subscribe", subscribe("history.saved", secret="whsec_" + "QUJD" * 7 + "QU=D"),
           194),
    # The lifetime.
    legacy("events/subscribe", subscribe("history.saved", ttlMs=-5), 200),
    legacy("events/subscribe", subscribe("history.saved", ttlMs=0), 201),
    legacy("events/subscribe", subscribe("history.saved", ttlMs="5"), 202),
    legacy("events/subscribe", subscribe("history.saved", ttlMs=True), 203),
    legacy("events/subscribe", subscribe("history.saved", ttlMs=[]), 204),
    legacy("events/subscribe", subscribe("history.saved", ttlMs=-0.5), 205),
    legacy("events/subscribe",
           dict(subscribe("history.saved", secret="x"), ttlMs=-1), 206),
    post('{"jsonrpc":"2.0","id":207,"method":"events/subscribe","params":{"name":"history.saved",'
         '"arguments":{},"delivery":{"mode":"webhook","url":"%s","secret":"%s"},"ttlMs":NaN}}'
         % (HOOK, SECRET)),
    post('{"jsonrpc":"2.0","id":208,"method":"events/subscribe","params":{"name":"history.saved",'
         '"arguments":{},"delivery":{"mode":"webhook","url":"%s","secret":"%s"},"ttlMs":Infinity}}'
         % (HOOK, SECRET)),
    post('{"jsonrpc":"2.0","id":209,"method":"events/subscribe","params":{"name":"history.saved",'
         '"arguments":{},"delivery":{"mode":"webhook","url":"%s","secret":"%s"},'
         '"ttlMs":-Infinity}}' % (HOOK, SECRET)),
    CALLS,
]

# --- Verification -------------------------------------------------------------------------------
for msg_id, answer in enumerate(["wrong", "garbage", "number", 500, 503, 404, 400, 302, 413, 204,
                                 "timeout", "refused", "tls"], start=300):
    REQUESTS += [
        scenario(answer=answer),
        legacy("events/subscribe", subscribe("history.saved", f"http://127.0.0.1:9/v{msg_id}"),
               msg_id),
    ]
REQUESTS += [
    scenario(answer="echo"),
    CALLS,
    # A URL that failed was not remembered as verified.
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/v300"), 320),
    CALLS,
]

# --- A Tailscale peer -------------------------------------------------------------------------------
_peer_host = {"Host": "100.82.22.3"}
REQUESTS += [
    scenario(app="tailscale", peer=PEER),
    legacy("events/list", {}, 400, _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://public.example/peer"), 401,
           _peer_host),
    CALLS,
    legacy("events/subscribe", subscribe("history.saved", "https://public.example/peer"), 402,
           _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://second.example:8443/p"), 403,
           _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://v6.example/p"), 404, _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://tailnet.example/p"), 405,
           _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://100.65.148.60/p"), 406,
           _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://93.184.215.14/p"), 407,
           _peer_host),
    CALLS,
]
for msg_id, url in enumerate([HOOK, "https://127.0.0.1:8443/hook", "https://localhost/hook",
                              "https://[::1]/hook", "https://[::ffff:127.0.0.1]/hook",
                              "https://0.0.0.0/hook", "https://192.168.1.20/hook",
                              "https://10.1.2.3/hook", "https://169.254.169.254/latest",
                              "https://printer.local/hook", "https://nas.lan/hook",
                              "https://box.home.arpa/hook", "https://app.localhost/hook",
                              "https://rebind.example/hook", "https://mixed.example/hook",
                              "https://lan.example/hook", "https://shared.example/hook",
                              "https://service.example/hook", "https://mapped.example/hook",
                              "https://ula.example/hook", "https://100.100.100.100/hook",
                              "https://100.64.0.9/hook", "https://[fd7a:115c:a1e0::53]/hook",
                              "https://[fe80::1]/hook", "https://224.0.0.1/hook",
                              "https://255.255.255.255/hook", "https://[2001:db8::1]/hook"],
                             start=410):
    REQUESTS.append(legacy("events/subscribe", subscribe("history.saved", url), msg_id,
                           _peer_host))
REQUESTS += [
    legacy("events/subscribe", subscribe("history.saved", "https://unknown.example/h"), 450,
           _peer_host),
    # Checked after the secret, before the lifetime.
    legacy("events/subscribe", subscribe("history.saved", "https://10.1.2.3/hook", "x"), 451,
           _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://10.1.2.3/hook", ttlMs=-1), 452,
           _peer_host),
    legacy("events/subscribe", subscribe("history.saved", "https://unknown.example/h", ttlMs=-1),
           453, _peer_host),
    CALLS,
]
for msg_id, answer in enumerate(["wrong", 500, 404, "timeout", "refused", "tls"], start=460):
    REQUESTS += [
        scenario(answer=answer),
        legacy("events/subscribe", subscribe("history.saved", f"https://public.example/w{msg_id}"),
               msg_id, _peer_host),
    ]
REQUESTS += [
    scenario(answer="echo"),
    CALLS,
    # A peer unsubscribes like anyone.
    legacy("events/unsubscribe", unsubscribe("history.saved", "https://public.example/peer"), 470,
           _peer_host),
    # Allowed agents over Tailscale: the peer's subscription is active.
    scenario(settings={"mcp_enabled": True, "mcp_events_enabled": True,
                       "mcp_tailscale_enabled": True}),
    legacy("events/subscribe", subscribe("history.saved", "https://second.example:8443/p"), 471,
           _peer_host),
    modern("events/subscribe", subscribe("history.saved", "https://second.example:8443/p"), 472,
           _peer_host),
    scenario(app="main", peer="::1"),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/local6"), 473),
    scenario(peer="::ffff:127.0.0.1"),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/mapped"), 474),
    scenario(peer="127.8.9.10"),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/other127"), 475),
    scenario(peer="testclient"),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/named"), 476),
    scenario(peer="192.168.1.10"),
    legacy("events/subscribe", subscribe("history.saved", "https://public.example/lan"), 477),
    scenario(peer="127.0.0.1"),
    CALLS,
]

# --- Unsubscribing ----------------------------------------------------------------------------
REQUESTS += [
    modern("events/unsubscribe", unsubscribe("transcription.completed",
                                             arguments={"modes": ["dictation"]}), 500),
    modern("events/unsubscribe", unsubscribe("transcription.completed",
                                             arguments={"modes": ["dictation"]}), 501),
    legacy("events/unsubscribe", unsubscribe("recording.started",
                                             arguments={"modes": ["action", "file", "dictation"],
                                                        "action_id": "summarize"}), 502),
    legacy("events/unsubscribe", unsubscribe("history.saved", "https://never.example/x"), 503),
    legacy("events/unsubscribe", unsubscribe("history.saved", "not even a URL"), 504),
    legacy("events/unsubscribe", dict(unsubscribe("history.saved"),
                                      delivery={"mode": "webhook", "url": HOOK,
                                                "secret": "ignored"}), 505),
    legacy("events/unsubscribe", {}, 510),
    legacy("events/unsubscribe", unsubscribe("events.gap"), 511),
    legacy("events/unsubscribe", unsubscribe("history.saved", arguments={"modes": ["x"]}), 512),
    legacy("events/unsubscribe", {"name": "history.saved"}, 513),
    legacy("events/unsubscribe", dict(unsubscribe("history.saved"), delivery={"mode": "poll"}),
           514),
    legacy("events/unsubscribe", dict(unsubscribe("history.saved"),
                                      delivery={"mode": "webhook"}), 515),
    legacy("events/unsubscribe", unsubscribe("history.saved", ""), 516),
    legacy("events/unsubscribe", unsubscribe("history.saved", 5), 517),
    raw_modern("events/unsubscribe",
               '{"name":"history.saved","arguments":{},"delivery":{"mode":"webhook",'
               '"url":"http://127.0.0.1/\\udc00"},"_meta":@META@}', 518),
    CALLS,
]

# --- The cap of 50, counted before verification and only for a new id ----------------------------
REQUESTS += [scenario(clear=True, answer="echo")]
for n in range(50):
    REQUESTS.append(legacy("events/subscribe",
                           subscribe("history.saved", f"http://127.0.0.1:9/cap/{n}"), 600 + n))
REQUESTS += [
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/cap/50"), 650),
    scenario(answer="wrong"),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/cap/51"), 651),
    # Refreshing is not adding; the URL is still verified.
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/cap/7"), 652),
    # Checked after the lifetime.
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/cap/52", ttlMs=0),
           653),
    scenario(answer="echo"),
    legacy("events/unsubscribe", unsubscribe("history.saved", "http://127.0.0.1:9/cap/3"), 654),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/cap/53"), 655),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/cap/54"), 656),
    CALLS,
    scenario(clear=True),
]

# --- Switched off, and live events not allowed ---------------------------------------------------
REQUESTS += [
    scenario(settings={"mcp_enabled": True, "mcp_events_enabled": False}),
    legacy("initialize", INITIALIZE, 700),
    modern("server/discover", None, 701),
    modern("events/list", None, 702),
    legacy("events/subscribe", subscribe("history.saved"), 703),
    legacy("events/unsubscribe", unsubscribe("history.saved"), 704),
    legacy("events/subscribe", {}, 705),
    scenario(settings={"mcp_enabled": True}),
    legacy("events/list", {}, 706),
    scenario(settings={"mcp_enabled": True, "mcp_events_enabled": "yes"}),
    legacy("events/list", {}, 707),
    modern("server/discover", None, 708),
    scenario(settings={"mcp_enabled": True, "mcp_events_enabled": True, "api_live_events": False}),
    legacy("initialize", INITIALIZE, 710),
    modern("events/list", None, 711),
    legacy("events/subscribe", subscribe("history.saved"), 712),
    legacy("events/unsubscribe", unsubscribe("history.saved"), 713),
    scenario(settings={"mcp_enabled": True, "mcp_events_enabled": False, "api_live_events": False}),
    legacy("events/list", {}, 714),
    scenario(settings={"mcp_enabled": False, "mcp_events_enabled": True}),
    legacy("events/subscribe", subscribe("history.saved", "http://127.0.0.1:9/mcp-off"), 715),
    scenario(settings={"mcp_enabled": True, "mcp_events_enabled": True, "api_live_events": "no"}),
    legacy("events/list", {}, 716),
    scenario(settings={"mcp_enabled": True, "mcp_events_enabled": True}),
    CALLS,
]
