"""remote_requests: requests after ready, each step on its own connection (hello, ready, the
requests, close), so a replay can play any one step. "host" has every optional callback (models,
switching, runtime, model management, records, MCP) with the permissions in setup.permissions,
switched per step; "bare" has none. The engine is tests/fakes/remote.py FakeEngine (it streams);
stream_refused uses one without a stream, as Parakeet is."""
from __future__ import annotations

import threading

import remote_wire

NORMALIZE = [
    {"pointer": "/token", "as": "token", "when": {"type": "paired"}},
    {"pointer": "/device_id", "as": "device_id", "when": {"type": "paired"}},
    {"pointer": "/token", "as": "token", "when": {"type": "hello"}},
    {"pointer": "/host/fingerprint", "as": "fingerprint"},
    {"pointer": "/device/id", "as": "device_id", "when": {"type": "ready"}},
    {"pointer": "/host_ms", "as": "host_ms", "dir": "s2c"},
]

MODELS = [
    {"family": "parakeet", "model": "parakeet-v3", "label": "Parakeet TDT 0.6B v3"},
    {"family": "parakeet", "model": "parakeet-v2", "label": "Parakeet TDT 0.6B v2"},
]
RUNTIME = {
    "family": "parakeet",
    "model": "parakeet-v3",
    "selected": {"device": "auto", "language": "auto"},
    "devices": ["auto", "cpu", "cuda"],
    "compute_types": {},
    "languages": ["auto", "en", "de"],
    "gpu": {"name": "NVIDIA GeForce RTX 4070", "total_mib": 12282},
    "dependencies": [],
}
SWITCHED = {"family": "parakeet", "model": "parakeet-v2", "label": "Parakeet TDT 0.6B v2",
            "device": "cuda", "streaming": False, "available": True, "status": "ready"}
CATALOG = {"families": [{"family": "parakeet", "models": [
    {"model": "parakeet-v3", "label": "Parakeet TDT 0.6B v3", "downloaded": True}]}]}
MCP_STATE = {"enabled": True, "port": 8765, "url": "http://127.0.0.1:8765/mcp"}
RECORDS_STAT = {"dictation": {"count": 3, "bytes": 30720}, "meeting": {"count": 0, "bytes": 0}}
RECORDING_ID = "0123456789abcdef0123456789abcdef"
PHRASES = ["Parakeet", "Kubernetes  cluster"]


def build(tmp):
    from tests.fakes.remote import FakeEngine

    return remote_wire.Context(
        tmp, models=MODELS, runtime=RUNTIME, switched=SWITCHED, catalog=CATALOG,
        mcp_state=MCP_STATE, records_stat=RECORDS_STAT, recording_id=RECORDING_ID,
        phrases=PHRASES, engine=FakeEngine().describe(), tone="tests/fakes/remote.py _tone")


def _select_model(family, model, device_name, device=None):
    if model == "parakeet-missing":
        raise RuntimeError("That model isn't downloaded on the host.")
    return dict(SWITCHED)


def _manage(op, header, device_name):
    if op == "model_catalog":
        return CATALOG
    return {"job": op, "family": header["family"], "model": header["model"]}


def _records(op, header, payload, device):
    from services.remote_records.gate import RecordsBusy

    if op == "records_stat":
        return RECORDS_STAT
    if op == "records_open":
        raise LookupError("")
    if op == "records_put":
        raise RecordsBusy("Another upload is still being written. Try again shortly.")
    if op == "records_delete":
        raise ValueError("Invalid record ID.")
    return {}


def _mcp(op, header, device_name):
    if op == "mcp_configure":
        if not isinstance(header["settings"].get("port"), int):
            raise ValueError("invalid_port: The port must be a whole number.")
        return {**MCP_STATE, "port": header["settings"]["port"]}
    return MCP_STATE


