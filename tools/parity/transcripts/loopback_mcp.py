"""Record OpenWhisper's loopback MCP server (meeting/agent/installed/mcp_server.py) for C++ replay.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/transcripts/loopback_mcp.py

The server is a ThreadingHTTPServer, not a FastAPI app, so this scenario runs on its own rather
than through Starlette's TestClient: it starts LoopbackMcpServer on 127.0.0.1 inside this
isolated process, sends every request below with http.client (a new connection each), and
writes tests/fixtures/transcripts/loopback_mcp.json:

    {"scenario", "source_commit", "normalize", "tools", "calls",
     "exchanges": [{"request": {method, path, headers, body | body_base64},
                    "response": {status, headers, body}} | {"action": ...}]}

Paths and headers name the endpoints as {token} and {closed_token}; the port, the tokens and
the Date and Server headers are in "normalize". An exchange with "action" changes the server
before the next request: "deactivate" sets {closed_token}'s endpoint inactive, "close" closes it.
"calls" are (name, arguments, meta) as the tool handler received them, in order.
"""
from __future__ import annotations

import base64
import http.client
import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import golden_format  # noqa: E402
import ow_isolation  # noqa: E402

OUT = ow_isolation.REPO / "tests" / "fixtures" / "transcripts" / "loopback_mcp.json"
MAX = 4 * 1024 * 1024

ENDPOINT_TOOLS = [
    {"name": "patch_state", "description": "Change the state.",
     "inputSchema": {"type": "object", "properties": {"ops": {"type": "array"}},
                     "required": ["ops"]}},
    {"name": "explode", "description": "Fails.", "inputSchema": {"type": "object"}},
    {"name": "ask_question", "inputSchema": {"type": "object", "properties": {
        "text": {"type": "string"}}}},
]
PATCH = {"name": "patch_state", "arguments": {"ops": [
    {"op": "set_topic", "text": "Beta \u00e9\u65e5", "evidence": ["sg_1"]}]}}


def _rpc(method, msg_id=None, params=None):
    message = {"jsonrpc": "2.0"}
    if msg_id is not None:
        message["id"] = msg_id
    message["method"] = method
    if params is not None:
        message["params"] = params
    return message


def _json(value):
    return json.dumps(value).encode("utf-8")


def _requests():
    """(method, path, headers, body bytes or None, declared Content-Length or None)."""
    auth = {"Authorization": "Bearer {token}", "Content-Type": "application/json"}
    closed = {"Authorization": "Bearer {closed_token}", "Content-Type": "application/json"}
    post = "/mcp/{token}"
    return [
        ("POST", post, auth, _json(_rpc("initialize", 1, {"protocolVersion": "2025-06-18"}))),
        ("POST", post, auth, _json(_rpc("initialize", 2, {"protocolVersion": "1999-01-01"}))),
        ("POST", post, auth, _json(_rpc("initialize", "three", {"protocolVersion": "2024-11-05"}))),
        ("POST", post, auth, _json(_rpc("initialize", 4))),
        ("POST", post, auth, _json(_rpc("notifications/initialized"))),
        ("POST", post, auth, _json(_rpc("tools/list", 5))),
        ("POST", post, auth, _json(_rpc("tools/call", 6, dict(PATCH, _meta={"k": "v"})))),
        ("POST", post, auth, _json(_rpc("tools/call", 7, {"name": "Bash", "arguments": {}}))),
        ("POST", post, auth, _json(_rpc("tools/call", 8, {"name": "explode", "arguments": {}}))),
        ("POST", post, auth, _json(_rpc("tools/call", 9, {"name": "ask_question",
                                                          "arguments": [1], "_meta": "x"}))),
        ("POST", post, auth, _json(_rpc("tools/call", 10, {"arguments": {}}))),
        ("POST", post, auth, _json(_rpc("server/discover", 11))),
        ("POST", post, auth, _json(_rpc("ping", 12))),
        ("POST", post, auth, _json({"jsonrpc": "2.0", "id": None, "method": "ping"})),
        ("POST", post, auth, _json({"jsonrpc": "2.0", "id": 13})),
        ("POST", post, auth, _json([_rpc("ping", 14), _rpc("notifications/initialized"),
                                    _rpc("tools/list", 15)])),
        ("POST", post, auth, _json([_rpc("notifications/initialized"),
                                    _rpc("notifications/cancelled")])),
        ("POST", post, auth, _json([])),
        ("POST", post, auth, _json(5)),
        ("POST", post, auth, _json([1, _rpc("ping", 16)])),
        ("POST", post, auth, b"{"),
        ("POST", post, auth, b""),
        ("POST", post, auth, b"\xef\xbb\xbf" + _json(_rpc("ping", 17))),
        ("POST", post, auth, b'{"jsonrpc": "2.0", "id": 18, "method": "\xff"}'),
        ("POST", post, auth, b'{"jsonrpc": "2.0", "id": 19, "method": "ping", "x": NaN}'),
        ("POST", post, auth, None, MAX + 1),
        ("GET", post, {"Authorization": "Bearer {token}"}, None),
        ("DELETE", post, {"Authorization": "Bearer {token}"}, None),
        ("PUT", post, auth, _json(_rpc("ping", 20))),
        ("POST", "/mcp/nope", dict(auth, Authorization="Bearer nope"),
         _json(_rpc("tools/list", 21))),
        ("POST", post, {"Content-Type": "application/json"}, _json(_rpc("tools/list", 22))),
        ("POST", post, dict(auth, Authorization="Bearer {token} "), _json(_rpc("tools/list", 23))),
        ("POST", post, dict(auth, Authorization="bearer {token}"), _json(_rpc("tools/list", 24))),
        ("POST", post, dict(auth, Origin="https://evil.example"), _json(_rpc("tools/list", 25))),
        ("POST", post, dict(auth, Origin="null"), _json(_rpc("ping", 26))),
        ("POST", post, dict(auth, Origin=""), _json(_rpc("ping", 27))),
        ("POST", "/mcp", auth, _json(_rpc("ping", 28))),
        ("POST", "/other/{token}", auth, _json(_rpc("ping", 29))),
        ("POST", "/mcp/{token}/extra", auth, _json(_rpc("ping", 30))),
        ("POST", "/mcp/{token}/", auth, _json(_rpc("ping", 31))),
        ("POST", "//mcp/{token}", auth, _json(_rpc("ping", 32))),
        ("POST", "/mcp/{token}?x=1", auth, _json(_rpc("ping", 33))),
        ("GET", "/", {}, None),
        ("POST", "/mcp/{closed_token}", closed, _json(_rpc("tools/list", 34))),
        {"action": "deactivate"},
        ("POST", "/mcp/{closed_token}", closed, _json(_rpc("tools/call", 35, PATCH))),
        ("POST", "/mcp/{closed_token}", closed, _json(_rpc("tools/list", 36))),
        {"action": "close"},
        ("POST", "/mcp/{closed_token}", closed, _json(_rpc("tools/call", 37, PATCH))),
    ]


