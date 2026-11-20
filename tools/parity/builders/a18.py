"""Reference builders of the a18 remote engine UI (fv_ui_remote, M9.10).

Each builder returns (widget, (width, height), focus widget or None) with fixed fake data: the
host "devbox", fingerprint ABCD..., paired dates of 2026-11-12, the clock of the run (dates
convert to the machine's Pacific time, as on the C++ side). The page is OpenWhisper's own Settings
→ Remote engine, built by a SettingsDialog over a throwaway settings file in the isolated data
folder (out/parity/ow-data), as builders/a06.py builds the Hotkeys page, and bound to a fake
service and record sync that answer at once; then taken out of the dialog, since OpenWhisper_CPP's
RemoteEnginePage is the page body the Settings shell hosts. The page body has no object name
there (settingsPage_<key> is the shell's wrapper).

No search runs: the found computers are set as the last search's result, and the rows the
refreshes replaced are deleted before the grab. The pairing countdown's timer never fires before
it. The dialogs (rename, record recovery) are built as their own windows, never run.
"""

import os
import time
from contextlib import ExitStack
from unittest.mock import MagicMock, patch

PRODUCT = "OpenWhisper_CPP"
PAGE_WIDTH = 900
FINGERPRINT = "ABCD" * 16

# Patches and dialogs live as long as the process: the references are grabbed after the
# builders return.
_KEEP = []
# The widgets the builders made, deleted at exit while the application still exists.
_WIDGETS = []
_APP = []


def _release():
    from PyQt6 import sip

    for widget in reversed(_WIDGETS):
        if not sip.isdeleted(widget):
            sip.delete(widget)
    _WIDGETS.clear()
    for stack, _dialog_kept in reversed(_KEEP):
        stack.close()
    _KEEP.clear()
    _APP.clear()


def _keep(widget):
    import atexit

    from PyQt6.QtWidgets import QApplication

    if not _APP:
        _APP.append(QApplication.instance())
        atexit.register(_release)
    _WIDGETS.append(widget)
    return widget


class _Service:
    """Just enough RemoteEngineService for the page, answering from fixed data."""

    def __init__(self, state, pairing=None, summaries=None, tailscale=None):
        self.state = state
        self.pairing = pairing
        self.summaries = summaries or {}
        self.tailscale = tailscale

    def add_listener(self, listener):
        pass

    def remove_listener(self, listener):
        pass

    def host_state(self):
        return dict(self.state)

    def client_pairing(self):
        return self.pairing

    def tailscale_status(self, refresh=False):
        return self.tailscale

    def pending_pairing_name(self):
        return ""

    def records_summary(self, device_id):
        return self.summaries.get(device_id, {})

    def recover_device_records(self, owner, device):
        pass

    def scan_nearby(self, sweep=False):
        raise RuntimeError("the reference page never searches")


class _Records:
    def __init__(self, **status):
        self.fields = dict(location="local", paired=False, host_name="")
        self.fields.update(status)

    def status(self):
        from services.remote_records.sync import SyncStatus

        return SyncStatus(**self.fields)

    def location(self):
        return self.fields["location"]

    def add_listener(self, listener):
        pass

    def remove_listener(self, listener):
        pass

    def refresh_summary(self):
        pass


def _engine():
    return {"family": "parakeet", "model": "parakeet-v3", "label": "Parakeet TDT 0.6B v3",
            "device": "cuda", "streaming": False, "available": True, "status": ""}


def _state(**fields):
    state = {"enabled": False, "running": False, "port": 47821, "error": "",
             "host_name": "devbox", "address": None, "address_kind": None, "fingerprint": "",
             "pairing": None, "clients": [], "devices": [], "engine": None, "tailscale": None,
             "tailscale_trust": True, "model_management": False, "keep_records": False,
             "manage_mcp": False, "pair_request": None, "notes": []}
    state.update(fields)
    return state


