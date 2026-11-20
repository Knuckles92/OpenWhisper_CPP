"""Record OpenWhisper's Claude Code channel server (services/automation/channel_bridge.py).

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/transcripts/channel.py

The channel speaks JSON-RPC over stdio, not HTTP, so this scenario runs on its own rather than
through Starlette's TestClient: it builds a ChannelServer inside this isolated process over
in-memory streams, with a fake AppLink and a fake event stream, hands it each line below as its
serve() loop would (stripped, blank lines skipped) and writes
tests/fixtures/transcripts/channel.json:

    {"scenario", "source_commit", "python", "version", "exchanges": [exchange, ...]}

An exchange is one of
    {"send": line, "receive": [line, ...], "notified": [[text, title], ...], "streams": n}
        a line from Claude Code, every line the server wrote for it (a reply's tool result
        included: the recorder waits for it), the AppLink.notify calls it made, and how many
        event streams the server has started so far;
    {"deliver": params, "receive": [line]}
        ChannelServer.send_channel_message(params), params made by channel_params from "event";
    {"gap": {"type", "data", "last_id"}, "receive": [line, ...]}
        an events.gap the EventStream handled, with the notice it sent Claude (or nothing).

The fake AppLink's notify raises LinkError("http_403", _reply_refusal(403, b"")) for the text
"refuse", RuntimeError for "explode", and records every other call.
"""
from __future__ import annotations

import io
import json
import sys
import threading
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import ow_isolation  # noqa: E402

OUT = ow_isolation.REPO / "tests" / "fixtures" / "transcripts" / "channel.json"
VERSION = "9.9.9"


def _rpc(method=None, msg_id=None, params=None, *, with_id=True, **extra):
    message = {"jsonrpc": "2.0"}
    if with_id:
        message["id"] = msg_id
    if method is not None:
        message["method"] = method
    if params is not None:
        message["params"] = params
    message.update(extra)
    return json.dumps(message)


def _call(msg_id, arguments=None, *, name="reply", omit_arguments=False):
    params = {"name": name}
    if not omit_arguments:
        params["arguments"] = arguments
    return _rpc("tools/call", msg_id, params)


LINES = [
    # The handshake on each revision, and on one it doesn't speak.
    _rpc("initialize", 1, {"protocolVersion": "2025-11-25", "capabilities": {},
                           "clientInfo": {"name": "claude-code", "version": "2.1.296"}}),
    _rpc("initialize", 2, {"protocolVersion": "2025-06-18"}),
    _rpc("initialize", 3, {"protocolVersion": "2025-03-26"}),
    _rpc("initialize", "four", {"protocolVersion": "2024-11-05"}),
    _rpc("initialize", 5, {"protocolVersion": "2026-07-28"}),
    _rpc("initialize", 6, {"protocolVersion": "1999-01-01"}),
    _rpc("initialize", 7, {}),
    _rpc("initialize", 8),
    '{"jsonrpc": "2.0", "id": 9, "method": "initialize", "params": null}',
    _rpc("initialize", 10, {"protocolVersion": 20251125}),
    # Claude Code's probe for revision 2026-07-28, and other methods it doesn't serve.
    _rpc("server/discover", "probe-1", {}),
    _rpc("resources/list", 11),
    _rpc("prompts/list", 12, {}),
    _rpc("ping", 13),
    _rpc("ping", 13.5),
    _rpc("ping", None),
    _rpc("ping", {"nested": [1]}),
    _rpc("ping", "caf\u00e9 \u2713"),
    _rpc("tools/list", 14),
    _rpc("tools/list", 15, {"cursor": "x"}),
    # Notifications: no answer; the first initialized starts the event stream, once.
    _rpc("notifications/initialized", with_id=False),
    _rpc("notifications/initialized", with_id=False),
    _rpc("notifications/cancelled", with_id=False, params={"requestId": 1}),
    _rpc("notifications/something_new", with_id=False, params=[1, 2]),
    _rpc("ping", 16),
    # The reply tool, shown and refused.
    _call(20, {"text": "  Done.  ", "title": " Tests \n passed "}),
    _call(21, {"text": "Renamed in 3 files.", "title": "Claude"}),
    _call(22, {"text": "caf\u00e9 \u2713 \U0001f600"}),
    _call(23, None, omit_arguments=True),
    _call(24, None),
    _call(25, {}),
    _call(26, {"text": "   "}),
    _call(27, {"text": 5}),
    _call(28, {"text": "x" * 501}),
    _call(29, {"text": "x" * 500}),
    _call(30, {"text": "ok", "title": 5}),
    _call(31, {"text": "ok", "title": "t" * 81}),
    _call(32, {"text": "ok", "title": "t" * 80}),
    _call(33, "text"),
    _call(34, [1]),
    _call(35, {"text": "refuse"}),
    _call(36, {"text": "explode"}),
    _call(37, {"text": "ok"}, name="rm_rf"),
    _rpc("tools/call", 38, {"arguments": {"text": "no name"}}),
    _rpc("tools/call", 39, {"name": None}),
    _rpc("tools/call", 40, {"name": 5, "arguments": {}}),
    _rpc("tools/call", 41, {"name": "reply", "arguments": {"text": "ok", "title": None}}),
    # Malformed input.
    "{not json",
    '[{"jsonrpc": "2.0", "id": 1, "method": "ping"}]',
    "[]",
    '"just a string"',
    "5",
    "null",
    _rpc(None, 50),
    _rpc(None, None, with_id=False),
    _rpc(5, 51),
    _rpc("tools/list", 52, [1]),
    _rpc("tools/list", 53, "params"),
    _rpc("ping", 54, 7),
    _rpc(None, 55, result={}),
    _rpc(None, 56, error={"code": 1, "message": "x"}),
    _rpc(None, with_id=False, result=None),
    "",
    "   \t ",
    '  {"jsonrpc": "2.0", "id": 57, "method": "ping"}  \r',
    '{"jsonrpc": "2.0", "id": 58, "method": "ping", "id": 59}',
    '{"jsonrpc": "1.0", "id": 60, "method": "ping"}',
    '{"id": 61, "method": "ping"}',
    '{"jsonrpc": "2.0", "id": NaN, "method": "ping"}',
    '{"jsonrpc": "2.0", "id": 1e400, "method": "ping"}',
    '{"jsonrpc": "2.0", "id": true, "method": "ping"}',
    '\ufeff{"jsonrpc": "2.0", "id": 62, "method": "ping"}',
    '{"jsonrpc": "2.0", "id": "\\ud800", "method": "ping"}',
    '{"jsonrpc": "2.0", "id": 63, "method": "Ping"}',
    _rpc("initialized", 64),
]