def _text_or_base64(data: bytes) -> dict:
    try:
        return {"body": data.decode("utf-8")}
    except UnicodeDecodeError:
        return {"body_base64": base64.b64encode(data).decode("ascii")}


def _send(port, tokens, method, path, headers, body, declared=None):
    def fill(text):
        return text.replace("{token}", tokens["token"]).replace("{closed_token}",
                                                               tokens["closed_token"])

    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        conn.putrequest(method, fill(path), skip_accept_encoding=True)
        sent = dict(headers)
        if declared is not None:
            sent["Content-Length"] = str(declared)
        elif body is not None:
            sent["Content-Length"] = str(len(body))
        for name, value in sent.items():
            conn.putheader(name, fill(value))
        conn.endheaders()
        if body is not None:
            conn.send(body)
        response = conn.getresponse()
        data = response.read()
        record_headers = []
        for name, value in response.getheaders():
            if name.lower() in ("date", "server"):
                value = f"<{name.lower()}>"
            record_headers.append([name, value])
        request = {"method": method, "path": path, "headers": [[k, v] for k, v in sent.items()]}
        if body is not None:
            request.update(_text_or_base64(body))
        return {"request": request,
                "response": dict({"status": response.status, "headers": record_headers},
                                 **_text_or_base64(data))}
    finally:
        conn.close()


def main() -> int:
    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    from meeting.agent.installed.mcp_server import LoopbackMcpServer

    ow_isolation.after_import_guards(data)

    calls = []

    def handler(name, args, meta):
        calls.append([name, args, meta])
        if name == "explode":
            raise RuntimeError("tool broke \u00e9")
        return json.dumps({"results": [{"ok": True}]}), False

    server = LoopbackMcpServer()
    server.start()
    try:
        endpoint = server.open_endpoint(ENDPOINT_TOOLS, handler)
        closed = server.open_endpoint(ENDPOINT_TOOLS, handler)
        tokens = {"token": endpoint.token, "closed_token": closed.token}
        exchanges = []
        for item in _requests():
            if isinstance(item, dict):
                if item["action"] == "deactivate":
                    closed.active = False
                else:
                    server.close_endpoint(closed)
                exchanges.append(item)
                continue
            exchanges.append(_send(server.port, tokens, *item))
        listed = endpoint.listed.is_set()
    finally:
        server.stop()
    record = {
        "scenario": "loopback_mcp",
        "source_commit": commit,
        "normalize": ["port", "{token}", "{closed_token}", "/response/headers/Date",
                      "/response/headers/Server"],
        "tools": ENDPOINT_TOOLS,
        "listed": listed,
        "calls": calls,
        "exchanges": exchanges,
    }
    golden_format.write(OUT, record)
    print(f"{OUT.name}: {len(exchanges)} exchanges, {len(calls)} tool calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
