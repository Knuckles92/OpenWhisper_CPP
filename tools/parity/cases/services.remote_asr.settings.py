"""Curated inputs for services/remote_asr/settings.py (M9.2 remote-wire): the client's saved
pairing, following a host that moved, renaming it here, and the token's credential name.

The wrappers run the real functions over a settings mapping held in memory: the settings
manager's load_all_settings and save_setting are replaced for the call, so nothing reads or
writes a settings file. A pairing is recorded as dataclasses.asdict plus its properties
(renamed, address, over_tailscale, tailscale_fallback); remember_host_addresses and
rename_client_host record {"result": <pairing or None>, "written": <the saved dict or None>}.
token_credential runs with config.data_root and config.local_app_dir replaced by the given
paths, which do not exist (realpath keeps them as they are, normalized).
"""

import contextlib
import dataclasses

KEY = "remote_engine_client"
FP = "AB" * 32


def _pairing_record(pairing):
    if pairing is None:
        return None
    out = dataclasses.asdict(pairing)
    out["alternates"] = list(pairing.alternates)
    out["renamed"] = pairing.renamed
    out["address"] = pairing.address
    out["over_tailscale"] = pairing.over_tailscale
    out["tailscale_fallback"] = pairing.tailscale_fallback
    return out


@contextlib.contextmanager
def _settings(saved):
    from services.settings import settings_manager

    written = []
    names = ("load_all_settings", "save_setting")
    originals = {name: settings_manager.__dict__.get(name) for name in names}
    settings_manager.load_all_settings = lambda: ({} if saved is None else {KEY: saved})
    settings_manager.save_setting = lambda key, value: written.append([str(key), value])
    try:
        yield written
    finally:
        for name, original in originals.items():
            if original is None:
                delattr(settings_manager, name)
            else:
                setattr(settings_manager, name, original)


def _load_client_pairing(module, raw):
    return _pairing_record(module.load_client_pairing({KEY: raw}))


def _remember_host_addresses(module, saved, fingerprint, reached, port, addresses):
    with _settings(saved) as written:
        result = module.remember_host_addresses(fingerprint, reached, port, addresses)
    return {"result": _pairing_record(result), "written": written[0][1] if written else None,
            "writes": len(written)}


def _rename_client_host(module, saved, name):
    with _settings(saved) as written:
        result = module.rename_client_host(name)
    return {"result": _pairing_record(result), "written": written[0][1] if written else None,
            "writes": len(written)}


def _token_credential(module, data_root, local_app_dir):
    import config

    originals = (config.data_root, config.local_app_dir)
    config.data_root = lambda: data_root
    config.local_app_dir = lambda: local_app_dir
    try:
        return module.token_credential()
    finally:
        config.data_root, config.local_app_dir = originals


def _name_for(module, raw, reported):
    return module.load_client_pairing({KEY: raw}).name_for(reported)


def _constants(module):
    return {"TOKEN_CREDENTIAL": module.TOKEN_CREDENTIAL,
            "PAIRING_ROUTES": list(module.PAIRING_ROUTES),
            "RECORD_LOCATIONS": list(module.RECORD_LOCATIONS)}


def _saved(**fields):
    saved = {"host": "192.168.1.20", "port": 47821, "fingerprint": FP, "host_name": "devbox",
             "device_id": "d1", "paired_at": "2026-01-02T03:04:05+00:00",
             "alternates": ["100.64.0.2"], "via": "approval"}
    saved.update(fields)
    return saved


_ROOT = "C:\\fv-golden-nonexistent"

