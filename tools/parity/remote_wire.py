"""Record OpenWhisper's remote-engine wire, both sides of every frame, for C++ replay.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/remote_wire.py --all
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/remote_wire.py --scenario remote_probe

OpenWhisper's SpeechHost (services/remote_asr/host.py), HistoryBroker and its client functions
(probe_host, pair_with_host, request_pairing, RemoteConnection, HistoryClient) run in this one
isolated process on 127.0.0.1 (port 0, discovery off, Tailscale and network discovery stubbed).
A tap on websockets' Protocol.send_frame logs every text, binary and close frame each side sends,
before its bytes reach the socket, so a frame is always logged before the peer can answer it.
Pings and pongs are left out (keepalive timing is not part of an exchange).

A scenario is tools/parity/transcripts/remote_<name>.py (one file per scenario) exporting

    build(tmp: Path) -> Context    remote_wire.Context(tmp, ...), the hosts' shared fixtures
    STEPS = [(name, fn), ...]       fn(context) drives clients and hosts; run in order
    NORMALIZE = [rule, ...]         volatile values (below)

and writes tests/fixtures/transcripts/remote_<name>.json:

    {"scenario", "source_commit", "python", "setup", "frames": [frame, ...], "normalize": [...]}

frame: {"step", "conn", "dir": "c2s"|"s2c", "kind": "text"|"binary"|"close"|"http", ...}
    text     "text": the frame
    binary   "header": the JSON header (4-byte big-endian length, header, payload), then
             "payload_hex" (64 KiB or less) or "payload_sha256" and "payload_len" (and
             "payload_fill", the byte, when the payload is one byte repeated); a frame that
             does not split that way has no "header" and its whole bytes are the payload
    close    "code", "reason" (an empty close frame is code 1005), "echo": true when the side
             had received a close first (the library's answer, not the application's close)
    http     c2s: "text" the upgrade request's path; s2c: "status", "body". Recorded only for a
             connection whose upgrade was answered with something other than 101.
"conn" numbers connections in the order they opened; frames are grouped by connection, each in
the order sent. "step" is the step during which the frame was sent.

Volatile values are replaced in the frames by placeholders "<kind#n>" (a JSON string, also where
the value was a number) and listed as {"path": "<frame index><JSON pointer>", "as": kind}. Kinds
token, device_id, nonce, commit, fingerprint, uuid and conn_id are bindings: one value is one
placeholder wherever it appears, so a replay can check that the token of "paired" is the one the
next "hello" sends. host_ms and iso_time get a placeholder per occurrence. A commit that is the
SHA-256 of "openwhisper-pair-commit-v1" and a recorded nonce names it: "of": "<nonce#n>". A JSON
string holding OpenWhisper's product name is listed as {"as": "product"} and left as is: a
FioriVoce host writes its own name there. NORMALIZE rules are {"pointer", "as"} with optional
"when": {top-level key: value}, "dir" and "keep" (literal values a step chose, left as they are);
they apply to text frames and binary headers.

"setup" lists the host's fixed answers (name, addresses, engine, models, runtime), every patch
the recorder applied after importing OpenWhisper (never in the checkout), and each step's client
and outcomes: what OpenWhisper's client function returned or raised, normalized the same way.
"""
from __future__ import annotations

import argparse
import copy
import dataclasses
import hashlib
import importlib.util
import json
import logging
import re
import shutil
import sys
import threading
import time
import warnings
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import ow_isolation  # noqa: E402

if __name__ == "__main__":
    # Scenario files `import remote_wire`: give them this module, not a second copy.
    sys.modules.setdefault("remote_wire", sys.modules[__name__])

SCENARIOS = TOOLS / "transcripts"
OUTPUT = ow_isolation.REPO / "tests" / "fixtures" / "transcripts"
PREFIX = "remote_"
LOOPBACK = "127.0.0.1"
PAYLOAD_HEX_LIMIT = 64 * 1024
COMMIT_DOMAIN = b"openwhisper-pair-commit-v1"
PRODUCT = "OpenWhisper"
#: How long a client waits for the host's close before sending its own: the host closes right
#: after its last frame, and a client that closes first would make the transcript depend on
#: which close crossed the network first.
CLIENT_CLOSE_WAIT_S = 0.5
STEP_SETTLE_S = 15.0

