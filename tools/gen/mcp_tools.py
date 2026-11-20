"""Write src/agentapi/data/mcp_tools.json: the in-app MCP server's identity, instructions and tools.

    $env:OPENWHISPER_CPP_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/gen/mcp_tools.py [--check]

OpenWhisper's MCP server (services/agent_mcp/app.py) is built under tools/parity/ow_isolation.py
over an empty database in out/parity and asked, through Starlette's TestClient (no socket), for
its initialize handshake and tools/list on the 2025-11-25 wire. The file keeps what they answer:

    {"server": {"name", "version"}, "instructions": str,
     "tools": [{"annotations", "description", "inputSchema", "name", "outputSchema"}, ...]}

Tools are in registration order, with their descriptions (the tool functions' docstrings) and
schemas as the SDK publishes them, keys in the order this wire sends them; the 2026-07-28 wire
sends the same tools with inputSchema's "type" first and outputSchema's keys sorted
(agentapi::McpServer derives that). "OpenWhisper" becomes "OpenWhisper_CPP" everywhere, and
agentapi puts identity().product in its place again when it loads the file. --check compares
with the committed file instead of writing it and fails on a difference.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "parity"))
import ow_isolation  # noqa: E402

OUTPUT = ROOT / "src" / "agentapi" / "data" / "mcp_tools.json"
OPENWHISPER, OPENWHISPER_CPP = "OpenWhisper", "OpenWhisper_CPP"
TOKEN = "g" * 40


def _post(client, method, params, msg_id):
    body = {"jsonrpc": "2.0", "id": msg_id, "method": method, "params": params}
    response = client.post("/mcp", json=body, headers={
        "Authorization": f"Bearer {TOKEN}",
        "Accept": "application/json, text/event-stream",
    })
    if response.status_code != 200:
        raise SystemExit(f"{method} answered {response.status_code}: {response.text}")
    return response.json()["result"]


def document() -> str:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    from fastapi.testclient import TestClient
    from services.agent_mcp.app import create_app
    from services.database import DatabaseManager
    from tests.fakes.settings import InMemorySettings

    work = ow_isolation.SCRATCH / "mcp-tools"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    database = work / "history.db"
    DatabaseManager(str(database)).close()
    app = create_app(database, TOKEN, enabled=lambda: True,
                     settings=InMemorySettings({"mcp_enabled": True}))
    with TestClient(app, base_url="http://127.0.0.1") as client:
        handshake = _post(client, "initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "generator", "version": "1"}}, 1)
        tools = _post(client, "tools/list", {}, 2)["tools"]
    server = handshake["serverInfo"]
    if server.get("name") != OPENWHISPER:
        raise SystemExit(f"The server is named {server.get('name')!r}; update this script.")
    document = {
        "server": {"name": OPENWHISPER_CPP, "version": server["version"]},
        "instructions": handshake["instructions"].replace(OPENWHISPER, OPENWHISPER_CPP),
        "tools": json.loads(json.dumps(tools).replace(OPENWHISPER, OPENWHISPER_CPP)),
    }
    if len(tools) != 22:
        raise SystemExit(f"{len(tools)} tools, not 22; review the new ones before regenerating.")
    text = json.dumps(document, indent=1, ensure_ascii=False) + "\n"
    if OPENWHISPER in text:
        raise SystemExit(f"{OPENWHISPER} is still in the document; decide how to rebrand it.")
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed file instead of writing it")
    args = parser.parse_args(argv)
    text = document()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date; run tools/gen/mcp_tools.py")
            return 1
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("w", encoding="utf-8", newline="\n") as out:
        out.write(text)
    print(f"{OUTPUT.relative_to(ROOT)}: {len(text)} characters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
