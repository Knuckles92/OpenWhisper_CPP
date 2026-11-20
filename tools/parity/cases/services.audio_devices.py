"""Curated inputs for services/audio_devices.py (fv_audio's devices.h).

Device lists come from tests/fixtures/audio/devices-*.json, the shapes sounddevice's
query_devices() and query_hostapis() report, behind the same fake sounddevice OpenWhisper's
tests use (tests/test_personalize_s7_audio_devices.py FakeSd): a format check fails for a
refused index, and for a WASAPI device opened without auto_convert. The C++ replay loads the
same files into audio::testing::FakeBackend. Generated on Windows, so ranked_candidates appends
the Sound Mapper.
"""
from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[3]
FIXTURES = REPO / "tests" / "fixtures" / "audio"
WASAPI = "Windows WASAPI"


class FakeSd:
    class WasapiSettings:
        def __init__(self, auto_convert=False):
            self.auto_convert = auto_convert

    def __init__(self, dump: dict, refuse=()):
        self.devices = [dict(d) for d in dump["devices"]]
        self.hostapis = [dict(h) for h in dump["hostapis"]]
        default = dump.get("default", {})
        self.default = SimpleNamespace(device=(default.get("input", -1), default.get("output", -1)),
                                       hostapi=default.get("hostapi", 0))
        self.refuse = set(dump.get("refuse", ())) | set(refuse)

    def query_devices(self):
        return list(self.devices)

    def query_hostapis(self, index=None):
        return list(self.hostapis) if index is None else self.hostapis[index]

    def check_input_settings(self, device=None, channels=None, dtype=None,
                             extra_settings=None, samplerate=None):
        api = self.hostapis[self.devices[device]["hostapi"]]["name"]
        converts = getattr(extra_settings, "auto_convert", False)
        if device in self.refuse or (api == WASAPI and not converts):
            raise Exception("Invalid sample rate [-9997]")


def _sd(fixture: str, refuse=()) -> FakeSd:
    return FakeSd(json.loads((FIXTURES / fixture).read_text(encoding="utf-8")), refuse)


def _device_dict(device) -> dict:
    return {"index": device.index, "name": device.name, "hostapi": device.hostapi,
            "channels": device.channels, "samplerate": device.samplerate,
            "label": device.label, "default_api": device.default_api, "role": device.role,
            "display": device.display}


def _list_input_devices(module, fixture: str):
    return [_device_dict(d) for d in module.list_input_devices(_sd(fixture))]


def _match_entry(module, entry: dict, fixture: str):
    found = module.match_entry(entry, module.list_input_devices(_sd(fixture)))
    return None if found is None else found.index


def _ranked_candidates(module, fixture: str, priority: list, exclude: list, check: bool,
                       refuse: list):
    ranked = module.ranked_candidates(priority, exclude=exclude, sd=_sd(fixture, refuse),
                                      check=check)
    return [[device.index, device.role] for device in ranked]


def _default_input(module, fixture: str):
    sd = _sd(fixture)
    found = module.default_input(sd)
    return None if found is None else found.index


def _is_system_alias(module, name: str):
    return module.is_system_alias(module.InputDevice(index=0, name=name, hostapi="MME"))


def _legacy_migration_pending(module, settings: dict):
    return module.legacy_migration_pending(settings)


WRAPPERS = {
    "list_input_devices": _list_input_devices,
    "match_entry": _match_entry,
    "ranked_candidates": _ranked_candidates,
    "default_input": _default_input,
    "is_system_alias": _is_system_alias,
    "legacy_migration_pending": _legacy_migration_pending,
}

MME, DSOUND, WASAPI_NAME = "MME", "Windows DirectSound", WASAPI
WIN, ALSA = "devices-windows.json", "devices-alsa.json"

NAMES = [
    "Microphone (2- USB Audio Device",
    "Microphone (Blue Snowball )",
    "Microphone Array (Realtek(R) Audio)",
    "MacBook Pro Microphone",
    "HDA Intel PCH: ALC892 Analog (hw:0,0)",
    "Microphone (12- USB Audio Device)",
    "Headset (soundcore V20i)",
    "Headset Microphone (Jabra Link 380)",
    "Line In (Realtek High Definition Audio)",
    "Mic (",
    "Microphone ()",
    "Microphone (USB (",
    "  Microphone (Spaced )  ",
    "Microphone\t(Tab Space )",
    "(3- Leading)",
    "Internal Microphone (Built-in)",
    "MICROPHONE (Upper Case)",
    "Микрофон (2- USB Аудио)",
    "default",
    "",
    "Desk (hw:12,3)  ",
    "Desk(hw:1,2)",
    "Stereo Mix (Realtek(R) Audio)",
]

