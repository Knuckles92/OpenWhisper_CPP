"""Curated inputs for services/automation/models.py (M5.3 automation-core).

Hooks are given as dicts of their dataclass fields (tuples as lists). Paths follow the platform
the vectors were made on (pins.platform): the replay of save_* and validate_* for programs and
filters runs only there.

    load_webhooks(rows) ...           [h.to_dict() for h in load_webhooks({key: rows})]
    validate_url(url)                 None, or the ValueError's message
    validate_webhook(hook, settings)  None, or the ValueError's message (same for the others)
    save_webhook(settings, hook)      the settings afterwards, or ["ValueError", message]
    delete_webhook(settings, id)      the settings afterwards
    find_webhook(settings, id)        the hook's to_dict() or None
"""

from services.settings import SettingsKey

_KEYS = {
    "webhook": SettingsKey.AUTOMATION_WEBHOOKS,
    "program": SettingsKey.AUTOMATION_PROGRAMS,
    "filter": SettingsKey.AUTOMATION_FILTERS,
}


def _message(fn, *args):
    try:
        fn(*args)
    except ValueError as exc:
        return str(exc)
    return None


def _hook(module, spec):
    spec = dict(spec)
    spec["events"] = tuple(spec.get("events", ()))
    return module.Webhook(**spec)


def _program(module, spec):
    spec = dict(spec)
    spec["args"] = tuple(spec.get("args", ()))
    spec["events"] = tuple(spec.get("events", ()))
    return module.Program(**spec)


def _filter(module, spec):
    spec = dict(spec)
    spec["args"] = tuple(spec.get("args", ()))
    return module.TextFilter(**spec)


_BUILD = {"webhook": _hook, "program": _program, "filter": _filter}


def _saved(module, noun, settings, spec):
    settings = dict(settings)
    try:
        getattr(module, f"save_{noun}")(settings, _BUILD[noun](module, spec))
    except ValueError as exc:
        return ["ValueError", str(exc)]
    return settings


def _deleted(module, noun, settings, item_id):
    settings = dict(settings)
    getattr(module, f"delete_{noun}")(settings, item_id)
    return settings


def _found(module, noun, settings, item_id):
    item = getattr(module, f"find_{noun}")(settings, item_id)
    return None if item is None else item.to_dict()


def _loaded(module, noun, rows):
    return [item.to_dict() for item in getattr(module, f"load_{noun}s")({_KEYS[noun]: rows})]


WRAPPERS = {"validate_url": lambda m, url: _message(m.validate_url, url)}
for _noun in _KEYS:
    WRAPPERS[f"load_{_noun}s"] = lambda m, rows, _n=_noun: _loaded(m, _n, rows)
    WRAPPERS[f"validate_{_noun}"] = lambda m, spec, settings, _n=_noun: _message(
        getattr(m, f"validate_{_n}"), _BUILD[_n](m, spec), settings)
    WRAPPERS[f"save_{_noun}"] = lambda m, settings, spec, _n=_noun: _saved(m, _n, settings, spec)
    WRAPPERS[f"delete_{_noun}"] = lambda m, settings, item_id, _n=_noun: _deleted(
        m, _n, settings, item_id)
    WRAPPERS[f"find_{_noun}"] = lambda m, settings, item_id, _n=_noun: _found(
        m, _n, settings, item_id)
WRAPPERS["secret_name"] = lambda m, hook_id: m.webhook_secret_name(hook_id)

_EXE = "C:\\Tools\\hook.exe"
_EVENTS = ["history.saved"]
_HOOKS = [{"id": f"h{n}", "name": f"hook {n}", "url": "https://a.example", "events": _EVENTS}
          for n in range(20)]
_PROGRAMS = [{"id": f"p{n}", "name": f"prog {n}", "program": _EXE, "events": _EVENTS}
             for n in range(20)]
_FILTERS = [{"id": f"f{n}", "name": f"filter {n}", "kind": "http", "stage": "after_cleanup",
             "url": "http://127.0.0.1:9000/f"} for n in range(10)]
_SAVED = {SettingsKey.AUTOMATION_WEBHOOKS: [
    {"id": "h1", "name": "n8n", "url": "http://localhost:5678/hook", "events": _EVENTS,
     "enabled": True},
    {"id": "h2", "name": "Home", "url": "https://ha.example/hook",
     "events": ["recording.started", "history.saved"], "enabled": False}]}
_SAVED_PROGRAMS = {SettingsKey.AUTOMATION_PROGRAMS: [
    {"id": "p1", "name": "Journal", "program": _EXE, "args": ["--append"], "events": _EVENTS,
     "enabled": True, "timeout_s": 30}]}
