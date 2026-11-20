"""Curated inputs for ui_qt/dialogs/settings_microphones.py (M2.2 appcore/mic_priority.h).

Device lists are M1.3's dumped PortAudio lists (tests/fixtures/audio/devices-*.json) read
through services.audio_devices.list_input_devices behind the fake sounddevice of
cases/services.audio_devices.py; each device goes into the vectors as
{index, name, hostapi, label, default_api, system_alias}. "windows" and "alsa" name the full
lists, "none" an unknown list (None: the first enumeration has not arrived), "empty" no devices.

MicrophoneSection's rules run as its real methods on a section built with a stand-in dialog: the
combos are a recording stand-in (items, current index, enabled) and _save is captured instead of
written, so the vectors record what the section would show and save.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace

_AUDIO_CASES = Path(__file__).with_name("services.audio_devices.py")
_spec = importlib.util.spec_from_file_location("_cases_audio_devices", _AUDIO_CASES)
_audio_cases = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_audio_cases)

_FIXTURES = {"windows": "devices-windows.json", "alsa": "devices-alsa.json"}


def _devices(name):
    from services import audio_devices

    if name == "none":
        return None
    if name == "empty":
        return []
    return audio_devices.list_input_devices(_audio_cases._sd(_FIXTURES[name]))


def _device_dict(device):
    from services import audio_devices

    return {"index": device.index, "name": device.name, "hostapi": device.hostapi,
            "label": device.label, "default_api": device.default_api,
            "system_alias": audio_devices.is_system_alias(device)}


class _Combo:
    """The QComboBox calls the section makes."""

    def __init__(self):
        self.items, self.current, self.enabled, self.blocked = [], -1, True, False

    def blockSignals(self, block):
        previous, self.blocked = self.blocked, block
        return previous

    def clear(self):
        self.items, self.current = [], -1

    def addItem(self, text, data=None):
        self.items.append([text, data])
        if self.current < 0:
            self.current = 0

    def findData(self, data):
        return next((i for i, item in enumerate(self.items) if item[1] == data), -1)

    def setCurrentIndex(self, index):
        self.current = index

    def currentData(self):
        return self.items[self.current][1] if 0 <= self.current < len(self.items) else None

    def count(self):
        return len(self.items)

    def setEnabled(self, enabled):
        self.enabled = bool(enabled)

    def isEnabled(self):
        return self.enabled


def _section(module, priority, devices):
    from services import audio_devices

    dialog = SimpleNamespace(audio_device_combo=_Combo(), on_audio_device_changed=None)
    section = module.MicrophoneSection(dialog, list_devices=lambda: [])
    section.devices = _devices(devices)
    section.priority = audio_devices.normalize_priority(priority)
    section.add_combo = _Combo()
    saved = []
    section._save = lambda new, updates=None, drops=(): saved.append(
        {"priority": new,
         "updates": {"" + key: value for key, value in (updates or {}).items()},
         "drops": ["" + key for key in drops]})
    return section, saved


def _device_list(module, devices):
    listed = _devices(devices)
    return None if listed is None else [_device_dict(device) for device in listed]


def _listed(module, devices):
    return [module._listed(device) for device in _devices(devices)]


def _label(module, entry, devices):
    section, _ = _section(module, [], devices)
    return list(section._label(entry))


def _fill_preferred(module, priority, devices):
    section, _ = _section(module, priority, devices)
    section._fill_preferred()
    combo = section.dialog.audio_device_combo
    return {"items": combo.items, "current": combo.current}


def _fill_add_combo(module, priority, devices):
    section, _ = _section(module, priority, devices)
    section._fill_add_combo()
    combo = section.add_combo
    return {"items": combo.items, "current": combo.current, "enabled": combo.enabled}


def _choose_preferred(module, token, priority, devices):
    section, saved = _section(module, priority, devices)
    section.choose_preferred(token)
    return saved[0] if saved else None


def _move(module, priority, index, offset):
    section, saved = _section(module, priority, "none")
    section._move(index, offset)
    return saved[0]["priority"] if saved else None


def _remove(module, priority, index):
    section, saved = _section(module, priority, "none")
    section._remove(index)
    return saved[0]["priority"] if saved else None


def _on_add_chosen(module, priority, token):
    section, saved = _section(module, priority, "none")
    section.add_combo.addItem("Add a microphone…", None)
    section.add_combo.addItem("Chosen", token)
    section.add_combo.setCurrentIndex(1)
    section._on_add_chosen(1)
    return {"saved": saved[0]["priority"] if saved else None,
            "reset_to": section.add_combo.current}


WRAPPERS = {
    "devices": _device_list,
    "_listed": _listed,
    "MicrophoneSection._label": _label,
    "MicrophoneSection._fill_preferred": _fill_preferred,
    "MicrophoneSection._fill_add_combo": _fill_add_combo,
    "MicrophoneSection.choose_preferred": _choose_preferred,
    "MicrophoneSection._move": _move,
    "MicrophoneSection._remove": _remove,
    "MicrophoneSection._on_add_chosen": _on_add_chosen,
}

SNOWBALL = {"name": "Microphone (Blue Snowball )", "hostapi": "MME"}
USB_MME = {"name": "Microphone (2- USB Audio Device", "hostapi": "MME"}
USB_WASAPI = {"name": "Microphone (2- USB Audio Device)", "hostapi": "Windows WASAPI"}
HEADSET_DS = {"name": "Headset (soundcore V20i)", "hostapi": "Windows DirectSound"}
GONE = {"name": "Microphone (Rode NT-USB)", "hostapi": "MME"}
MAPPER = {"name": "Microsoft Sound Mapper - Input", "hostapi": "MME"}
ALSA_USB = {"name": "USB Audio Device: - (hw:1,0)", "hostapi": "ALSA"}

PRIORITIES = [
    [],
    [SNOWBALL],
    [SNOWBALL, USB_MME],
    [USB_WASAPI, SNOWBALL],
    [GONE],
    [SNOWBALL, GONE, HEADSET_DS],
    [GONE, SNOWBALL, USB_MME, HEADSET_DS],
    [MAPPER, SNOWBALL],
    [ALSA_USB],
]

SETTINGS = [
    {},
    {"audio_input_priority": []},
    {"audio_input_priority": [SNOWBALL]},
    {"audio_input_priority": [SNOWBALL, USB_MME]},
    {"audio_input_priority": [SNOWBALL, USB_MME, HEADSET_DS]},
    {"audio_input_priority": [SNOWBALL, USB_MME, HEADSET_DS, GONE]},
    {"audio_input_priority": [GONE, {"name": "Microphone (Blue Snowball )"}]},
    {"audio_input_priority": [{"name": "  "}, {"name": 5}, "x", SNOWBALL, SNOWBALL,
                              {"name": "Microphone (Blue Snowball )", "hostapi": None}]},
    {"audio_input_priority": "not a list"},
    {"audio_input_priority": [{"name": "Headset Microphone (Jabra Evolve2)", "hostapi": ""}]},
    {"audio_input_priority": [ALSA_USB]},
    {"audio_input_priority": [{"name": "Mikrofon (Größe Straße)", "hostapi": "MME"}]},
]

TOKENS = [
    '["MME", "Microphone (Blue Snowball )"]',
    '["Windows WASAPI", "Microphone (2- USB Audio Device)"]',
    '["", "Headset"]',
    '["MME", ""]',
    '["MME"]',
    '["MME", "a", "b"]',
    '[1, "name"]',
    '["MME", 2]',
    '"ab"',
    '"abc"',
    '{"MME": 1, "Mic": 2}',
    '{"only": 1}',
    "5",
    "null",
    "not json",
    "",
    '["MME", "Mikrofon (Gr\\u00f6\\u00dfe)"]',
    '  ["MME", "x"]  ',
]

DEVICE_SETS = ["windows", "alsa", "none", "empty"]

CASES = {
    "entry_token": [[SNOWBALL], [HEADSET_DS], [{"name": "Mikrofon (Größe)", "hostapi": "MME"}],
                    [{"name": 'Quote " and \\ back', "hostapi": "ALSA"}],
                    [{"name": "", "hostapi": ""}]],
    "token_entry": [[token] for token in TOKENS] + [[None]],
    "saved_priority": [[settings] for settings in SETTINGS],
    "preferred_token": [[settings] for settings in SETTINGS],
    "rail_value": [[settings] for settings in SETTINGS],
    "preferred_name": [[settings] for settings in SETTINGS],
    "devices": [[name] for name in DEVICE_SETS],
    "_listed": [["windows"], ["alsa"]],
    "MicrophoneSection._label": [
        [entry, devices] for entry in (SNOWBALL, USB_MME, USB_WASAPI, HEADSET_DS, GONE, MAPPER,
                                       ALSA_USB)
        for devices in DEVICE_SETS
    ],
    "MicrophoneSection._fill_preferred": [
        [priority, devices] for priority in PRIORITIES for devices in DEVICE_SETS
    ],
    "MicrophoneSection._fill_add_combo": [
        [priority, devices] for priority in PRIORITIES for devices in DEVICE_SETS
    ],
    "MicrophoneSection.choose_preferred": [
        [token, priority, devices]
        for token in ("", TOKENS[0], TOKENS[1], '["Windows DirectSound", "Headset (soundcore '
                      'V20i)"]', '["MME", "Microphone (Rode NT-USB)"]', "not json")
        for priority in ([], [SNOWBALL, USB_MME], [GONE, USB_WASAPI, SNOWBALL])
        for devices in ("windows", "none")
    ],
    "MicrophoneSection._move": [
        [priority, index, offset]
        for priority in ([SNOWBALL], [SNOWBALL, USB_MME], [SNOWBALL, USB_MME, HEADSET_DS, GONE])
        for index in (0, 1, 2, 3, 4)
        for offset in (-1, 1)
    ],
    "MicrophoneSection._remove": [
        [priority, index]
        for priority in ([], [SNOWBALL], [SNOWBALL, USB_MME, HEADSET_DS])
        for index in (0, 1, 2, 3)
    ],
    "MicrophoneSection._on_add_chosen": [
        [priority, token]
        for priority in ([], [SNOWBALL], [SNOWBALL, USB_MME])
        for token in (TOKENS[0], TOKENS[1], '["MME", "Microphone (2- USB Audio Device"]',
                      "not json", None)
    ],
}

PINS = {
    "note": "devices name a list: windows and alsa are tests/fixtures/audio/devices-*.json "
            "through list_input_devices, none is an unknown list, empty no devices; the "
            "'devices' function records each list as the replay builds it",
}