def _dialog():
    from services.settings import SettingsManager
    from ui_qt.dialogs import settings_dialog as settings_dialog_module
    from ui_qt.dialogs import settings_downloads as downloads_module
    from ui_qt.dialogs import settings_models as models_module

    folder = os.path.join(os.environ["OPENWHISPER_DATA_DIR"], "a18")
    os.makedirs(folder, exist_ok=True)
    store = SettingsManager(os.path.join(folder, f"settings{len(_KEEP)}.json"))
    store.save_all_settings({})
    stack = ExitStack()
    for module in (settings_dialog_module, models_module, downloads_module):
        stack.enter_context(patch.object(module, "settings_manager", store))
    stack.enter_context(patch.object(settings_dialog_module.history_manager, "set_retention"))
    for module in (models_module, downloads_module):
        stack.enter_context(patch.object(module, "scan_cached_models", return_value={}))
    stack.enter_context(patch("services.local_asr.cache.inventory", return_value={}))
    stack.enter_context(
        patch.object(settings_dialog_module, "is_native_wayland_session", return_value=False))
    dialog = settings_dialog_module.SettingsDialog(background_cache_scan=False)
    dialog.on_profile_hotkey_capture = MagicMock()
    _KEEP.append((stack, _keep(dialog)))
    return dialog


def _flush():
    from PyQt6.QtCore import QCoreApplication, QEvent

    for _ in range(3):
        QCoreApplication.processEvents()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete.value)


def _page(service=None, records=None, scan=None):
    """The built Remote engine page body, out of its dialog and scroll area, bound to `service`
    and `records`, with `scan` as the last search."""
    from ui_qt.dialogs.settings_destinations import REMOTE_ENGINE

    dialog = _dialog()
    section = dialog.remote_section
    if records is not None:
        section._records = records
    # Bound before the page is built, as the controller's service is once it exists: the page
    # is drawn bound from the start.
    if service is not None:
        section.bind(service, lambda name: None)
        section._scan = scan
        section._scan_at = time.monotonic()
    dialog.ensure_page(REMOTE_ENGINE)
    dialog._page_scrolls[REMOTE_ENGINE].takeWidget()
    page = dialog._pages[REMOTE_ENGINE]
    page.setParent(None)
    page.setObjectName("")
    _keep(page)
    _flush()
    return page, section


def _settled_height(widget, width):
    """The height the widget's layout needs at `width`, measured shown."""
    widget.resize(width, 4000)
    widget.show()
    _flush()
    height = widget.layout().minimumSize().height()
    widget.hide()
    _flush()
    return height


def _tailnet(name, address):
    from services.remote_asr.tailscale import TailscaleStatus

    return TailscaleStatus("running", name=name, dns_name=f"{name}.tail.ts.net", address=address,
                           owner="owner@example.com", tailnet="example.com")


def _found():
    """devbox on this network and the tailnet, and an older studio box on this network."""
    from services.remote_asr import discovery
    from services.remote_asr.service import NearbyHost, NearbyScan, TailnetHost
    from services.remote_asr.tailscale import TailscalePeer

    engine = {"label": "Parakeet TDT 0.6B v3", "device": "cuda", "available": True,
              "family": "parakeet"}
    peer = TailscalePeer("devbox", "devbox.tail.ts.net", "100.64.0.2", "windows", True,
                         "owner@example.com", False, True)
    devbox = NearbyHost(
        "devbox", FINGERPRINT, engine, True, True,
        lan=discovery.LanHost("192.168.1.20", 47821, "devbox", FINGERPRINT, engine, 1, True),
        tailnet=TailnetHost(peer, 47821, "devbox", engine, True, True, FINGERPRINT, True))
    old = {"label": "Whisper turbo", "device": "cpu", "available": True, "family": "whisper"}
    studio = NearbyHost(
        "studio", "EF01" * 16, old, True, False,
        lan=discovery.LanHost("192.168.1.31", 47821, "studio", "EF01" * 16, old, 1, False))
    return NearbyScan(_tailnet("laptop", "100.64.0.1"), (devbox, studio))