def build_host(ctx, **kwargs):
    permissions = ctx.values.setdefault(
        "permissions", {"models": False, "records": False, "mcp": False, "settled": True})
    return ctx.host(
        models=lambda: [dict(m) for m in MODELS],
        runtime=lambda: dict(RUNTIME),
        select_model=_select_model,
        model_management=lambda: permissions["models"],
        manage_models=_manage,
        configure_runtime=lambda family, model, settings, device_name: dict(SWITCHED),
        records=_records,
        records_enabled=lambda: permissions["records"],
        records_summary=lambda device_id: {"dictation": {"count": 3, "bytes": 30720},
                                           "meeting": {"count": 0, "bytes": 0}},
        mcp=_mcp,
        mcp_enabled=lambda: permissions["mcp"],
        engine_settled=lambda: permissions["settled"],
        **kwargs)


def _permit(ctx, name, value):
    ctx.values["permissions"][name] = value
    ctx.note(f"permission {name} = {value}")


def _connect(ctx, host=None, **kwargs):
    from services.remote_asr.client import RemoteConnection

    host = host or ctx.values["host"]
    connection = RemoteConnection(remote_wire.LOOPBACK, host.port, ctx.values["token"],
                                  ctx.values["fingerprint"], **kwargs)
    ctx.call("connect", connection.connect)
    return connection


def _request(ctx, connection, op, **fields):
    label = op if not fields else f"{op} {sorted(fields)}"
    return ctx.call(label, connection.request, op, **fields)


def _tone(samples):
    from tests.fakes.remote import _tone as tone

    return tone(samples)


def pair(ctx):
    ctx.values["host"] = host = build_host(ctx, keep=True)
    ctx.values["bare"] = ctx.host(keep=True)
    ctx.client("pair_with_host")
    result = ctx.pair(host)
    ctx.values["token"] = result.token
    ctx.values["fingerprint"] = result.fingerprint
    ctx.values["device_id"] = result.device_id


def transcribe(ctx):
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx)
    _request(ctx, connection, "transcribe", audio=_tone(16000), language="en")
    _request(ctx, connection, "transcribe", audio=_tone(16000), language=None, phrases=PHRASES)
    _request(ctx, connection, "describe")
    _request(ctx, connection, "bogus")
    connection.close()


def bad_frames(ctx):
    ctx.client("raw frames on a RemoteConnection's socket")
    connection = _connect(ctx)
    ws = connection._ws
    frames = [
        "{}",
        b"\x00\x01",
        b"\x00\x01\x00\x00{}",
        b"\x00\x00\x00\x03abc",
        b"\x00\x00\x00\x02[]",
        b"\x00\x00\x00\x1a" + b'{"id":7,"op":"transcribe"}' + b"\x00\x00\x00",
        b"\x00\x00\x00\x1a" + b'{"id":8,"op":"transcribe"}' + b"\x00" * (120 * 16000 * 2 + 2),
    ]
    replies = []
    for frame in frames:
        ws.send(frame)
        replies.append(ws.recv(timeout=10))
    ctx.outcome("replies", replies)
    connection.close()


def stream(ctx):
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx)
    _request(ctx, connection, "stream", session="s1", language="en", finish=False,
             audio=_tone(8000))
    _request(ctx, connection, "stream", session="s1", language="en", finish=True,
             audio=_tone(8000))
    _request(ctx, connection, "cancel_stream", session="s1")
    _request(ctx, connection, "stream", language="en", finish=False, audio=_tone(1600))
    connection.close()


def stream_refused(ctx):
    from services.remote_asr.engines import HostEngine
    from tests.fakes.remote import FakeEngine

    class NoStreamEngine(FakeEngine):
        stream = HostEngine.stream

    host = ctx.host(engine=NoStreamEngine())
    ctx.note("the engine has HostEngine's stream (none), as a Parakeet host's")
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx, host)
    _request(ctx, connection, "stream", session="s1", language="en", finish=False,
             audio=_tone(8000))
    _request(ctx, connection, "cancel_stream", session="s1")
    connection.close()


