"""Curated inputs for the wording helpers of ui_qt/dialogs/settings_remote.py (M9.10).

    _paired_on(iso)                         "Jan 5, 2025" in the machine's zone, or ""
    _stored_phrase(stored)                  "12 dictations and 3 meetings (1.2 GB)"
    _records_location_description(loc, h)  the storage tile's description
    _records_host_problem(status, host)     status is a dict of SyncStatus fields
    _sharing_address(state, tailnet, on)    state is host_state()'s dict, tailnet a dict of
                                            TailscaleStatus fields or None
    _sharing_status(state, where)
    _engine_phrase(engine)
    _find_row_detail(host)                  a found computer's row, built by _find_row from a
                                            dict standing for NearbyHost: {"text", "button",
                                            "enabled", "tooltip"}
    pairing_grants_phrase(grants)           the pair-request dialog's grants line
                                            (pair_request_dialog.py), which the C++ keeps with
                                            these helpers

The aware timestamps convert with astimezone(), so they hold the machine's local zone
(pins.local_timezone: Pacific, America/Los_Angeles rules); the replay passes that zone in. The
two found-row and grants wrappers build OpenWhisper's widgets, so they make an offscreen
QApplication first.
"""

import os


# The application the row and dialog wrappers need, kept for the life of the process.
_APP = []


def _app():
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication

    if not _APP:
        _APP.append(QApplication.instance() or QApplication([]))
    return _APP[0]


def _records_host_problem(module, status, host):
    from services.remote_records.sync import SyncStatus

    return module._records_host_problem(SyncStatus(**status), host)


def _tailnet(fields):
    if fields is None:
        return None
    from services.remote_asr.tailscale import TailscaleStatus

    return TailscaleStatus(**fields)


def _sharing_address(module, state, tailnet, on_tailnet):
    return module._sharing_address(state, _tailnet(tailnet), on_tailnet)


def _find_row_detail(module, host):
    from types import SimpleNamespace

    from services.remote_asr import discovery
    from services.remote_asr.service import NearbyHost, TailnetHost
    from services.remote_asr.tailscale import TailscalePeer
    from ui_qt.widgets import Button, WrappedLabel

    _app()
    lan = None
    if host.get("lan"):
        lan = discovery.LanHost(host["lan"], 47821, host["host_name"], host["fingerprint"],
                                host["engine"], 1 if host["compatible"] else 2, host["approval"])
    tailnet = None
    if host.get("tailnet"):
        peer = host["tailnet"]
        tailnet = TailnetHost(
            TailscalePeer(peer["name"], peer["name"] + ".tail.ts.net", peer["address"], "windows",
                          True, peer["owner"], False, peer["mine"]),
            47821, host["host_name"], host["engine"], peer["pairing"], host["compatible"],
            host["fingerprint"], host["approval"])
    found = NearbyHost(host["host_name"], host["fingerprint"], host["engine"], host["compatible"],
                       host["approval"], lan=lan, tailnet=tailnet)
    page = SimpleNamespace(_pairing_busy=host.get("busy", False), _connect=lambda h: None,
                           _use_code=lambda h: None)
    row = module.RemoteEngineSection._find_row(page, found)
    label = row.findChild(WrappedLabel, "remoteFindLabel")
    button = row.findChildren(Button)[0]
    result = {"text": label.text(), "button": button.text(), "enabled": button.isEnabled(),
              "tooltip": label.toolTip()}
    row.deleteLater()
    return result


def _pairing_grants_phrase(module, grants):
    from ui_qt.dialogs.pair_request_dialog import PairRequestDialog

    _app()
    dialog = PairRequestDialog({"id": "r1", "name": "laptop", "address": "192.168.1.9",
                                "sas": "123456", "seconds_left": 60}, None, grants)
    text = dialog.grants_label.text()
    dialog._timer.stop()
    dialog.deleteLater()
    return text


WRAPPERS = {
    "_records_host_problem": _records_host_problem,
    "_sharing_address": _sharing_address,
    "_find_row_detail": _find_row_detail,
    "pairing_grants_phrase": _pairing_grants_phrase,
}

_ENGINE = {"label": "Fake Parakeet", "device": "cuda", "available": True, "family": "parakeet"}
_FP = "AB" * 32


def _host(**fields):
    host = {"host_name": "devbox", "fingerprint": _FP, "engine": dict(_ENGINE),
            "compatible": True, "approval": True, "lan": "192.168.1.20", "tailnet": None}
    host.update(fields)
    return host


def _peer(**fields):
    peer = {"name": "devbox", "address": "100.64.0.2", "owner": "owner@example.com",
            "mine": True, "pairing": True}
    peer.update(fields)
    return peer


def _state(**fields):
    state = {"enabled": True, "running": True, "port": 47821, "error": "", "host_name": "devbox",
             "address": "192.168.1.20", "address_kind": "lan", "fingerprint": _FP,
             "engine": dict(_ENGINE)}
    state.update(fields)
    return state


_TAILNET = {"state": "running", "name": "laptop", "address": "100.64.0.1",
            "owner": "owner@example.com"}