CASES = {
    "constants": [[]],
    "load_client_pairing": [
        [_saved()],
        [None], [[]], ["text"], [{}],
        [_saved(port="5000")], [_saved(port=5000.9)], [_saved(port="abc")], [_saved(port=None)],
        [_saved(port=0)], [_saved(port=65536)], [_saved(port=True)], [_saved(port=[1])],
        [{k: v for k, v in _saved().items() if k != "port"}],
        [_saved(host="")], [_saved(host=5)], [_saved(fingerprint="")], [_saved(fingerprint=None)],
        [_saved(via="tailscale")], [_saved(via="code")], [_saved(via="bogus")], [_saved(via=1)],
        [{k: v for k, v in _saved().items() if k != "via"}],
        [_saved(alternates="100.64.0.2")],
        [_saved(alternates=["192.168.1.20", "", 7, None, "100.64.0.2", "100.64.0.2", "fd7a::1"])],
        [_saved(host_name="")], [_saved(host_name=None)], [_saved(host_name=12)],
        [_saved(device_id=None, paired_at=0)], [_saved(paired_name="DESKTOP-1")],
        [_saved(paired_name="")], [_saved(host="100.64.0.2", alternates=["192.168.1.20"])],
        [_saved(host="fe80::1")], [_saved(alternates=["192.168.1.30", "fd7a:115c:a1e0::5"])],
        [_saved(extra="ignored")],
    ],
    "name_for": [
        [_saved(), "NEW-NAME"], [_saved(), ""], [_saved(), None],
        [_saved(host_name="Studio PC", paired_name="DESKTOP-4K2J9"), "NEW-NAME"],
    ],
    "remember_host_addresses": [
        [_saved(), "ab" * 32, "192.168.1.37", 47821, ["192.168.1.37", "100.64.0.2"]],
        [_saved(host="192.168.1.37"), "AB" * 32, "192.168.1.37", 47821,
         ["192.168.1.37", "100.64.0.2"]],
        [_saved(), "AB" * 32, "100.64.0.2", 47821, ["192.168.1.20", "100.64.0.2"]],
        [_saved(host="devbox.local", alternates=["192.168.1.20", "100.64.0.2"]), "AB" * 32,
         "192.168.1.37", 47821, ["192.168.1.37"]],
        [_saved(), "CD" * 32, "10.0.0.1", 47821, ["10.0.0.1"]],
        [_saved(), "AB" * 32, "192.168.1.37", 47821, ["192.168.1.37"]],
        [None, "AB" * 32, "192.168.1.37", 47821, ["192.168.1.37"]],
        [_saved(), "", "192.168.1.37", 47821, ["192.168.1.37"]],
        [_saved(), "AB" * 32, "192.168.1.20", 5000, ["192.168.1.20"]],
        [_saved(), "AB" * 32, "100.64.0.2", 5000, ["100.64.0.9", "192.168.1.50"]],
        [_saved(), "AB" * 32, "192.168.1.37", 47821, ["100.64.0.3"]],
        [_saved(), "AB" * 32, "192.168.1.37", 47821, []],
        [_saved(), "AB" * 32, "192.168.1.37", 47821, ["", 7, None, "192.168.1.37", "192.168.1.37"]],
        [_saved(paired_name="DESKTOP-4K2J9", host_name="Studio PC"), "AB" * 32, "192.168.1.37",
         47821, ["192.168.1.37"]],
        [_saved(alternates=[]), "AB" * 32, "192.168.1.20", 47821, ["192.168.1.20"]],
    ],
    "rename_client_host": [
        [_saved(host_name="DESKTOP-4K2J9"), " Studio PC "],
        [_saved(host_name="Studio PC", paired_name="DESKTOP-4K2J9"), ""],
        [_saved(host_name="Studio PC", paired_name="DESKTOP-4K2J9"), "Other"],
        [_saved(host_name="Studio PC", paired_name="DESKTOP-4K2J9"), "DESKTOP-4K2J9"],
        [_saved(host_name="DESKTOP-4K2J9"), "DESKTOP-4K2J9"],
        [_saved(host_name="DESKTOP-4K2J9"), "\u0000\u200b"],
        [_saved(host_name="DESKTOP-4K2J9"), "x" * 80],
        [_saved(host_name="DESKTOP-4K2J9"), None],
        [None, "Studio PC"],
    ],
    "token_credential": [
        [_ROOT + "\\Local\\OpenWhisper", _ROOT + "\\Local\\OpenWhisper"],
        [_ROOT + "\\Local\\OpenWhisper", _ROOT + "\\local\\openwhisper"],
        [_ROOT + "/Local/OpenWhisper", _ROOT + "\\Local\\OpenWhisper\\"],
        [_ROOT + "\\checkout\\data", _ROOT + "\\Local\\OpenWhisper"],
        [_ROOT + "\\Checkout\\Data", _ROOT + "\\Local\\OpenWhisper"],
        [_ROOT + "\\checkout\\.\\x\\..\\data", _ROOT + "\\Local\\OpenWhisper"],
        [_ROOT + "\\Donn\u00e9es\\\u00c4rger", _ROOT + "\\Local\\OpenWhisper"],
    ],
}

WRAPPERS = {
    "constants": _constants,
    "load_client_pairing": _load_client_pairing,
    "name_for": _name_for,
    "remember_host_addresses": _remember_host_addresses,
    "rename_client_host": _rename_client_host,
    "token_credential": _token_credential,
}