_SAVED_FILTERS = {SettingsKey.AUTOMATION_FILTERS: [
    {"id": "f1", "name": "Fix names", "kind": "http", "stage": "after_cleanup",
     "url": "http://127.0.0.1:9000/filter", "program": "", "args": [], "timeout_ms": 1500,
     "enabled": True}]}

CASES = {
    "load_webhooks": [
        [None], [[]], ["x"], [_SAVED[SettingsKey.AUTOMATION_WEBHOOKS]],
        [[{"id": " a ", "name": " A ", "url": " http://x ", "events": "history.saved"},
          {"id": "a", "name": "dup", "url": "http://y"},
          {"id": "b", "name": "", "url": "http://y"}, {"id": "c", "name": "C", "url": ""},
          {"id": 5, "name": "N", "url": "http://n"}, "junk", None, ["list"],
          {"id": "d", "name": "D", "url": "http://d", "enabled": "yes",
           "events": ["nope", "history.saved", 5, "events.gap", "history.saved",
                      "recording.started"]},
          {"id": "e", "name": "E", "url": "http://e", "enabled": False, "events": None}]],
        [[{"id": f"h{n}", "name": f"H{n}", "url": "http://h",
           "events": ["nope"] * 64 + ["history.saved"]} for n in range(3)]],
        [[{"id": f"h{n}", "name": f"H{n}", "url": "http://h"} for n in range(25)]],
    ],
    "load_programs": [
        [None], [_SAVED_PROGRAMS[SettingsKey.AUTOMATION_PROGRAMS]],
        [[{"id": "a", "name": "A", "program": "x.exe", "timeout_s": 0},
          {"id": "b", "name": "B", "program": "x.exe", "timeout_s": 999},
          {"id": "c", "name": "C", "program": "x.exe", "timeout_s": True},
          {"id": "d", "name": "D", "program": "x.exe", "timeout_s": 12.5},
          {"id": "e", "name": "E", "program": "x.exe", "timeout_s": "45"},
          {"id": "f", "name": "F", "program": "x.exe", "timeout_s": -4},
          {"id": "g", "name": "G", "program": " ", "timeout_s": 5},
          {"id": "h", "name": "H", "program": "y", "args": ["a", 1, None, " b ", "c"] * 10},
          {"id": "i", "name": "I", "program": "z", "timeout_s": 300, "args": "notalist"}]],
        [[{"id": f"p{n}", "name": f"P{n}", "program": "x"} for n in range(22)]],
    ],
    "load_filters": [
        [None],
        [[{"id": "a", "name": "A", "kind": "shell", "stage": "after_cleanup", "url": "x"},
          {"id": "b", "name": "B", "kind": "http", "stage": "later", "url": "x"},
          {"id": "c", "name": "C", "kind": "http", "stage": "after_cleanup"},
          "junk",
          {"id": "d", "name": "D", "kind": "http", "stage": "after_cleanup", "url": "http://x",
           "timeout_ms": 99999, "enabled": "yes"},
          {"id": "e", "name": "E", "kind": " program ", "stage": "before_cleanup",
           "program": "p", "timeout_ms": 5},
          {"id": "f", "name": "F", "kind": "program", "stage": "before_cleanup",
           "url": "http://ignored", "program": ""},
          {"id": "g", "name": "G", "kind": "program", "stage": " before_cleanup ",
           "program": " p.exe ", "args": ["--x"], "timeout_ms": 4999.9},
          {"id": "d", "name": "Dup", "kind": "http", "stage": "after_cleanup", "url": "http://d"},
          {"id": "h", "name": "H", "kind": "HTTP", "stage": "after_cleanup", "url": "http://h"}]],
        [[{"id": f"f{n}", "name": f"F{n}", "kind": "http", "stage": "after_cleanup",
           "url": "http://f"} for n in range(12)]],
    ],
    "validate_url": [
        [""], ["http://localhost:1/x"], ["https://hooks.example.com/x"], ["ftp://a.example"],
        ["nope"], ["http://"], ["http:///path"], ["https://a.example/x y"],
        ["https://a.example/\u3000"], ["https://" + "a" * 1990 + ".com"],
        ["https://" + "a" * 1991 + ".com"], ["http://[::1]:8080/x"], ["http://[::1/x"],
        ["http://::1]/x"], ["http://[127.0.0.1]/"], ["http://[v1.fe]/"], ["http://[vz.x]/"],
        ["HTTP://Example.COM"], ["  https://lead.example"], ["https://tab\t.example"],
        ["https://user:pw@host.example:99/p?q#f"], ["https://a.example\n"], ["javascript:x"],
        ["http://x@[::1]junk/"], ["http://[fe80::1%25eth0]/"], ["mailto:a@b"],
    ],
    "url_is_private": [
        ["http://localhost:1/x"], ["http://127.0.0.1/x"], ["http://192.168.1.20/x"],
        ["http://homeassistant.local:8123"], ["https://hooks.example.com/x"], ["http://8.8.8.8"],
        ["http://LOCALHOST"], ["http://box.lan/x"], ["http://box.home.arpa"], ["http://a.localhost"],
        ["http://localhost.example"], ["http://10.0.0.1"], ["http://172.16.5.4"],
        ["http://172.32.0.1"], ["http://169.254.1.1"], ["http://100.64.0.1"], ["http://0.0.0.0"],
        ["http://[::1]:8080/"], ["http://[fe80::1]/"], ["http://[fc00::1]/"], ["http://[2001:db8::1]/"],
        ["http://[2606:4700::1111]/"], ["http://[::ffff:192.168.0.1]/"], ["http://[::ffff:8.8.8.8]/"],
        ["http://[fe80::1%25eth0]/"], ["http://192.0.0.9"], ["http://192.0.0.8"],
        ["http://010.0.0.1"], ["http://1.2.3"], ["http://[::1/"], ["not a url"], [""],
        ["http://255.255.255.255"], ["http://240.0.0.1"], ["http://198.18.0.1"],
        ["http://[2001:4:112::1]/"], ["http://[64:ff9b:1::1]/"], ["http://[::]/"],
        ["http://[::ffff:127.0.0.1]/"], ["http://\u0661.1.1.1"],
    ],
    "validate_webhook": [
        [{"id": "h1", "name": "n8n", "url": "http://localhost:5678/hook", "events": _EVENTS}, {}],
        [{"id": "h3", "name": "N8N", "url": "https://a.example", "events": _EVENTS}, _SAVED],
        [{"id": "h1", "name": "N8N", "url": "https://a.example", "events": _EVENTS}, _SAVED],
        [{"id": "h3", "name": "x", "url": "ftp://a.example", "events": _EVENTS}, _SAVED],
        [{"id": "h3", "name": "x", "url": "https://a.example", "events": []}, _SAVED],
        [{"id": "h3", "name": "x", "url": "https://a.example", "events": ["nope", "events.gap"]},
         _SAVED],
        [{"id": "h3", "name": "  ", "url": "https://a.example", "events": _EVENTS}, _SAVED],
        [{"id": "h3", "name": "n" * 81, "url": "https://a.example", "events": _EVENTS}, {}],
        [{"id": "h3", "name": "\u00e9" * 80, "url": "https://a.example", "events": _EVENTS}, {}],
        [{"id": "x", "name": "extra", "url": "https://a.example", "events": _EVENTS},
         {SettingsKey.AUTOMATION_WEBHOOKS: _HOOKS}],
        [{"id": "h0", "name": "renamed", "url": "https://a.example", "events": _EVENTS},
         {SettingsKey.AUTOMATION_WEBHOOKS: _HOOKS}],
        [{"id": "h3", "name": "Stra\u00dfe", "url": "https://a.example", "events": _EVENTS},
         {SettingsKey.AUTOMATION_WEBHOOKS: [{"id": "h9", "name": "STRASSE", "url": "http://s"}]}],
    ],
    "validate_program": [
        [{"id": "p", "name": "Script", "program": "python", "events": _EVENTS}, {}],
        [{"id": "p", "name": "Script", "program": "", "events": _EVENTS}, {}],
        [{"id": "p", "name": "Script", "program": _EXE, "args": ["x.py"], "events": _EVENTS,
          "timeout_s": 10}, {}],
        [{"id": "p", "name": "Script", "program": _EXE, "events": _EVENTS, "timeout_s": 0}, {}],
        [{"id": "p", "name": "Script", "program": _EXE, "events": _EVENTS, "timeout_s": 301}, {}],
        [{"id": "p", "name": "Script", "program": _EXE, "events": [], "timeout_s": 300}, {}],
        [{"id": "p", "name": "Script", "program": _EXE, "args": ["a"] * 33, "events": _EVENTS},
         {}],
        [{"id": "p", "name": "Script", "program": _EXE, "args": ["a" * 1001], "events": _EVENTS},
         {}],
        [{"id": "p", "name": "Script", "program": "\\\\server\\share\\x.exe", "events": _EVENTS},
         {}],
        [{"id": "p", "name": "Script", "program": "/usr/bin/x", "events": _EVENTS}, {}],
        [{"id": "p", "name": "Script", "program": "C:x.exe", "events": _EVENTS}, {}],
        [{"id": "p", "name": "Script", "program": "C:/x.exe", "events": _EVENTS}, {}],
        [{"id": "p2", "name": "journal", "program": _EXE, "events": _EVENTS}, _SAVED_PROGRAMS],
        [{"id": "x", "name": "extra", "program": _EXE, "events": _EVENTS},
         {SettingsKey.AUTOMATION_PROGRAMS: _PROGRAMS}],
    ],
    "validate_filter": [
        [{"id": "f2", "name": "Slow", "kind": "http", "stage": "before_cleanup",
          "url": "http://127.0.0.1:1", "timeout_ms": 9000}, _SAVED_FILTERS],
        [{"id": "f2", "name": "Fast", "kind": "http", "stage": "before_cleanup",
          "url": "http://127.0.0.1:1", "timeout_ms": 99}, {}],
        [{"id": "f2", "name": "Ok", "kind": "http", "stage": "before_cleanup",
          "url": "http://127.0.0.1:1", "timeout_ms": 100}, {}],
        [{"id": "f2", "name": "Shell", "kind": "shell", "stage": "before_cleanup"}, {}],
        [{"id": "f2", "name": "Later", "kind": "http", "stage": "later", "url": "http://x"}, {}],
        [{"id": "f2", "name": "NoUrl", "kind": "http", "stage": "after_cleanup"}, {}],
        [{"id": "f3", "name": "Local", "kind": "program", "stage": "before_cleanup",
          "program": _EXE}, {}],
        [{"id": "f3", "name": "Local", "kind": "program", "stage": "before_cleanup",
          "program": "rel.exe"}, {}],
        [{"id": "f3", "name": "fix NAMES", "kind": "program", "stage": "before_cleanup",
          "program": _EXE}, _SAVED_FILTERS],
        [{"id": "x", "name": "extra", "kind": "http", "stage": "after_cleanup",
          "url": "http://x"}, {SettingsKey.AUTOMATION_FILTERS: _FILTERS}],
    ],
    "save_webhook": [
        [{}, {"id": "h1", "name": "n8n", "url": "http://localhost:5678/hook",
              "events": ["history.saved", "bogus"]}],
        [_SAVED, {"id": " h2 ", "name": " Renamed ", "url": " https://b.example ",
                  "events": ["recording.started", "recording.started"], "enabled": True}],
        [_SAVED, {"id": "h3", "name": "New", "url": "https://c.example", "events": _EVENTS,
                  "enabled": False}],
        [_SAVED, {"id": "  ", "name": "New", "url": "https://c.example", "events": _EVENTS}],
        [_SAVED, {"id": "h3", "name": "N8N", "url": "https://c.example", "events": _EVENTS}],
        [{"other": 1}, {"id": "h1", "name": "x", "url": "https://a.example", "events": []}],
    ],
    "save_program": [
        [{}, {"id": "p", "name": "Script", "program": _EXE, "args": ["x.py"], "events": _EVENTS,
              "timeout_s": 10}],
        [_SAVED_PROGRAMS, {"id": "p1", "name": " Journal ", "program": " " + _EXE + " ",
                           "args": [" --x "], "events": _EVENTS, "timeout_s": 300}],
        [{}, {"id": "", "name": "Script", "program": _EXE, "events": _EVENTS}],
    ],
    "save_filter": [
        [{}, {"id": "f1", "name": "Fix names", "kind": "http", "stage": "after_cleanup",
              "url": "http://127.0.0.1:9000/filter"}],
        [_SAVED_FILTERS, {"id": "f3", "name": "Local", "kind": "program",
                          "stage": "before_cleanup", "program": _EXE, "args": ["-q"]}],
        [_SAVED_FILTERS, {"id": "f1", "name": "Fix names", "kind": "http",
                          "stage": "after_cleanup", "url": " http://127.0.0.1:9001/ ",
                          "timeout_ms": 5000, "enabled": False}],
        [{}, {"id": " ", "name": "x", "kind": "http", "stage": "after_cleanup",
              "url": "http://x"}],
    ],
    "delete_webhook": [[_SAVED, "h1"], [_SAVED, "nope"], [{}, "h1"]],
    "delete_program": [[_SAVED_PROGRAMS, "p1"], [{}, "p1"]],
    "delete_filter": [[_SAVED_FILTERS, "f1"], [_SAVED_FILTERS, "x"]],
    "find_webhook": [[_SAVED, "h2"], [_SAVED, "H2"], [{}, "h1"]],
    "find_program": [[_SAVED_PROGRAMS, "p1"], [_SAVED_PROGRAMS, "p2"]],
    "find_filter": [[_SAVED_FILTERS, "f1"], [_SAVED_FILTERS, "f2"]],
    "secret_name": [["h1"], [""]],
}