#: Placeholder kinds and the shape of the value each stands for (the text the value has on the
#: wire: a JSON string's content, or a number's digits).
SHAPES = {
    "token": r"[A-Za-z0-9_-]{43}",
    "device_id": r"[0-9a-f]{32}",
    "nonce": r"[0-9a-f]{64}",
    "commit": r"[0-9a-f]{64}",
    "fingerprint": r"[0-9A-F]{64}",
    "host_ms": r"[0-9]+\.[0-9]",
    "iso_time": r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?(\+00:00|Z)?",
    "uuid": r"[0-9a-f]{32}",
    "conn_id": r"[0-9a-f]{8}",
}
UNBOUND = {"host_ms", "iso_time"}


class RecordingError(RuntimeError):
    """A scenario did something the recorder refuses, or its exchange is not reproducible."""


# ---- the tap (no OpenWhisper or websockets needed) ----

class _Connection:
    def __init__(self, index: int, port: int, step: str):
        self.index = index
        self.port = port
        self.step = step
        self.sides: set[str] = set()
        self.frames: list[dict] = []
        self.http_status: int | None = None


class Tap:
    """Every frame both sides of each loopback connection send, in send order.

    A connection is known by the client's local port; ``register`` is called once per side.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._connections: list[_Connection] = []
        self.step = ""

    def register(self, port: int, side: str) -> _Connection:
        with self._lock:
            for connection in reversed(self._connections):
                if connection.port == port and side not in connection.sides:
                    connection.sides.add(side)
                    return connection
            connection = _Connection(len(self._connections), port, self.step)
            connection.sides.add(side)
            self._connections.append(connection)
            return connection

    @staticmethod
    def _dir(side: str) -> str:
        return "s2c" if side == "server" else "c2s"

    def frame(self, connection: _Connection, side: str, kind: str, data: bytes,
              echo: bool = False, close: tuple[int, str] | None = None) -> None:
        record = {"step": self.step, "dir": self._dir(side), "kind": kind}
        if kind == "text":
            record["text"] = data.decode("utf-8")
        elif kind == "binary":
            record.update(split_binary(data))
        elif kind == "close":
            code, reason = close if close is not None else (1005, "")
            record["code"] = int(code)
            record["reason"] = reason
            if echo:
                record["echo"] = True
        else:
            raise RecordingError(f"unknown frame kind {kind}")
        with self._lock:
            connection.frames.append(record)

    def http_request(self, connection: _Connection, path: str) -> None:
        with self._lock:
            connection.frames.append({"step": self.step, "dir": "c2s", "kind": "http",
                                      "text": path})

    def http_response(self, connection: _Connection, status: int, body: bytes) -> None:
        with self._lock:
            connection.http_status = status
            connection.frames.append({"step": self.step, "dir": "s2c", "kind": "http",
                                      "status": int(status),
                                      "body": body.decode("utf-8", "replace")})

    def records(self) -> list[dict]:
        """Frames grouped by connection; the HTTP request and the 101 of an upgrade that
        succeeded are left out."""
        out = []
        with self._lock:
            for connection in self._connections:
                failed = connection.http_status not in (None, 101)
                for frame in connection.frames:
                    if frame["kind"] == "http" and not failed:
                        continue
                    out.append({"step": frame["step"], "conn": connection.index,
                                **{k: v for k, v in frame.items() if k != "step"}})
        return out

    def connection_count(self) -> int:
        with self._lock:
            return len(self._connections)


def split_binary(data: bytes) -> dict:
    """A request frame as {"header", payload}; a frame that is not one keeps all its bytes."""
    data = bytes(data)
    header_text = None
    payload = data
    if len(data) >= 4:
        length = int.from_bytes(data[:4], "big")
        if 4 + length <= len(data):
            try:
                header_text = data[4:4 + length].decode("utf-8")
                payload = data[4 + length:]
            except UnicodeDecodeError:
                header_text = None
                payload = data
    record = {}
    if header_text is not None:
        record["header"] = header_text
    if len(payload) <= PAYLOAD_HEX_LIMIT:
        record["payload_hex"] = payload.hex()
    else:
        record["payload_sha256"] = hashlib.sha256(payload).hexdigest()
        record["payload_len"] = len(payload)
        if payload.count(payload[:1]) == len(payload):
            # One byte repeated (an oversized test payload): enough to send it again.
            record["payload_fill"] = payload[:1].hex()
    return record


# ---- normalization (no OpenWhisper needed) ----

def _pointer_escape(key: str) -> str:
    return key.replace("~", "~0").replace("/", "~1")


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
    if isinstance(parent, list):
        parent[int(parts[-1])] = new_value
    else:
        parent[parts[-1]] = new_value


def json_style(text: str, value) -> dict | None:
    """The json.dumps arguments that write ``value`` as exactly ``text``, or None."""
    for style in ({"separators": (",", ":")}, {}):
        if json.dumps(value, **style) == text:
            return style
    return None


def _strings(value, pointer: str = ""):
    if isinstance(value, str):
        yield pointer, value
    elif isinstance(value, dict):
        for key, item in value.items():
            yield from _strings(item, f"{pointer}/{_pointer_escape(str(key))}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from _strings(item, f"{pointer}/{index}")


def _wire_text(value) -> str:
    """The value as its shape is checked: a string's content, a number's repr."""
    return value if isinstance(value, str) else json.dumps(value)


class Normalizer:
    """Replaces volatile values by placeholders, the same value by the same placeholder."""

    def __init__(self, rules: list[dict]):
        for rule in rules:
            if rule.get("as") not in SHAPES:
                raise RecordingError(f"NORMALIZE rule {rule} names no known kind")
            if not str(rule.get("pointer", "")).startswith("/"):
                raise RecordingError(f"NORMALIZE rule {rule} has no JSON pointer")
        self.rules = rules
        self.values: dict[tuple[str, str], str] = {}
        self._counts: dict[str, int] = {}

    def placeholder(self, kind: str, value) -> str:
        text = _wire_text(value)
        if not re.fullmatch(SHAPES[kind], text):
            raise RecordingError(f"{text!r} does not have the shape of a {kind}")
        key = (kind, text)
        if kind not in UNBOUND and key in self.values:
            return self.values[key]
        self._counts[kind] = self._counts.get(kind, 0) + 1
        name = f"<{kind}#{self._counts[kind]}>"
        if kind not in UNBOUND:
            self.values[key] = name
        return name

    def _applies(self, rule: dict, frame: dict, document) -> bool:
        if "dir" in rule and rule["dir"] != frame["dir"]:
            return False
        for key, expected in rule.get("when", {}).items():
            if not isinstance(document, dict) or document.get(key) != expected:
                return False
        return True

    def frames(self, frames: list[dict]) -> tuple[list[dict], list[dict]]:
        """The frames with placeholders, and the normalize list."""
        out = copy.deepcopy(frames)
        entries = []
        for index, frame in enumerate(out):
            field = "text" if frame["kind"] == "text" else (
                "header" if frame["kind"] == "binary" and "header" in frame else None)
            if field is None:
                continue
            try:
                document = json.loads(frame[field])
            except ValueError:
                continue
            style = json_style(frame[field], document)
            replaced = []
            for rule in self.rules:
                if not self._applies(rule, frame, document):
                    continue
                try:
                    value = pointer_get(document, rule["pointer"])
                except (KeyError, IndexError, TypeError, ValueError):
                    continue
                if isinstance(value, bool) or not isinstance(value, (str, int, float)):
                    continue
                if isinstance(value, str) and value.startswith("<") and value.endswith(">"):
                    continue  # an earlier rule already replaced it
                if value in rule.get("keep", ()):
                    continue  # a fixed value a step chose, such as a token never issued
                replaced.append((rule["pointer"], rule["as"], value))
            for pointer, value in _strings(document):
                if PRODUCT in value:
                    entries.append({"path": f"{index}{pointer}", "as": "product"})
            if not replaced:
                continue
            if style is None:
                raise RecordingError(f"frame {index} is not json.dumps output; cannot "
                                     f"normalize it: {frame[field]!r}")
            for pointer, kind, value in replaced:
                pointer_set(document, pointer, self.placeholder(kind, value))
                entries.append({"path": f"{index}{pointer}", "as": kind})
            frame[field] = json.dumps(document, **style)
        self._link_commits(out, entries)
        return out, entries

    def _link_commits(self, frames: list[dict], entries: list[dict]) -> None:
        nonces = {name: text for (kind, text), name in self.values.items() if kind == "nonce"}
        commits = {name: text for (kind, text), name in self.values.items() if kind == "commit"}
        for entry in entries:
            if entry["as"] != "commit":
                continue
            index, pointer = _split_path(entry["path"])
            frame = frames[index]
            document = json.loads(frame["text" if frame["kind"] == "text" else "header"])
            name = pointer_get(document, pointer)
            for nonce_name, nonce in nonces.items():
                digest = hashlib.sha256(COMMIT_DOMAIN + bytes.fromhex(nonce)).hexdigest()
                if digest == commits[name]:
                    entry["of"] = nonce_name

    def value(self, value, ports=()):
        """Any structure with every recorded volatile string replaced by its placeholder, and
        each of the hosts' ``ports`` after 127.0.0.1 by "<port>"."""
        if isinstance(value, str):
            for (kind, text), name in self.values.items():
                if value == text:
                    return name
            # Messages name the address dialled; the host's port is new on every run. Other
            # loopback ports (a fixed MCP URL a step chose) stay as they are.
            return re.sub(r"127\.0\.0\.1:([0-9]+)(?![0-9])",
                          lambda m: "127.0.0.1:<port>" if int(m.group(1)) in ports
                          else m.group(0), value)
        if isinstance(value, list):
            return [self.value(item, ports) for item in value]
        if isinstance(value, dict):
            return {key: self.value(item, ports) for key, item in value.items()}
        return value