CASES = {
    "display_name": [[name] for name in NAMES],
    "short_name": [[name] for name in NAMES],
    "hostapi_label": [["Windows DirectSound"], ["Windows WASAPI"], ["MME"], ["Windows"],
                      ["Windows "], ["windows WDM-KS"], ["ALSA"], [""]],
    "device_key": [
        [{"name": "Desk", "hostapi": MME, "index": 4}],
        [{"name": "Desk"}],
        [{"hostapi": MME}],
        [{"name": None, "hostapi": None}],
        [{"name": 3, "hostapi": True}],
        [{"name": "", "hostapi": 0}],
        [{}],
    ],
    "normalize_priority": [
        [None],
        ["junk"],
        [[]],
        [[{"name": "USB mic", "hostapi": MME}, {"name": "USB mic", "hostapi": MME},
          {"name": ""}, "junk", {"name": "Desk", "hostapi": 3}]],
        [[{"name": "  "}, {"name": "A"}, {"name": "A", "hostapi": ""}, {"name": "A", "hostapi": MME},
          {"name": " A ", "hostapi": None}, {"name": "B", "hostapi": WASAPI_NAME, "extra": 1}]],
        [[{"name": " "}, {"name": 5}, [], {"hostapi": MME}]],
    ],
    "list_input_devices": [[WIN], [ALSA]],
    "default_input": [[WIN], [ALSA]],
    "is_system_alias": [["Microsoft Sound Mapper - Input"], ["MICROSOFT SOUND MAPPER - INPUT"],
                        ["Primary Sound Capture Driver"], ["Microphone (Blue Snowball )"], [""]],
    "match_entry": [
        [{"name": "Microphone (Blue Snowball )", "hostapi": MME}, WIN],
        [{"name": "Microphone (3- USB Audio Device)", "hostapi": WASAPI_NAME}, WIN],
        [{"name": "Microphone (4- USB Audio Device", "hostapi": MME}, WIN],
        [{"name": "Microphone (2- USB Audio Device)", "hostapi": "Windows WDM-KS"}, WIN],
        [{"name": "Microphone (Webcam)", "hostapi": MME}, WIN],
        [{"name": "Microphone (USB", "hostapi": MME}, WIN],
        [{"name": "", "hostapi": MME}, WIN],
        [{"name": "Headset (soundcore V20i)", "hostapi": WASAPI_NAME}, WIN],
        [{"name": "Primary Sound Capture Driver", "hostapi": DSOUND}, WIN],
        [{"name": "Microphone (2- USB Audio Device)", "hostapi": DSOUND}, WIN],
        [{"name": "Speakers (Realtek) [Loopback]", "hostapi": WASAPI_NAME}, WIN],
        [{"name": "USB PnP Sound Device: Audio (hw:2,0)", "hostapi": "ALSA"}, ALSA],
        [{"name": "HDA Intel PCH: ALC892 Analog", "hostapi": "ALSA"}, ALSA],
        [{"name": "default", "hostapi": "PulseAudio"}, ALSA],
    ],
    "ranked_candidates": [
        [WIN, [{"name": "Microphone (2- USB Audio Device)", "hostapi": WASAPI_NAME},
               {"name": "Microphone (Gone)", "hostapi": MME},
               {"name": "Microphone (Blue Snowball )", "hostapi": MME}], [], True, []],
        [WIN, [{"name": "Headset (soundcore V20i)", "hostapi": DSOUND},
               {"name": "Microphone (2- USB Audio Device", "hostapi": MME}],
         [{"name": "Microphone (2- USB Audio Device", "hostapi": MME}], True, [7]],
        [WIN, [], [], True, []],
        [WIN, [{"name": "Microphone (2- USB Audio Device)", "hostapi": WASAPI_NAME}], [], False, []],
        [WIN, [{"name": "Microphone (2- USB Audio Device)", "hostapi": WASAPI_NAME}],
         [{"name": "Microphone (2- USB Audio Device)", "hostapi": WASAPI_NAME}], False, []],
        [WIN, [{"name": "Microphone (Blue Snowball )", "hostapi": MME},
               {"name": "Microphone (Blue Snowball )", "hostapi": MME}], [], True, [1]],
        [WIN, [{"name": "Microsoft Sound Mapper - Input", "hostapi": MME}], [], True, []],
        [WIN, [], [{"name": "Microphone (Blue Snowball )", "hostapi": MME},
                   {"name": "Microsoft Sound Mapper - Input", "hostapi": MME}], True, []],
        [ALSA, [{"name": "USB PnP Sound Device: Audio (hw:2,0)", "hostapi": "ALSA"}], [], True, []],
    ],
    "legacy_migration_pending": [
        [{}], [{"audio_input_device": 2}], [{"audio_input_device": True}],
        [{"audio_input_device": 2.0}], [{"audio_input_device": "2"}],
        [{"audio_input_device": 2, "audio_input_priority": []}],
        [{"audio_input_device": -1}], [{"audio_input_priority": None}],
    ],
}

TRACE = {
    "tests": ["tests/test_personalize_s7_audio_devices.py"],
    "functions": ["display_name", "short_name", "hostapi_label", "normalize_priority",
                  "device_key"],
}
