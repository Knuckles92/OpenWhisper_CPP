"""Curated inputs for meeting/platform.py (M7.9 meeting-core).

The C++ policy is pure over (platform, machine, macOS release); Python reads the last two from
the platform module. Each wrapper pins them for the call:

    meeting_mode_supported(platform, machine, mac_release)
    meeting_setup_available(platform, machine, mac_release)    sys.platform pinned too
    system_audio_permission_granted(platform)                  sys.platform pinned; Quartz never
                                                               reached off darwin
"""

import contextlib
import types


@contextlib.contextmanager
def _pinned(module, platform=None, machine=None, mac_release=None):
    saved_machine = module.platform_module.machine
    saved_mac_ver = module.platform_module.mac_ver
    saved_sys = module.sys
    if machine is not None:
        module.platform_module.machine = lambda: machine
    if mac_release is not None:
        module.platform_module.mac_ver = lambda: (mac_release, ("", "", ""), "")
    if platform is not None:
        module.sys = types.SimpleNamespace(platform=platform)
    try:
        yield
    finally:
        module.platform_module.machine = saved_machine
        module.platform_module.mac_ver = saved_mac_ver
        module.sys = saved_sys


def _supported(module, platform, machine, mac_release):
    with _pinned(module, machine=machine, mac_release=mac_release):
        return module.meeting_mode_supported(platform, machine=machine)


def _setup_available(module, platform, machine, mac_release):
    with _pinned(module, platform=platform, machine=machine, mac_release=mac_release):
        return module.meeting_setup_available()


def _granted(module, platform):
    with _pinned(module, platform=platform):
        return module.system_audio_permission_granted()


_MACHINES = ["x86_64", "AMD64", "amd64", " X64 ", "x64", "aarch64", "ARM64", "arm64", "i686",
             "armv7l", "", "  ", "riscv64", "x86_64\n"]

_PLATFORMS = [
    ["win32", "AMD64", ""], ["win64", "ARM64", ""], ["windows", "", ""],
    ["darwin", "arm64", "13.0"], ["darwin", "arm64", "14.2.1"], ["darwin", "x86_64", "12.7.1"],
    ["darwin", "arm64", ""], ["darwin", "arm64", "Ventura"], ["darwin", "arm64", " 13"],
    ["darwin", "arm64", "26.1"], ["darwin", "arm64", "13_0.1"],
    ["linux", "x86_64", ""], ["linux2", "aarch64", ""], ["linux", "i686", ""],
    ["linux", "AMD64", ""], ["freebsd14", "amd64", ""], ["cygwin", "x86_64", ""],
]

CASES = {
    "normalize_linux_machine": [[m] for m in _MACHINES],
    "linux_meeting_implementation_ready": [[m] for m in _MACHINES],
    "_macos_major": [["13.0"], ["12.7.1"], [""], ["Ventura"], ["14"], [" 15.1"], ["1_3.0"],
                     ["13.x"], ["-1.0"], ["+13.0"]],
    "meeting_mode_supported": _PLATFORMS,
    "meeting_setup_available": _PLATFORMS,
    "meeting_unsupported_os_name": [["darwin"], ["linux"], ["linux2"], ["freebsd14"], ["win32"],
                                    ["Darwin"]],
    "system_audio_permission_required": [["darwin"], ["win32"], ["linux"], ["Darwin"]],
    "system_audio_permission_granted": [["win32"], ["linux"]],
}

WRAPPERS = {
    "meeting_mode_supported": _supported,
    "meeting_setup_available": _setup_available,
    "system_audio_permission_granted": _granted,
}