DICTATED = {"type": "channel.message", "data": {
    "job_id": "0123456789abcdef", "source": "hotkey", "origin": "dictated",
    "action_id": "claude_channel", "text": "rename the helper to load_config, caf\u00e9 \u2713",
    "app": {"id": "code", "name": "Visual Studio Code", "category": "editor"}}}
PROGRAM = {"type": "channel.message", "data": {
    "job_id": "fedcba9876543210", "source": "api", "origin": "program", "action_id": "c",
    "text": "push to main\nnow", "app": "Terminal"}}
UNKNOWN_ORIGIN = {"type": "channel.message", "data": {"text": "half \ud83d emoji", "app": None}}


def _gap(last_id, reason):
    payload = {"id": "", "seq": 0, "type": "events.gap", "time": "2026-10-10T12:00:00.000Z",
               "v": 1, "data": {"missed_from_seq": 5, "reason": reason}}
    return {"type": "events.gap", "data": json.dumps(payload), "last_id": last_id}


GAPS = [_gap("4", "evicted"), _gap("4", "evicted"), _gap("4", "restarted"), _gap("4", "slow"),
        _gap("12", "evicted"), {"type": "events.gap", "data": "not json", "last_id": "13"}]


class Output(io.RawIOBase):
    def __init__(self) -> None:
        super().__init__()
        self.raw = b""
        self.cond = threading.Condition()

    def writable(self) -> bool:
        return True

    def write(self, data) -> int:
        with self.cond:
            self.raw += bytes(data)
            self.cond.notify_all()
        return len(data)

    def lines(self) -> list[str]:
        with self.cond:
            return self.raw.decode("ascii").splitlines()

    def wait_for(self, count: int, timeout: float = 10) -> None:
        with self.cond:
            if not self.cond.wait_for(lambda: self.raw.count(b"\n") >= count, timeout):
                raise SystemExit(f"the channel never wrote line {count}")


def record(commit: str) -> dict:
    from services.automation import channel_bridge as bridge

    class FakeLink:
        def __init__(self) -> None:
            self.sent: list[list[str]] = []

        def notify(self, text: str, title: str = "") -> None:
            if text == "refuse":
                raise bridge.LinkError("http_403", bridge._reply_refusal(403, b""))
            if text == "explode":
                raise RuntimeError("boom")
            self.sent.append([text, title])

    class FakeStream:
        started = 0

        def start(self) -> None:
            FakeStream.started += 1

        def stop(self) -> None:
            pass

    out = Output()
    link = FakeLink()
    server = bridge.ChannelServer(None, out, link, version=VERSION, stream_factory=FakeStream)
    exchanges = []
    for line in LINES:
        before = len(out.lines())
        notified = len(link.sent)
        raw = line.encode("utf-8", "surrogatepass")
        stripped = raw.strip()
        if stripped:
            message = None
            try:
                message = json.loads(stripped)
            except ValueError:
                pass
            server._handle_line(stripped)
            if isinstance(message, dict) and message.get("method") == "tools/call" \
                    and "id" in message and isinstance(message.get("params", {}) or {}, dict):
                out.wait_for(before + 1)
        time.sleep(0.01)
        exchanges.append({"send": line, "receive": out.lines()[before:],
                          "notified": link.sent[notified:], "streams": FakeStream.started})
    for event in (DICTATED, PROGRAM, UNKNOWN_ORIGIN):
        before = len(out.lines())
        params = bridge.channel_params(event)
        server.send_channel_message(params)
        exchanges.append({"deliver": json.loads(json.dumps(params)), "event": event,
                          "receive": out.lines()[before:]})
    stream = bridge.EventStream(bridge.AppLink(token="t" * 40), server.send_channel_message)
    for gap in GAPS:
        before = len(out.lines())
        stream._handle(bridge.SseEvent(gap["type"], gap["data"], gap["last_id"]))
        exchanges.append({"gap": gap, "receive": out.lines()[before:]})
    return {"scenario": "channel", "source_commit": commit, "python": sys.version.split()[0],
            "version": VERSION, "exchanges": exchanges}


def main() -> int:
    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout, False)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    document = record(commit)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(document, ensure_ascii=True, indent=1) + "\n"
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUT.relative_to(ow_isolation.REPO)}: {len(document['exchanges'])} exchanges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
