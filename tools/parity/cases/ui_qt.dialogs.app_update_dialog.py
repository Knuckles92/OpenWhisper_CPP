"""Curated inputs for ui_qt/dialogs/app_update_dialog.py's text methods (M3.2,
update/update_copy.h).

`texts` runs the dialog's text methods on a stub self (no widget is built) holding the check
result a spec describes and the error text. A spec is null (the check failed) or {"status",
"current_version", "channel", "apply_mode", "can_apply", "release": null | {"version",
"prerelease", "setup_size", "native_size"}}; a size of null means no such asset. The result is
{"title", "body", "size", "hint", "primary", "later", "progress": [[phase, done, total, text]],
"phase_text": {phase: text or null}}. Product names are compared rebranded.
"""

import types

PHASES = ["downloading", "verifying", "extracting", "opening", "restarting", "rolling_back"]
PROGRESS = [["downloading", 0, 0], ["downloading", 1500000, 91717696],
            ["downloading", 999, 1000], ["verifying", 91717696, 91717696],
            ["extracting", 3, 120], ["opening", 0, 0], ["restarting", 0, 0],
            ["rolling_back", 1, 2]]


def _asset(module, version, kind, size):
    from services.app_update import ReleaseAsset

    if size is None:
        return None
    name = (f"OpenWhisper-Setup-{version}.exe" if kind == "setup"
            else f"OpenWhisper-{version}-win64.tar.xz")
    return ReleaseAsset(url=f"https://github.com/x/y/releases/download/v{version}/{name}",
                        name=name, size_bytes=size, sha256="ab" * 32)


def _result(module, spec):
    from services.app_update import ReleaseInfo, UpdateCheckResult

    if spec is None:
        return None
    release = None
    if spec["release"] is not None:
        r = spec["release"]
        release = ReleaseInfo(
            version=r["version"], tag_name="v" + r["version"], html_url="https://example/notes",
            notes="notes", setup_asset=_asset(module, r["version"], "setup", r["setup_size"]),
            native_asset=_asset(module, r["version"], "native", r["native_size"]),
            prerelease=r["prerelease"])
    return UpdateCheckResult(status=spec["status"], current_version=spec["current_version"],
                             channel=spec["channel"], release=release,
                             can_apply=spec["can_apply"], apply_mode=spec["apply_mode"])


def _texts(module, spec, error):
    dialog = module.AppUpdateDialog
    stub = types.SimpleNamespace(_result=_result(module, spec), _error=error or "")
    stub._download_size_bytes = lambda: dialog._download_size_bytes(stub)
    return {
        "title": dialog._title_text(stub),
        "body": dialog._body_text(stub),
        "size": dialog._download_size_bytes(stub),
        "hint": dialog._hint_text(stub),
        "primary": dialog._primary_label(stub),
        "later": dialog._later_label(stub),
        "progress": [[phase, done, total, dialog._progress_detail_text(stub, phase, done, total)]
                     for phase, done, total in PROGRESS],
        "phase_text": {phase: module._PHASE_TEXT.get(phase) for phase in PHASES},
    }


def _spec(status, channel="installer", mode="setup", can_apply=True, version="0.2.0",
          prerelease=False, setup_size=91717696, native_size=None, current="0.1.0"):
    release = None if version is None else {
        "version": version, "prerelease": prerelease, "setup_size": setup_size,
        "native_size": native_size}
    return {"status": status, "current_version": current, "channel": channel,
            "apply_mode": mode, "can_apply": can_apply, "release": release}


CASES = {
    "texts": [
        [None, ""],
        [None, "GitHub rate-limited this update check. Try again later."],
        [_spec("update_available"), ""],
        [_spec("update_available", prerelease=True), ""],
        [_spec("update_available", setup_size=999), ""],
        [_spec("update_available", setup_size=1500000), ""],
        [_spec("update_available", setup_size=1000000000), ""],
        [_spec("update_available", mode="notify_only", can_apply=False), ""],
        [_spec("update_available", channel="source", mode="notify_only", can_apply=False), ""],
        [_spec("update_available", mode="native", native_size=123456789), ""],
        [_spec("update_available", mode="native", native_size=None), ""],
        [_spec("update_available", mode="setup", setup_size=None, can_apply=False), ""],
        [_spec("update_available", version=None), ""],
        [_spec("up_to_date", version="0.1.0"), ""],
        [_spec("up_to_date", version=None), ""],
        [_spec("development", version="0.0.9", current="0.1.0"), ""],
        [_spec("development", version=None), ""],
        [_spec("development", channel="source", mode="notify_only", can_apply=False,
               version="0.0.9"), "ignored error"],
    ],
}

WRAPPERS = {"texts": _texts}

PINS = {"texts_note": "AppUpdateDialog text methods on a stub self; channel labels for win32"}
