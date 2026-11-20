"""Record HTTP transcripts of OpenWhisper's FastAPI apps, for C++ replay against pure handlers.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/record_transcripts.py --scenario agent_api
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/record_transcripts.py --all

A scenario is tools/parity/transcripts/<name>.py (one file per scenario, so packages never share
one) exporting

    build(tmp: Path) -> app     the ASGI app, built in this process against fixtures copied
                                into tmp (a fresh folder under out/parity/transcripts/<name>)
    REQUESTS = [request, ...]
    NORMALIZE = [...]           optional: values that differ from run to run (below)
    CONTEXT = {...}             optional: what a replay needs to rebuild the app (token, fixture)

A request is a dict:

    method   "GET" (default), "POST", ...
    path     the raw path, percent-encoded as it goes on the wire ("/v1/transcriptions/a%20b")
    query    [[name, value], ...] or {name: value}; a value may be
             {"$from": [label or index, "/json/pointer"]}: that value of an earlier response body
             (an index counts exchanges; a label names the first exchange of a request), or a
             callable, called once OpenWhisper can be imported, giving the text to send
    headers  {name: value} sent with the client's own (httpx adds host, accept, user-agent ...);
             a value of None leaves that header out
    body     request body text
    label    a name later requests use in "$from"
    paginate true: send again with cursor=<next_cursor> until the answer has none

Requests go through Starlette's TestClient (no socket; exceptions answered as the server would,
redirects not followed). The transcript, tests/fixtures/transcripts/<name>.json, records what the
app received (method, decoded path, raw path, query pairs as FastAPI parses them, headers in
order, body) and what it answered (status, headers in order, body text), with "refs" naming the
query values taken from earlier answers so a replay can take them from its own.

NORMALIZE entries: {"pointer": "/json/pointer", "pattern": "regex"} replaces that response value
with "<normalized>" wherever it matches (and in later requests that reuse it); a plain string
names a response header whose value is replaced. The replay checks the pattern and substitutes
its own value.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import re
import shutil
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ow_isolation  # noqa: E402

SCENARIOS = TOOLS / "transcripts"
OUTPUT = ow_isolation.REPO / "tests" / "fixtures" / "transcripts"
NORMALIZED = "<normalized>"


def load_scenario(name: str):
    path = SCENARIOS / f"{name}.py"
    if not path.is_file():
        raise SystemExit(f"No scenario {path.relative_to(ow_isolation.REPO)}.")
    spec = importlib.util.spec_from_file_location(f"transcript_{name}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Scenario files in the same folder that other recorders run: remote_*.py are remote_wire.py's,
# loopback_mcp.py, meeting_ws.py and channel.py record themselves.
OTHER_RECORDERS = ("remote_", "loopback_mcp.py", "meeting_ws.py", "channel.py")


def all_scenarios() -> list[str]:
    return sorted(p.stem for p in SCENARIOS.glob("*.py")
                  if not p.name.startswith(("_",) + OTHER_RECORDERS))


def pointer_get(document, pointer: str):
    value = document
    for part in pointer.split("/")[1:]:
        part = part.replace("~1", "/").replace("~0", "~")
        value = value[int(part)] if isinstance(value, list) else value[part]
    return value


def pointer_set(document, pointer: str, new_value) -> None:
    parts = [p.replace("~1", "/").replace("~0", "~") for p in pointer.split("/")[1:]]
    parent = document
    for part in parts[:-1]:
        parent = parent[int(part)] if isinstance(parent, list) else parent[part]
    last = parts[-1]
    if isinstance(parent, list):
        parent[int(last)] = new_value
    else:
        parent[last] = new_value


class Capture:
    """ASGI wrapper keeping the last HTTP scope the app received."""

    def __init__(self, app):
        self.app = app
        self.scope = None

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http":
            self.scope = scope
        await self.app(scope, receive, send)


def compact(value) -> str:
    # Starlette's JSONResponse rendering.
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


class Recorder:
    def __init__(self, client, capture, normalize):
        self.client = client
        self.capture = capture
        self.normalize = normalize
        self.exchanges = []
        self.raw_bodies = []
        self.labels = {}

    def resolve(self, value, refs, name):
        if callable(value):
            return value()
        if isinstance(value, dict) and "$from" in value:
            where, pointer = value["$from"]
            index = self.labels[where] if isinstance(where, str) else where
            if index < 0:
                index += len(self.exchanges)
            refs[name] = [index, pointer]
            return pointer_get(json.loads(self.raw_bodies[index]), pointer)
        return value

    def send(self, request: dict) -> None:
        from starlette.datastructures import QueryParams

        method = request.get("method", "GET")
        query = request.get("query", [])
        if isinstance(query, dict):
            query = list(query.items())
        refs = {}
        sent_query = [(name, self.resolve(value, refs, name)) for name, value in query]
        headers = {k: v for k, v in request.get("headers", {}).items() if v is not None}
        if "label" in request:
            self.labels[request["label"]] = len(self.exchanges)
        response = self.client.request(method, request["path"], params=sent_query or None,
                                       headers=headers, content=request.get("body", "") or None)
        scope = self.capture.scope
        received = {
            "method": scope["method"],
            "path": scope["path"],
            "raw_path": scope["raw_path"].decode("latin-1"),
            "query": [list(pair) for pair in QueryParams(scope["query_string"]).multi_items()],
            "headers": [[k.decode("latin-1"), v.decode("latin-1")] for k, v in scope["headers"]],
            "body": request.get("body", ""),
        }
        body = response.content.decode("utf-8")
        answered = {
            "status": response.status_code,
            "headers": [[k.lower(), v] for k, v in response.headers.multi_items()],
            "body": body,
        }
        exchange = {"request": received, "response": answered}
        if refs:
            exchange["refs"] = {"query": refs}
        if "note" in request:
            exchange["note"] = request["note"]
        self.exchanges.append(exchange)
        self.raw_bodies.append(body)
        if request.get("paginate"):
            while True:
                try:
                    cursor = json.loads(body).get("next_cursor")
                except (ValueError, AttributeError):
                    cursor = None
                if not cursor:
                    return
                following = {k: v for k, v in request.items() if k not in ("label", "paginate")}
                following["query"] = [(n, v) for n, v in query if n != "cursor"] + [
                    ("cursor", {"$from": [len(self.exchanges) - 1, "/next_cursor"]})]
                self.send(following)
                body = self.raw_bodies[-1]

    def finish(self) -> list[dict]:
        rules = [r for r in self.normalize if isinstance(r, dict)]
        header_names = {r.lower() for r in self.normalize if isinstance(r, str)}
        replaced = set()
        for index, exchange in enumerate(self.exchanges):
            response = exchange["response"]
            for pair in response["headers"]:
                if pair[0] in header_names:
                    pair[1] = NORMALIZED
            if not rules:
                continue
            try:
                document = json.loads(response["body"])
            except ValueError:
                continue
            if compact(document) != response["body"]:
                continue
            changed = []
            for rule in rules:
                try:
                    value = pointer_get(document, rule["pointer"])
                except (KeyError, IndexError, TypeError, ValueError):
                    continue
                if isinstance(value, str) and re.fullmatch(rule["pattern"], value):
                    pointer_set(document, rule["pointer"], NORMALIZED)
                    changed.append(rule["pointer"])
                    replaced.add((index, rule["pointer"]))
            if changed:
                response["body"] = compact(document)
                exchange["normalized"] = changed
        for exchange in self.exchanges:
            for name, (index, pointer) in exchange.get("refs", {}).get("query", {}).items():
                if (index, pointer) in replaced:
                    exchange["request"]["query"] = [
                        [n, NORMALIZED if n == name else v] for n, v in exchange["request"]["query"]]
        return self.exchanges


def record(name: str, scenario, commit: str) -> Path:
    from fastapi.testclient import TestClient

    tmp = ow_isolation.SCRATCH / "transcripts" / name
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    capture = Capture(scenario.build(tmp))
    client = TestClient(capture, base_url="http://127.0.0.1", raise_server_exceptions=False,
                        follow_redirects=False)
    normalize = list(getattr(scenario, "NORMALIZE", []))
    with client:
        recorder = Recorder(client, capture, normalize)
        for request in scenario.REQUESTS:
            recorder.send(request)
        exchanges = recorder.finish()
    document = {
        "scenario": name,
        "source_commit": commit,
        "python": sys.version.split()[0],
        "context": getattr(scenario, "CONTEXT", {}),
        "normalize": normalize,
        "exchanges": exchanges,
    }
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / f"{name}.json"
    with path.open("w", encoding="utf-8", newline="\n") as out:
        out.write(json.dumps(document, ensure_ascii=False, indent=1) + "\n")
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario", action="append", default=[],
                        help="transcripts/<name>.py to record (repeatable)")
    parser.add_argument("--all", action="store_true", help="every scenario")
    parser.add_argument("--allow-other-commit", action="store_true",
                        help="record although the checkout is not at the pinned commit")
    args = parser.parse_args(argv)
    names = all_scenarios() if args.all else args.scenario
    if not names:
        parser.error("name --scenario or --all")

    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout, args.allow_other_commit)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    reference = ow_isolation.reference()
    if sys.version.split()[0] != reference["python"]:
        raise SystemExit(f"Python {sys.version.split()[0]} is not the pinned {reference['python']}.")
    for name in names:
        path = record(name, load_scenario(name), commit)
        count = len(json.loads(path.read_text(encoding="utf-8"))["exchanges"])
        print(f"{path.relative_to(ow_isolation.REPO)}: {count} exchanges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