def engine_loading(ctx):
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx)
    _permit(ctx, "settled", False)
    _request(ctx, connection, "transcribe", audio=_tone(16000), language="en")
    _request(ctx, connection, "stream", session="s1", language="en", finish=False,
             audio=_tone(1600))
    _request(ctx, connection, "describe")
    _permit(ctx, "settled", True)
    connection.close()


def update_hold(ctx):
    host = ctx.values["host"]
    ctx.client("RemoteConnection.request and recording_activity")
    connection = _connect(ctx, reports_recording=True)
    hold = host.prepare_update()
    ctx.outcome("prepare_update", {"token": bool(hold.get("token")),
                                   "expires_in": hold.get("expires_in")})
    _request(ctx, connection, "transcribe", audio=_tone(16000), language="en")
    _request(ctx, connection, "select_model", family="parakeet", model="parakeet-v2")
    ctx.call("recording_activity recording_begin", connection.recording_activity,
             "recording_begin", RECORDING_ID)
    _request(ctx, connection, "describe")
    ctx.outcome("cancel_update", host.cancel_update(hold["token"]))
    connection.close()


def busy(ctx):
    from tests.fakes.remote import FakeEngine

    entered, release = threading.Event(), threading.Event()

    class GatedEngine(FakeEngine):
        def transcribe(self, audio, language):
            entered.set()
            release.wait(10)
            return super().transcribe(audio, language)

    host = ctx.host(engine=GatedEngine(), max_active_jobs=1, max_queued_jobs=0)
    ctx.note("max_active_jobs=1, max_queued_jobs=0; the first connection's transcribe holds "
             "the one slot until the second has been refused")
    ctx.client("RemoteConnection.request (the second of two)")
    first = _connect(ctx, host)
    second = _connect(ctx, host)
    worker = ctx.background(_request, ctx, first, "transcribe", audio=_tone(16000),
                            language="en")
    ctx.wait(entered.is_set, "the first decode to start")
    _request(ctx, second, "transcribe", audio=_tone(16000), language="en")
    release.set()
    worker.join()
    second.close()
    first.close()


def engine_changed(ctx):
    from tests.fakes.remote import FakeEngine

    engines = [FakeEngine()]
    host = ctx.host(engine_provider=lambda: engines[0])
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx, host)
    engines[0] = FakeEngine(model="parakeet-v2")
    ctx.note("the host's engine becomes FakeEngine(model='parakeet-v2') after ready")
    _request(ctx, connection, "describe")
    ctx.outcome("closed_for_engine_change", connection.closed_for_engine_change)
    connection.close()


def select_model(ctx):
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx)
    _request(ctx, connection, "select_model", family="parakeet", model="parakeet-v2")
    _request(ctx, connection, "select_model", family="parakeet")
    _request(ctx, connection, "select_model", family="parakeet", model="parakeet-missing")
    _request(ctx, connection, "select_model", family="parakeet", model="parakeet-v2",
             device="cuda")
    _permit(ctx, "models", True)
    _request(ctx, connection, "select_model", family="parakeet", model="parakeet-v2",
             device="tpu")
    _request(ctx, connection, "select_model", family="parakeet", model="parakeet-v2",
             device="cpu", extra=1)
    _request(ctx, connection, "select_model", family="parakeet", model="parakeet-v2",
             device="cpu")
    _permit(ctx, "models", False)
    connection.close()
    bare = _connect(ctx, ctx.values["bare"])
    _request(ctx, bare, "select_model", family="parakeet", model="parakeet-v2")
    bare.close()


def configure_runtime(ctx):
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx)
    _request(ctx, connection, "configure_runtime", family="parakeet", model="parakeet-v3",
             settings={"device": "cpu"})
    _request(ctx, connection, "configure_runtime", family="parakeet", model="parakeet-v3",
             settings={"device": "cpu"}, extra=1)
    _request(ctx, connection, "configure_runtime", family="parakeet", model="parakeet-v3",
             settings="cpu")
    _request(ctx, connection, "configure_runtime", family="", model="parakeet-v3",
             settings={})
    connection.close()
    bare = _connect(ctx, ctx.values["bare"])
    _request(ctx, bare, "configure_runtime", family="parakeet", model="parakeet-v3",
             settings={"device": "cpu"})
    bare.close()


