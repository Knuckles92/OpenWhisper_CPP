"""remote_history: the direct-history connection. HistoryClient (services/remote_history/client.py)
says hello with purpose "history", the host's HistoryBroker answers ready with client_history,
asks two queries and one the client refuses as invalid, then one whose answer is over 8 MiB, which
the client replaces by "unavailable" (HistoryReader.query patched to return 9 MiB for "check");
stopping the client closes the connection."""
from __future__ import annotations

import remote_wire

NORMALIZE = [
    {"pointer": "/token", "as": "token", "when": {"type": "paired"}},
    {"pointer": "/device_id", "as": "device_id", "when": {"type": "paired"}},
    {"pointer": "/token", "as": "token", "when": {"type": "hello"}},
    {"pointer": "/host/fingerprint", "as": "fingerprint"},
    {"pointer": "/id", "as": "uuid", "when": {"type": "history_query"}},
    {"pointer": "/id", "as": "uuid", "when": {"type": "history_result"}},
]
DATE = "2026-09-01T12:00:00+00:00"
OVER_LIMIT = 9 * 1024 * 1024


def build(tmp):
    from services.database import DatabaseManager
    from services.models import TranscriptionHistory

    path = tmp / "client.db"
    database = DatabaseManager(str(path))
    rows = [{"id": f"h{n}", "text": f"Launch {n}", "timestamp": DATE, "model": "test"}
            for n in range(3)]
    with database.get_session() as session:
        for row in rows:
            session.add(TranscriptionHistory(**row))
    ctx = remote_wire.Context(tmp, client_history_rows=rows)
    ctx.values["database"] = database
    ctx.values["database_path"] = path
    ctx.cleanup(database.close)
    return ctx


def pair(ctx):
    ctx.values["host"] = host = ctx.host(keep=True)
    ctx.client("pair_with_host")
    result = ctx.pair(host)
    ctx.values["pairing"] = result


def connect(ctx):
    from services.remote_asr.settings import ClientPairing
    from services.remote_history.client import HistoryClient

    host, pairing = ctx.values["host"], ctx.values["pairing"]
    ctx.client("HistoryClient")
    client = HistoryClient(
        ctx.values["database_path"],
        pairing=lambda: ClientPairing(remote_wire.LOOPBACK, host.port, pairing.fingerprint,
                                      remote_wire.HOST_NAME),
        token=lambda: pairing.token,
        enabled=lambda: True)
    ctx.values["client"] = client
    client.start()
    ctx.wait(lambda: host.history.clients()[0]["status"] == "online", "the client to say hello")
    ctx.outcome("clients", host.history.clients())


def _query(ctx, operation, params):
    host, pairing = ctx.values["host"], ctx.values["pairing"]
    ctx.call(f"HistoryBroker.query {operation}", host.history.query, pairing.device_id,
             operation, params)


def queries(ctx):
    ctx.client("HistoryClient (answering HistoryBroker.query)")
    _query(ctx, "transcriptions", {"limit": 2})
    _query(ctx, "transcription", {"record_id": "h1"})
    _query(ctx, "transcriptions", {"limit": 0})


def over_limit(ctx):
    from services.remote_history.client import HistoryReader

    original = HistoryReader.query

    def query(reader, operation, params):
        if operation == "check":
            return {"result": {"blob": "x" * OVER_LIMIT}}
        return original(reader, operation, params)

    ctx.patch(HistoryReader, "query", query,
              "HistoryReader.query answers check with a 9 MiB result")
    ctx.client("HistoryClient (answering HistoryBroker.query)")
    _query(ctx, "check", {})


def stop(ctx):
    host = ctx.values["host"]
    ctx.client("HistoryClient.stop")
    ctx.values["client"].stop()
    ctx.wait(lambda: host.history.clients()[0]["status"] == "unavailable",
             "the host to see the client go")
    ctx.outcome("clients", host.history.clients())


STEPS = [
    ("pair", pair),
    ("connect", connect),
    ("queries", queries),
    ("over_limit", over_limit),
    ("stop", stop),
]