def _split_path(path: str) -> tuple[int, str]:
    slash = path.index("/")
    return int(path[:slash]), path[slash:]


# ---- outcomes ----

def plain(value):
    """A client function's result as JSON data."""
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        value = dataclasses.asdict(value)
    if isinstance(value, dict):
        return {str(k): plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return repr(value)


def raised(exc: BaseException) -> dict:
    out = {"type": type(exc).__name__, "message": str(exc)}
    for name in ("code", "retry_after_s", "retryable", "sent", "refused"):
        if hasattr(exc, name):
            out[name] = plain(getattr(exc, name))
    return out


# ---- the recording session (needs OpenWhisper) ----

TAP = Tap()
_INSTALLED = False


def install_tap(tap: Tap = TAP) -> None:
    """Wrap websockets so every connection of this process reports to ``tap``."""
    global _INSTALLED
    if _INSTALLED:
        return
    from websockets import client as ws_client
    from websockets import server as ws_server
    from websockets.frames import BINARY as OP_BINARY, CLOSE as OP_CLOSE, TEXT as OP_TEXT, Close
    from websockets.protocol import CLIENT, Protocol, State
    from websockets.sync import connection as sync_connection

    original_init = sync_connection.Connection.__init__

    def init(self, sock, protocol, *args, **kwargs):
        side = "client" if protocol.side is CLIENT else "server"
        port = sock.getsockname()[1] if side == "client" else sock.getpeername()[1]
        protocol._fv_conn = tap.register(port, side)
        protocol._fv_side = side
        _CONNECTIONS.append(self)
        original_init(self, sock, protocol, *args, **kwargs)

    original_send_frame = Protocol.send_frame

    def send_frame(self, frame):
        connection = getattr(self, "_fv_conn", None)
        if connection is not None and frame.opcode in (OP_TEXT, OP_BINARY, OP_CLOSE):
            if frame.opcode is OP_CLOSE:
                close = Close.parse(frame.data)
                tap.frame(connection, self._fv_side, "close", b"",
                          echo=self.close_rcvd is not None,
                          close=(int(close.code), close.reason))
            else:
                tap.frame(connection, self._fv_side,
                          "text" if frame.opcode is OP_TEXT else "binary", bytes(frame.data))
        original_send_frame(self, frame)

    original_request = ws_client.ClientProtocol.send_request

    def send_request(self, request):
        connection = getattr(self, "_fv_conn", None)
        if connection is not None:
            tap.http_request(connection, request.path)
        original_request(self, request)

    original_response = ws_server.ServerProtocol.send_response

    def send_response(self, response):
        connection = getattr(self, "_fv_conn", None)
        if connection is not None:
            tap.http_response(connection, response.status_code, bytes(response.body or b""))
        original_response(self, response)

    original_close = sync_connection.Connection.close

    def close(self, *args, **kwargs):
        if getattr(self.protocol, "_fv_side", "") == "client":
            deadline = time.monotonic() + CLIENT_CLOSE_WAIT_S
            while (time.monotonic() < deadline and self.protocol.close_rcvd is None
                   and self.protocol.state is State.OPEN):
                time.sleep(0.005)
        return original_close(self, *args, **kwargs)

    sync_connection.Connection.__init__ = init
    Protocol.send_frame = send_frame
    ws_client.ClientProtocol.send_request = send_request
    ws_server.ServerProtocol.send_response = send_response
    sync_connection.Connection.close = close
    _INSTALLED = True


_CONNECTIONS: list = []


def install_guards(setup_patches: list) -> None:
    """Keep the recorder on loopback: no real Tailscale, no network discovery, no other bind."""
    from services.remote_asr import discovery, tailscale
    from services.remote_asr.host import SpeechHost

    tailscale.status = lambda timeout=4.0: tailscale.TailscaleStatus("not_installed")
    tailscale.whois = lambda address, timeout=4.0: None
    discovery.find_host = lambda *args, **kwargs: None
    discovery.search = lambda *args, **kwargs: []
    original_start = SpeechHost.start

    def start(self, port=0, bind="0.0.0.0", discovery_port=None):
        if bind != LOOPBACK or discovery_port is not None:
            raise RecordingError(f"the recorder serves on {LOOPBACK} without discovery only "
                                 f"(asked for {bind}, discovery {discovery_port})")
        return original_start(self, port=port, bind=bind, discovery_port=None)

    SpeechHost.start = start
    setup_patches.extend([
        "tailscale.status reports not_installed and tailscale.whois finds nobody "
        "(tests/fakes/remote.py no_real_tailscale)",
        "discovery.find_host and discovery.search find nothing (no UDP leaves the process)",
        "SpeechHost.start serves on 127.0.0.1 only, without discovery",
    ])


HOST_NAME = "studio"
DEVICE_NAME = "laptop"
ADDRESSES = ["192.168.1.20", "100.101.102.103"]
PAIRING_CODE = "246810"


class Context:
    """What a scenario's steps use: the host's identity and device store, hosts on 127.0.0.1,
    clients, outcomes and patches, all reported in the transcript's setup."""

    def __init__(self, tmp: Path, **setup):
        from services.remote_asr.tls import ensure_host_identity
        from tests.fakes.remote import ListStore

        self.tmp = Path(tmp)
        self.identity = ensure_host_identity(str(self.tmp / "host"))
        self.store = ListStore()
        self.setup = {"host_name": HOST_NAME, "device_name": DEVICE_NAME,
                      "addresses": list(ADDRESSES), "pairing_code": PAIRING_CODE,
                      "fingerprint": self.identity.fingerprint, **setup,
                      "patches": [], "steps": []}
        self._step: dict | None = None
        self._hosts: list = []
        self._step_hosts: list = []
        self._restore: list = []
        self._threads: list = []
        self._cleanups: list = []
        self.values: dict = {}
        #: Every port a host of this scenario served on (new on every run).
        self.ports: set[int] = set()

    def cleanup(self, fn) -> None:
        """Run ``fn`` once the scenario ended, after its hosts stopped."""
        self._cleanups.append(fn)

    # -- steps --

    def begin(self, name: str) -> None:
        self._step = {"name": name, "client": "", "outcomes": []}
        self.setup["steps"].append(self._step)
        self._step_hosts = []

    def end(self) -> None:
        for thread in self._threads:
            thread.join(STEP_SETTLE_S)
            if thread.is_alive():
                raise RecordingError(f"step {self._step['name']}: a client thread never ended")
        self._threads = []
        for host in self._step_hosts:
            host.stop()
            self._hosts.remove(host)
        self._step_hosts = []
        settle(open_ok=True)
        for undo in reversed(self._restore):
            undo()
        self._restore = []

    def finish(self) -> None:
        for host in list(self._hosts):
            host.stop()
        self._hosts = []
        for fn in reversed(self._cleanups):
            fn()
        self._cleanups = []
        settle(open_ok=False)

    def client(self, description: str) -> None:
        """Which client this step drives: an OpenWhisper client function, or "raw" frames."""
        self._step["client"] = description

    def note(self, text: str) -> None:
        self._step.setdefault("notes", []).append(text)

    def patch(self, target, name: str, value, note: str) -> None:
        """Set ``target.name`` for this step only, and say so in the setup."""
        previous = getattr(target, name)
        setattr(target, name, value)
        self._restore.append(lambda: setattr(target, name, previous))
        self.setup["patches"].append({"step": self._step["name"], "patch": note})

    def call(self, label: str, fn, *args, **kwargs):
        """Run a client function and keep what it returned or raised."""
        try:
            result = fn(*args, **kwargs)
        except Exception as exc:  # noqa: BLE001 - the outcome is the point
            self._step["outcomes"].append({"call": label, "raised": raised(exc)})
            return exc
        self._step["outcomes"].append({"call": label, "returned": plain(result)})
        return result

    def outcome(self, label: str, value) -> None:
        self._step["outcomes"].append({"call": label, "returned": plain(value)})

    def background(self, fn, *args, **kwargs) -> threading.Thread:
        thread = threading.Thread(target=fn, args=args, kwargs=kwargs, daemon=True)
        self._threads.append(thread)
        thread.start()
        return thread

    @staticmethod
    def wait(predicate, what: str, timeout: float = 10.0) -> None:
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise RecordingError(f"timed out waiting for {what}")
            time.sleep(0.005)

    # -- hosts --

    def host(self, engine=None, keep: bool = False, **kwargs):
        """A started SpeechHost on 127.0.0.1 with this scenario's identity and devices."""
        from services.remote_asr.host import DeviceRegistry, SpeechHost
        from tests.fakes.remote import FakeEngine

        engine = engine if engine is not None else FakeEngine()
        kwargs.setdefault("engine_provider", lambda: engine)
        host = SpeechHost(registry=DeviceRegistry(self.store.load, self.store.save),
                          identity=self.identity, host_name=HOST_NAME,
                          addresses=lambda: list(ADDRESSES), **kwargs)
        host.start(port=0, bind=LOOPBACK)
        self.ports.add(host.port)
        self._hosts.append(host)
        if not keep:
            self._step_hosts.append(host)
        return host

    def open_pairing(self, host) -> str:
        host.open_pairing()
        # The code is random; a fixed one keeps the client's frame the same on every run.
        host._pairing.code = PAIRING_CODE
        return PAIRING_CODE

    def pair(self, host, label: str = "pair_with_host"):
        """Pair DEVICE_NAME with ``host`` by code, as a setup for later steps."""
        from services.remote_asr.client import pair_with_host

        code = self.open_pairing(host)
        return self.call(label, pair_with_host, LOOPBACK, host.port, code, DEVICE_NAME)

    # -- raw clients --

    def raw(self, host, path: str | None = None):
        """A websockets client opened exactly as OpenWhisper's client opens one."""
        from websockets.sync.client import connect

        from services.remote_asr import protocol
        from services.remote_asr.tls import client_context

        return connect(f"wss://{LOOPBACK}:{host.port}{path or protocol.PATH}",
                       ssl=client_context(), open_timeout=6.0, close_timeout=2,
                       max_size=protocol.MAX_REPLY_BYTES, compression=None,
                       ping_interval=None, ping_timeout=None)

    @staticmethod
    def drain(ws) -> list:
        """Every message until the host closes, then the close is answered."""
        from websockets.exceptions import ConnectionClosed

        messages = []
        while True:
            try:
                messages.append(ws.recv(timeout=STEP_SETTLE_S))
            except ConnectionClosed:
                break
        ws.close()
        return messages


def settle(open_ok: bool) -> None:
    """Wait for every connection that is closing to finish (its reader thread ends once the
    TCP connection is gone); at the end of a scenario every connection must have closed."""
    from websockets.protocol import State

    for connection in list(_CONNECTIONS):
        if open_ok and connection.protocol.state is State.OPEN:
            continue
        connection.recv_events_thread.join(STEP_SETTLE_S)
        if connection.recv_events_thread.is_alive():
            raise RecordingError("a connection never finished closing")
    if not open_ok:
        _CONNECTIONS.clear()


def load_scenario(name: str):
    path = SCENARIOS / f"{name}.py"
    if not name.startswith(PREFIX) or not path.is_file():
        raise SystemExit(f"No scenario tools/parity/transcripts/{name}.py.")
    spec = importlib.util.spec_from_file_location(f"wire_{name}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def all_scenarios() -> list[str]:
    return sorted(p.stem for p in SCENARIOS.glob(f"{PREFIX}*.py"))


def record(name: str, scenario, commit: str, guards: list) -> Path:
    tmp = ow_isolation.SCRATCH / "remote_wire" / name
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    tap = TAP
    tap.__init__()
    _CONNECTIONS.clear()
    context = scenario.build(tmp)
    context.setup["patches"][:0] = list(guards)
    for step_name, step in scenario.STEPS:
        tap.step = step_name
        context.begin(step_name)
        try:
            step(context)
        finally:
            context.end()
    tap.step = ""
    context.finish()
    normalizer = Normalizer(list(getattr(scenario, "NORMALIZE", [])))
    frames, normalize = normalizer.frames(tap.records())
    # The certificate is new on every run, whether or not a frame names it.
    normalizer.placeholder("fingerprint", context.identity.fingerprint)
    setup = normalizer.value(context.setup, context.ports)
    document = {
        "scenario": name,
        "source_commit": commit,
        "python": sys.version.split()[0],
        "setup": setup,
        "frames": frames,
        "normalize": normalize,
    }
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / f"{name}.json"
    with path.open("w", encoding="utf-8", newline="\n") as out:
        out.write(json.dumps(document, ensure_ascii=False, indent=1) + "\n")
    return path


def prepare(allow_other_commit: bool = False) -> tuple[str, list]:
    """Isolate, import OpenWhisper, install the guards and the tap. Returns the commit and the
    guards' descriptions."""
    checkout = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(checkout, allow_other_commit)
    data = ow_isolation.isolate()
    ow_isolation.after_import_guards(data)
    reference = ow_isolation.reference()
    if sys.version.split()[0] != reference["python"]:
        raise SystemExit(f"Python {sys.version.split()[0]} is not the pinned {reference['python']}.")
    # websockets 17 asks for connect() as a context manager; OpenWhisper's client does not use
    # one either, and the raw clients here close themselves.
    warnings.filterwarnings("ignore", message="connect\\(\\) must be used as a context manager")
    # Refused requests are logged as warnings with tracebacks; the transcript holds them.
    logging.disable(logging.WARNING)
    # A host stopped right after it started can close its socket under serve_forever's first
    # select() on Windows; the host has stopped either way.
    original_hook = threading.excepthook

    def excepthook(args):
        if (args.thread is not None and args.thread.name == "RemoteEngineHost"
                and issubclass(args.exc_type, OSError)):
            return
        original_hook(args)

    threading.excepthook = excepthook
    guards: list = []
    install_guards(guards)
    install_tap()
    return commit, guards


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario", action="append", default=[],
                        help="transcripts/remote_<name>.py to record (repeatable)")
    parser.add_argument("--all", action="store_true", help="every remote_* scenario")
    parser.add_argument("--allow-other-commit", action="store_true",
                        help="record although the checkout is not at the pinned commit")
    args = parser.parse_args(argv)
    names = all_scenarios() if args.all else args.scenario
    if not names:
        parser.error("name --scenario or --all")
    commit, guards = prepare(args.allow_other_commit)
    for name in names:
        path = record(name, load_scenario(name), commit, guards)
        count = len(json.loads(path.read_text(encoding="utf-8"))["frames"])
        print(f"{path.relative_to(ow_isolation.REPO)}: {count} frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
