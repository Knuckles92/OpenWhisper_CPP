"""OpenWhisper's remote-engine host or client as a live peer, for interop tests on 127.0.0.1.

    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/remote_peer.py --role host --port 0 --data <dir>
    & $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/remote_peer.py --role client --data <dir> `
        --host 127.0.0.1 --port <port> --code <digits>

Runs under ow_isolation.py with every writable OpenWhisper path inside --data (a folder the
caller owns, outside the checkout and %LOCALAPPDATA%\\OpenWhisper), the in-memory credential
store, and a `tailscale` stub first on PATH (it exits 1, "Tailscale isn't running"), with
tailscale.status and tailscale.whois stubbed as tests/fakes/remote.py does. Network discovery is
stubbed out; nothing binds anything but 127.0.0.1.

Every answer is one JSON line on stdout. Commands are JSON lines on stdin; the peer stops and
exits 0 at end of input, leaving nothing running.

host   serves tests/fakes/remote.py FakeEngine (or --unavailable) as --name with pairing open, and
       prints {"port", "fingerprint", "code"} first. Commands:
         {"cmd": "open_pairing"}            -> {"code"}
         {"cmd": "pending"}                 -> {"pending": request or null}
         {"cmd": "answer", "allow": bool}   -> {"answered": bool}
         {"cmd": "devices"}                 -> {"devices": [...]} (no token digests)
         {"cmd": "clients"}                 -> {"clients": [...]}
         {"cmd": "remove", "device_id": id} -> {"removed": bool}
client pairs --device-name with the host by --code (or asks for approval with --request,
       printing {"event": "sas", "sas"} when both screens show the digits), prints
       {"event": "paired", ...} or {"event": "error", "type", "message"}, then takes:
         {"op": "probe"}                                        -> {"probe": ...}
         {"op": "connect", "reports_recording": bool}           -> {"ready": ...}
         {"op": "request", "name": op, "seconds": s, ...fields} -> {"result": ...}
                (seconds: that much of tests/fakes/remote.py's tone as the audio)
         {"op": "close"}                                        -> {"closed": true}
       A failing command prints {"error": {"type", "message", ...}} and the peer goes on.
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import sys
import threading
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
import ow_isolation  # noqa: E402

LOOPBACK = "127.0.0.1"
_OUT = threading.Lock()


def say(message: dict) -> None:
    with _OUT:
        sys.stdout.write(json.dumps(message) + "\n")
        sys.stdout.flush()


def plain(value):
    import dataclasses

    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        value = dataclasses.asdict(value)
    if isinstance(value, dict):
        return {str(k): plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return repr(value)


def failure(exc: BaseException) -> dict:
    out = {"type": type(exc).__name__, "message": str(exc)}
    for name in ("code", "retry_after_s", "sent", "refused"):
        if hasattr(exc, name):
            out[name] = plain(getattr(exc, name))
    return out


def tailscale_stub(folder: Path) -> Path:
    """A `tailscale` that is never running, first on PATH, so the owner's is never asked."""
    folder.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        (folder / "tailscale.cmd").write_text(
            "@echo off\r\necho Tailscale isn't running 1>&2\r\nexit /b 1\r\n", encoding="ascii")
    else:
        script = folder / "tailscale"
        script.write_text("#!/bin/sh\necho \"Tailscale isn't running\" >&2\nexit 1\n",
                          encoding="ascii")
        script.chmod(0o755)
    os.environ["PATH"] = str(folder) + os.pathsep + os.environ.get("PATH", "")
    return folder


def isolate(data: Path, allow_other_commit: bool) -> None:
    checkout = ow_isolation.checkout()
    ow_isolation.check_reference_commit(checkout, allow_other_commit)
    # isolate() refuses a folder inside the checkout or the owner's OpenWhisper data before
    # anything is written there, the stub included.
    root = ow_isolation.isolate(data)
    tailscale_stub(data / "bin")
    ow_isolation.after_import_guards(root)
    logging.disable(logging.WARNING)
    from services.remote_asr import discovery, tailscale

    tailscale.status = lambda timeout=4.0: tailscale.TailscaleStatus("not_installed")
    tailscale.whois = lambda address, timeout=4.0: None
    discovery.find_host = lambda *args, **kwargs: None
    discovery.search = lambda *args, **kwargs: []


def commands():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            command = json.loads(line)
        except ValueError:
            say({"error": {"type": "ValueError", "message": "not a JSON line"}})
            continue
        if isinstance(command, dict):
            yield command
        else:
            say({"error": {"type": "ValueError", "message": "not a JSON object"}})