CASES = {
    "_paired_on": [
        ["2025-01-05T12:00:00+00:00"],
        ["2026-11-14T10:00:00+00:00"],
        ["2026-11-14T07:59:59+00:00"],
        ["2026-11-14T08:00:00+00:00"],
        ["2026-03-08T10:30:00+00:00"],
        ["2026-07-04T03:00:00+00:00"],
        ["2026-12-31T23:30:00Z"],
        ["2027-01-01T07:59:59+00:00"],
        ["2026-10-10T12:00:00+05:30"],
        ["2026-10-01T09:05:00"],
        ["2026-10-01"],
        ["2026-02-09T10:00:00+00:00"],
        ["2026-02-10T20:00:00+00:00"],
        ["2026-11-14T10:00:00.123456+00:00"],
        [""],
        ["not a date"],
        ["2026-13-45T00:00:00+00:00"],
        [" 2026-11-14T10:00:00+00:00"],
        [None],
        [12345],
    ],
    "_stored_phrase": [
        [{}],
        [{"dictation": {"count": 12, "bytes": 1288490188}, "meeting": {"count": 3, "bytes": 0}}],
        [{"dictation": {"count": 1, "bytes": 2048}}],
        [{"meeting": {"count": 1, "bytes": 1023}}],
        [{"dictation": {"count": 3, "bytes": 2048}, "meeting": {"count": 1, "bytes": 0}}],
        [{"dictation": {"count": 0, "bytes": 512}, "meeting": {"count": 2, "bytes": 1048576}}],
        [{"dictation": {"count": 0, "bytes": 0}, "meeting": {"count": 0, "bytes": 0}}],
        [{"dictation": {"count": 2}}],
        [{"dictation": {"count": 2, "bytes": None}, "meeting": None}],
        [{"dictation": {"count": "4", "bytes": "100"}}],
        [{"dictation": {"count": 2.9, "bytes": 1536.5}}],
        [{"dictation": {"count": True, "bytes": 0}}],
        [{"dictation": {}, "meeting": {"count": 5, "bytes": 5368709120}}],
        [{"other": {"count": 9, "bytes": 9}}],
        [None],
        [[]],
        ["text"],
    ],
    "_records_location_description": [
        ["local", "devbox"],
        ["host", "devbox"],
        ["both", "devbox"],
        ["host", "the host"],
        ["both", "Studio PC"],
        ["elsewhere", "devbox"],
    ],
    "_records_host_problem": [
        [{"location": "local", "paired": True, "host_supports": False}, "devbox"],
        [{"location": "host", "paired": True, "host_supports": False}, "devbox"],
        [{"location": "both", "paired": True, "host_supports": False, "host_keeps": False},
         "devbox"],
        [{"location": "host", "paired": True, "host_keeps": False, "host_supports": True},
         "devbox"],
        [{"location": "both", "paired": True, "host_keeps": False}, "the host"],
        [{"location": "host", "paired": True, "host_keeps": True, "host_supports": True},
         "devbox"],
        [{"location": "host", "paired": True}, "devbox"],
    ],
    "_sharing_address": [
        [_state(), None, False],
        [_state(address="fe80::1"), None, False],
        [_state(address=None), None, False],
        [_state(address=""), None, False],
        [_state(address="10.8.0.4", address_kind="vpn"), None, False],
        [_state(), _TAILNET, True],
        [_state(), dict(_TAILNET, name=""), True],
        [_state(address="10.8.0.4", address_kind="vpn", port=50000), _TAILNET, True],
        [_state(), _TAILNET, False],
        [_state(address="fd7a:115c:a1e0::5"), dict(_TAILNET, address="100.64.0.9"), True],
    ],
    "_sharing_status": [
        [_state(), "192.168.1.20:47821"],
        [_state(error="UDP port 47821 is in use"), "192.168.1.20:47821"],
        [_state(engine=dict(_ENGINE, compute_type="int8")), "devbox:47821"],
        [_state(engine=dict(_ENGINE, available=False, status="The host is loading its speech "
                                                           "engine.")), "devbox:47821"],
        [_state(engine=dict(_ENGINE, available=False)), "devbox:47821"],
        [_state(engine=None), "devbox:47821"],
        [_state(engine={}), "devbox:47821"],
        [_state(running=False), "devbox:47821"],
        [_state(running=False, enabled=False), "devbox:47821"],
        [_state(engine={"available": True}), "devbox:47821"],
    ],
    "_engine_phrase": [
        [{"label": "Whisper turbo", "device": "cuda", "compute_type": "int8_float32"}],
        [{"label": "Parakeet v3", "device": "cpu"}],
        [{"label": "Parakeet v3", "device": "", "compute_type": "int8"}],
        [{"label": "Parakeet v3"}],
        [{"label": "", "device": "cuda"}],
        [{}],
        [{"label": None, "device": "cuda"}],
        [{"label": "Parakeet TDT 0.6B", "device": "cuda", "compute_type": None}],
        [{"label": 3, "device": True, "compute_type": 1.5}],
    ],
    "_find_row_detail": [
        [_host()],
        [_host(approval=False)],
        [_host(compatible=False)],
        [_host(engine={})],
        [_host(engine=dict(_ENGINE, available=False))],
        [_host(engine=dict(_ENGINE, compute_type="int8"))],
        [_host(lan=None, tailnet=_peer())],
        [_host(tailnet=_peer())],
        [_host(lan=None, tailnet=_peer(mine=False))],
        [_host(lan=None, approval=False, tailnet=_peer(mine=False))],
        [_host(lan=None, approval=False, tailnet=_peer(mine=False, owner=""))],
        [_host(approval=False, tailnet=_peer(mine=False, owner=""))],
        [_host(lan=None, approval=False, tailnet=_peer(pairing=False))],
        [_host(lan=None, approval=False, tailnet=_peer(name="studio"))],
        [_host(busy=True)],
        [_host(compatible=False, approval=False, busy=True)],
        [_host(lan="fe80::1")],
    ],
    "pairing_grants_phrase": [
        [[]],
        [["manage its models"]],
        [["manage its models", "keep its records here"]],
        [["manage its models", "keep its records here", "manage its MCP server"]],
    ],
}
