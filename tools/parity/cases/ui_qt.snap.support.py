"""Curated inputs for ui_qt/snap/support.py (fv_ui_snap support). Each case poses as one
desktop, [platform, hyprland, grim on PATH, wayland, macOS Screen Recording granted], the way
tests/test_snap_session.py's TestSupport fixture does, and restores the module afterwards.
snap_support returns [supported, reason]; the macOS reason names the product."""


def _posed(module, platform, hyprland, grim, wayland, granted, call):
    saved = {
        (module, "_platform"): module._platform,
        (module.hyprland, "available"): module.hyprland.available,
        (module.shutil, "which"): module.shutil.which,
        (module, "is_wayland"): module.is_wayland,
        (module, "is_wayland_session"): module.is_wayland_session,
        (module.macos, "capture_granted"): module.macos.capture_granted,
    }
    module._platform = lambda: platform
    module.hyprland.available = lambda: hyprland
    module.shutil.which = lambda name: "/usr/bin/grim" if grim and name == "grim" else None
    module.is_wayland = lambda: wayland
    module.is_wayland_session = lambda: wayland
    module.macos.capture_granted = lambda: granted
    try:
        return call()
    finally:
        for (owner, name), value in saved.items():
            setattr(owner, name, value)


def _snap_support(module, *desktop):
    return list(_posed(module, *desktop, call=module.snap_support))


def _uses_grim(module, *desktop):
    return _posed(module, *desktop, call=module.uses_grim)


WRAPPERS = {"snap_support": _snap_support, "uses_grim": _uses_grim}

DESKTOPS = [
    [platform, hyprland, grim, wayland, granted]
    for platform in ("win32", "darwin", "linux", "freebsd14", "cygwin")
    for hyprland in (False, True)
    for grim in (False, True)
    for wayland in (False, True)
    for granted in (False, True)
]

CASES = {"snap_support": DESKTOPS, "uses_grim": DESKTOPS}