def run_host(args) -> int:
    from services.remote_asr.host import DeviceRegistry, SpeechHost
    from services.remote_asr.tls import ensure_host_identity
    from tests.fakes.remote import FakeEngine, ListStore

    engine = FakeEngine(available=not args.unavailable)
    store = ListStore()
    host = SpeechHost(engine_provider=lambda: engine,
                      registry=DeviceRegistry(store.load, store.save),
                      identity=ensure_host_identity(str(Path(args.data) / "host")),
                      host_name=args.name)
    port = host.start(port=args.port, bind=LOOPBACK)
    try:
        say({"port": port, "fingerprint": host.identity.fingerprint,
             "code": host.open_pairing()})
        for command in commands():
            name = command.get("cmd")
            if name == "open_pairing":
                say({"code": host.open_pairing()})
            elif name == "pending":
                say({"pending": host.pending_request()})
            elif name == "answer":
                pending = host.pending_request()
                say({"answered": bool(pending) and host.answer_request(
                    pending["id"], command.get("allow") is True)})
            elif name == "devices":
                say({"devices": host.registry.list()})
            elif name == "clients":
                say({"clients": host.connected_clients()})
            elif name == "remove":
                say({"removed": host.remove_device(str(command.get("device_id") or ""))})
            else:
                say({"error": {"type": "ValueError", "message": f"unknown cmd {name!r}"}})
    finally:
        host.stop()
    return 0


def run_client(args) -> int:
    from services.remote_asr.client import (RemoteConnection, pair_with_host, probe_host,
                                            request_pairing)
    from tests.fakes.remote import _tone

    try:
        if args.request:
            result = request_pairing(args.host, args.port, args.device_name,
                                     on_code=lambda sas: say({"event": "sas", "sas": sas}))
        else:
            result = pair_with_host(args.host, args.port, args.code, args.device_name)
    except Exception as exc:  # noqa: BLE001 - reported to the caller
        say({"event": "error", **failure(exc)})
        result = None
    else:
        say({"event": "paired", **plain(result)})
    connection = None
    try:
        for command in commands():
            op = command.get("op")
            try:
                if op == "probe":
                    say({"probe": plain(probe_host(args.host, args.port))})
                elif op == "connect":
                    if result is None:
                        raise RuntimeError("Not paired.")
                    if connection is not None:
                        connection.close()
                    connection = RemoteConnection(
                        args.host, args.port, result.token, result.fingerprint,
                        alternates=result.alternates,
                        reports_recording=command.get("reports_recording") is True)
                    say({"ready": connection.connect()})
                elif op == "request":
                    if connection is None:
                        raise RuntimeError("Not connected.")
                    fields = {k: v for k, v in command.items()
                              if k not in ("op", "name", "seconds")}
                    seconds = command.get("seconds")
                    audio = _tone(int(float(seconds) * 16000)) if seconds is not None else None
                    say({"result": connection.request(str(command.get("name")), audio=audio,
                                                      **fields)})
                elif op == "close":
                    if connection is not None:
                        connection.close()
                        connection = None
                    say({"closed": True})
                else:
                    raise ValueError(f"unknown op {op!r}")
            except Exception as exc:  # noqa: BLE001 - reported to the caller
                say({"error": failure(exc)})
    finally:
        if connection is not None:
            connection.close()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--role", choices=("host", "client"), required=True)
    parser.add_argument("--port", type=int, default=0,
                        help="host: the port to serve on (0 picks one); client: the host's")
    parser.add_argument("--data", required=True, help="the folder OpenWhisper writes into")
    parser.add_argument("--host", default=LOOPBACK, help="client: the host's address")
    parser.add_argument("--code", default="", help="client: the pairing code")
    parser.add_argument("--request", action="store_true",
                        help="client: ask for approval instead of sending a code")
    parser.add_argument("--name", default="peer-host", help="host: its name")
    parser.add_argument("--device-name", default="peer-client", help="client: its name")
    parser.add_argument("--unavailable", action="store_true",
                        help="host: the engine reports itself unavailable")
    parser.add_argument("--allow-other-commit", action="store_true")
    args = parser.parse_args(argv)
    if args.host != LOOPBACK:
        parser.error(f"the peer talks to {LOOPBACK} only")
    isolate(Path(args.data).resolve(), args.allow_other_commit)
    return run_host(args) if args.role == "host" else run_client(args)


if __name__ == "__main__":
    sys.exit(main())