def model_ops(ctx):
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx)
    _request(ctx, connection, "model_catalog")
    _permit(ctx, "models", True)
    _request(ctx, connection, "model_catalog")
    _request(ctx, connection, "model_catalog", family="parakeet")
    _request(ctx, connection, "download_model", family="parakeet", model="parakeet-v2")
    _request(ctx, connection, "download_model", family="parakeet")
    _request(ctx, connection, "download_model", family="parakeet", model="parakeet-v2",
             extra=1)
    _request(ctx, connection, "install_runtime", family="parakeet", model="parakeet-v2",
             device="cuda")
    _request(ctx, connection, "install_runtime", family="parakeet", model="parakeet-v2",
             device="tpu")
    _permit(ctx, "models", False)
    connection.close()


def recording(ctx):
    ctx.client("RemoteConnection.recording_activity and request")
    connection = _connect(ctx, reports_recording=True)
    ctx.call("recording_activity recording_begin", connection.recording_activity,
             "recording_begin", RECORDING_ID)
    ctx.call("recording_activity recording_end", connection.recording_activity,
             "recording_end", RECORDING_ID)
    ctx.call("recording_activity recording_begin XYZ", connection.recording_activity,
             "recording_begin", "XYZ")
    _request(ctx, connection, "recording_end", recording_id=RECORDING_ID,
             payload=b"\x00\x00")
    connection.close()
    untracked = _connect(ctx)
    ctx.call("recording_activity recording_begin", untracked.recording_activity,
             "recording_begin", RECORDING_ID)
    untracked.close()


def records_and_mcp(ctx):
    ctx.client("RemoteConnection.request")
    bare = _connect(ctx, ctx.values["bare"])
    _request(ctx, bare, "records_stat")
    _request(ctx, bare, "mcp_state")
    bare.close()
    connection = _connect(ctx)
    _request(ctx, connection, "records_stat")
    _request(ctx, connection, "records_begin", kind="dictation", record_id="r1")
    _request(ctx, connection, "records_open", record_id="r1")
    _request(ctx, connection, "records_delete", record_id="../r1")
    _permit(ctx, "records", True)
    _request(ctx, connection, "records_put", upload="u1", offset=0, payload=b"abc")
    _request(ctx, connection, "records_begin", kind="dictation", record_id="r1")
    _permit(ctx, "records", False)
    _request(ctx, connection, "mcp_state")
    _permit(ctx, "mcp", True)
    _request(ctx, connection, "mcp_state")
    _request(ctx, connection, "mcp_state", extra=1)
    _request(ctx, connection, "mcp_configure")
    _request(ctx, connection, "mcp_configure", settings={"port": "eighty"})
    _request(ctx, connection, "mcp_configure", settings={"port": 8766})
    _permit(ctx, "mcp", False)
    connection.close()


def device_removed(ctx):
    host = ctx.values["host"]
    ctx.client("RemoteConnection.request")
    connection = _connect(ctx)
    host.remove_device(ctx.values["device_id"])
    ctx.wait(lambda: not connection.alive, "the host to close the connection")
    _request(ctx, connection, "describe")
    connection.close()


STEPS = [
    ("pair", pair),
    ("transcribe", transcribe),
    ("bad_frames", bad_frames),
    ("stream", stream),
    ("stream_refused", stream_refused),
    ("engine_loading", engine_loading),
    ("update_hold", update_hold),
    ("busy", busy),
    ("engine_changed", engine_changed),
    ("select_model", select_model),
    ("configure_runtime", configure_runtime),
    ("model_ops", model_ops),
    ("recording", recording),
    ("records_and_mcp", records_and_mcp),
    ("device_removed", device_removed),
]