def client_unpaired(variant):
    service = _Service(_state(), tailscale=_tailnet("laptop", "100.64.0.1"))
    page, _section = _page(service, _Records(), _found())
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def client_paired(variant):
    from services.remote_asr.settings import ClientPairing

    pairing = ClientPairing(host="192.168.1.20", port=47821, fingerprint=FINGERPRINT,
                            host_name="devbox", device_id="laptop-device",
                            paired_at="2026-11-12T18:00:00+00:00", via="code")
    records = _Records(location="both", paired=True, host_name="devbox",
                       host_keeps=True, host_supports=True,
                       stored={"dictation": {"count": 12, "bytes": 1288490188},
                               "meeting": {"count": 3, "bytes": 0}})
    service = _Service(_state(), pairing=pairing)
    page, _section = _page(service, records)
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def host_sharing(variant):
    from services.remote_asr.reachability import Note

    devices = [
        {"id": "laptop-device", "name": "laptop", "paired_at": "2026-11-12T18:00:00+00:00",
         "last_seen": "2026-11-14T09:58:00+00:00", "via": "approval"},
        {"id": "studio-device", "name": "Studio PC", "paired_at": "2026-11-13T17:30:00+00:00",
         "last_seen": "2026-11-13T20:00:00+00:00", "via": "tailscale",
         "paired_name": "DESKTOP-4K2J9"},
    ]
    clients = [{"device_id": "laptop-device", "name": "laptop", "address": "192.168.1.31",
                "busy": False, "since": 0.0}]
    note = Note("public_network",
                "Windows treats \"Home\" as a public network, so computers on it may not reach "
                "this one.", action="Open network settings", url="ms-settings:network-status")
    state = _state(enabled=True, running=True, address="192.168.1.20", address_kind="lan",
                   fingerprint=FINGERPRINT, pairing=("482913", 54.0), clients=clients,
                   devices=devices, engine=_engine(),
                   tailscale=_tailnet("devbox", "100.64.0.2"), keep_records=True,
                   notes=[note])
    summaries = {"laptop-device": {"dictation": {"count": 4, "bytes": 2048}}}
    service = _Service(state, summaries=summaries)
    page, _section = _page(service, _Records())
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def unbound(variant):
    page, _section = _page()
    return page, (PAGE_WIDTH, _settled_height(page, PAGE_WIDTH)), None


def _shown_dialog(dialog):
    """A dialog at the size it opens with, as its own window."""
    dialog.adjustSize()
    size = dialog.size()
    _keep(dialog)
    return dialog, (size.width(), size.height())


def remote_name_dialog(variant):
    from ui_qt.dialogs import settings_remote

    built = []

    def capture(self):
        built.append(self)
        return 0

    from PyQt6.QtWidgets import QDialog

    with patch.object(QDialog, "exec", capture):
        settings_remote._ask_name(
            None, "Rename host",
            "The name this computer shows for its host. Leave it empty to go back to "
            "DESKTOP-4K2J9, the name it gave when pairing.", "devbox", "DESKTOP-4K2J9")
    dialog, size = _shown_dialog(built[0])
    return dialog, size, None


def record_recovery_dialog(variant):
    from PyQt6.QtWidgets import QDialog

    from ui_qt.dialogs import settings_remote

    built = []

    def capture(self):
        built.append(self)
        return 0

    class Service:
        def recoverable_record_owners(self):
            return [{"id": "0f3c9a7e21d84b6c", "name": "laptop",
                     "counts": {"dictation": 12, "meeting": 3}}]

        def host_state(self):
            return {"devices": [{"id": "9b1e44c07a2f4d55", "name": "Studio PC"}]}

    section = settings_remote.RemoteEngineSection()
    section._service = Service()
    section._recovery_busy = False
    stand_in = type("Tile", (), {"window": lambda self: None})()
    section.__dict__["devices_tile"] = stand_in
    with patch.object(QDialog, "exec", capture):
        section._recover_records()
    dialog = built[0]
    _keep(dialog)
    return dialog, (dialog.width(), dialog.height()), None


BUILDERS = {
    "remote_engine_page/client-unpaired": client_unpaired,
    "remote_engine_page/client-paired": client_paired,
    "remote_engine_page/host-sharing": host_sharing,
    "remote_engine_page/unbound": unbound,
    "remote_name_dialog": remote_name_dialog,
    "record_recovery_dialog": record_recovery_dialog,
}
