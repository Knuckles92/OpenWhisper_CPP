"""The loader of services/actions/model.py (M3.1 backup-create ports it for the backup sanitizer).

    load_actions(raw)    [action.to_dict() for action in load_actions({"actions": raw})]

Shortcuts normalize through the platform's vocabulary (pins.platform); the replay compares the
"hotkey" field only on that platform.
"""


def _load_actions(module, raw):
    return [action.to_dict() for action in module.load_actions({"actions": raw})]


WRAPPERS = {"load_actions": _load_actions}

_WELL_FORMED = [
    {"id": "l", "name": "Ask Claude", "kind": "open_link",
     "params": {"template": "claude://claude.ai/new?q={text}"}, "hotkey": "ctrl+alt+c",
     "shaping": "cleanup", "spoken_names": ["claude"], "description": "Open a chat.",
     "enabled": True},
    {"id": "a", "name": "Agent", "kind": "agent",
     "params": {"agent": "codex", "mode": "ask", "surface": "terminal", "folder": "C:\\work",
                "new_folder": True, "include_selection": True, "model": "gpt", "output": "paste"}},
    {"id": "ch", "name": "Channel", "kind": "channel", "params": {"ignored": 1}},
    {"id": "w", "name": "Hook", "kind": "webhook", "params": {"url": "https://h.example/x",
                                                               "output": "notice"}},
    {"id": "p", "name": "Prog", "kind": "program",
     "params": {"program": "C:\\x.exe", "args": ["/c", "calc"], "output": "none",
                "timeout_s": 45}},
    {"id": "c", "name": "Copy", "kind": "clipboard"},
    {"id": "s", "name": "Scripts", "kind": "scripts", "enabled": False},
]

CASES = {
    "load_actions": [
        [_WELL_FORMED],
        [None], [{}], ["actions"], [5], [[]],
        # Kinds as people misspell them.
        [[{"id": "x1", "name": "Notes", "kind": "program ",
           "params": {"program": "C:\\Windows\\System32\\cmd.exe", "args": ["/c", "calc"],
                      "output": "none", "timeout_s": 30},
           "hotkey": "ctrl+alt+9", "spoken_names": ["notes"], "enabled": True},
          {"id": "x2", "name": "Hook", "kind": " Webhook", "params": {"url": "https://h"}},
          {"id": "x3", "name": "Shell", "kind": "shell"},
          {"id": "x4", "name": "Up", "kind": "PROGRAM"},
          {"id": "x5", "name": "Tab", "kind": "\tclipboard\n"}]],
        # Wrong types.
        [[{"id": 5, "name": "N", "kind": "clipboard"},
          {"id": "a", "name": "", "kind": "clipboard"},
          {"id": "b", "name": "  ", "kind": "clipboard"},
          {"id": "c", "name": "C", "kind": None},
          {"id": "d", "name": "D", "kind": "clipboard", "params": ["x"], "hotkey": 5,
           "shaping": 3, "spoken_names": "names", "description": 7, "enabled": "yes"},
          {"id": "e", "name": "E", "kind": "program",
           "params": {"program": 5, "args": "notalist", "output": None, "timeout_s": True}},
          {"id": "f", "name": "F", "kind": "program",
           "params": {"program": " p.exe ", "args": [1, "a", None, "b", " c "],
                      "output": " paste ", "timeout_s": 30.0}},
          {"id": "g", "name": "G", "kind": "agent",
           "params": {"new_folder": 1, "include_selection": 0, "folder": None, "mode": " ask "}},
          {"id": "h", "name": "H", "kind": "program",
           "params": {"args": ["x"] * 40, "timeout_s": -3}},
          "string row", 7, None, ["list row"]]],
        # Repeated ids: the first wins, after stripping.
        [[{"id": "a", "name": "First", "kind": "clipboard"},
          {"id": " a ", "name": "Second", "kind": "clipboard"},
          {"id": "A", "name": "Third", "kind": "clipboard"}]],
        # Text cleaning.
        [[{"id": "  t ", "name": "  Name  ", "kind": "clipboard", "shaping": "  ",
           "description": "  " + "d" * 310 + "  ",
           "spoken_names": ["  two   words  ", "Two Words", "", "   ", 5, "x" * 50,
                            "Stra\u00dfe", "STRASSE", "a", "b", "c", "d", "e", "f", "g", "h"]},
          {"id": "u", "name": "U", "kind": "clipboard", "shaping": " profile:abc ",
           "description": "caf\u00e9 \U0001f600" * 40, "spoken_names": ["\u00e9" * 45]},
          {"id": "v", "name": "V", "kind": "clipboard", "shaping": "transform:t"}]],
        # Shortcuts.
        [[{"id": "k1", "name": "K1", "kind": "clipboard", "hotkey": "Alt+Ctrl+X"},
          {"id": "k2", "name": "K2", "kind": "clipboard", "hotkey": "  "},
          {"id": "k3", "name": "K3", "kind": "clipboard", "hotkey": "kp_1"},
          {"id": "k4", "name": "K4", "kind": "clipboard", "hotkey": "ctrl+shift+kp_add"},
          {"id": "k5", "name": "K5", "kind": "clipboard", "hotkey": "win+F5"},
          {"id": "k6", "name": "K6", "kind": "clipboard", "hotkey": "ctrl+"},
          {"id": "k7", "name": "K7", "kind": "clipboard", "hotkey": "nonsense+zz"},
          {"id": "k8", "name": "K8", "kind": "clipboard", "hotkey": " ctrl + alt + a "}]],
        # More than MAX_ACTIONS rows: the first 50 kept.
        [[{"id": f"r{i}", "name": f"R{i}", "kind": "clipboard"} for i in range(55)]],
        [[{"id": "dup", "name": "D", "kind": "clipboard"}] * 3
         + [{"id": f"q{i}", "name": "Q", "kind": "scripts"} for i in range(52)]],
    ],
}


# -- M5.3 automation-core: the rest of the module --------------------------------------------
#
#     validate_action(action, settings)      None, or the ValueError's message
#     save_action(settings, action)          the settings afterwards, or ["ValueError", message]
#     delete_action(settings, action_id)     the settings afterwards
#     find_action(settings, action_id)       the action's to_dict() or None
#     expand_link(template, text)            the link, or ["ValueError", message]
#     public_dict(action) / param(action, key) / shaping_ids(action)
#     presets() / tables()                   PRESETS and the module's tables, as data
#
# An action is a dict of Action fields (spoken_names as a list). Paths and shortcuts follow the
# platform the vectors were made on (pins.platform); the replay compares those cases only there.


def _action(module, spec):
    spec = dict(spec)
    if "spoken_names" in spec:
        spec["spoken_names"] = tuple(spec["spoken_names"])
    return module.Action(**spec)


def _message(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except ValueError as exc:
        return str(exc)
    return None


def _saved(module, settings, spec):
    settings = dict(settings)
    try:
        module.save_action(settings, _action(module, spec))
    except ValueError as exc:
        return ["ValueError", str(exc)]
    return settings


def _deleted(module, settings, action_id):
    settings = dict(settings)
    module.delete_action(settings, action_id)
    return settings


def _found(module, settings, action_id):
    action = module.find_action(settings, action_id)
    return None if action is None else action.to_dict()


def _expanded(module, template, text):
    try:
        return module.expand_link(template, text)
    except ValueError as exc:
        return ["ValueError", str(exc)]


def _presets(module):
    return [{"id": p.id, "name": p.name, "kind": p.kind, "params": dict(p.params),
             "description": p.description, "icon": p.icon, "shaping": p.shaping, "note": p.note}
            for p in module.PRESETS]


def _tables(module):
    return {
        "ActionKind": list(module.ActionKind.ALL), "Output": list(module.Output.ALL),
        "AgentMode": list(module.AgentMode.ALL), "AgentSurface": list(module.AgentSurface.ALL),
        "AGENTS": list(module.AGENTS), "AGENT_NAMES": dict(module.AGENT_NAMES),
        "PARAMS": {kind: dict(params) for kind, params in module.PARAMS.items()},
        "REFUSED_SCHEMES": sorted(module.REFUSED_SCHEMES),
        "limits": [module.MAX_ACTIONS, module.NAME_MAX_CHARS, module.DESCRIPTION_MAX_CHARS,
                   module.MAX_SPOKEN_NAMES, module.SPOKEN_NAME_MAX_CHARS,
                   module.TEMPLATE_MAX_CHARS],
    }


WRAPPERS.update({
    "validate_action": lambda m, spec, settings: _message(
        m.validate_action, _action(m, spec), settings),
    "save_action": _saved,
    "delete_action": _deleted,
    "find_action": _found,
    "expand_link": _expanded,
    "public_dict": lambda m, spec: _action(m, spec).public_dict(),
    "param": lambda m, spec, key: _action(m, spec).param(key),
    "shaping_ids": lambda m, spec: [_action(m, spec).profile_id(),
                                    _action(m, spec).transform_id()],
    "presets": _presets,
    "tables": _tables,
    "secret_name": lambda m, action_id: m.action_secret_name(action_id),
    "hotkey_conflict": lambda m, hotkey, settings, exclude_id: m.action_hotkey_conflict(
        hotkey, settings, exclude_id=exclude_id),
})

_LINK = {"id": "a1", "name": "Ask Claude", "kind": "open_link",
         "params": {"template": "claude://claude.ai/new?q={text}"}}
_EXE = "C:\\Tools\\journal.exe"
_TRANSFORMS = {"text_transforms": [
    {"id": "t", "name": "Polish", "instruction": "x", "hotkey": "ctrl+alt+p"}]}
_PROFILES = {"transcript_cleanup_profiles": [
    {"id": "email", "name": "Email", "instructions": "Write an email.", "hotkey": "ctrl+alt+e"}]}
_SAVED = {"actions": [
    {"id": "a1", "name": "Ask Claude", "kind": "open_link",
     "params": {"template": "claude://claude.ai/new?q={text}"}, "hotkey": "ctrl+alt+k",
     "shaping": "cleanup", "spoken_names": ["Claude", "assistant"], "description": "",
     "enabled": True},
    {"id": "w", "name": "Home", "kind": "webhook",
     "params": {"url": "https://ha.example/hook", "output": "none"}, "hotkey": "",
     "shaping": "raw", "spoken_names": ["house", "Claude"], "description": "", "enabled": True}]}
_FULL = {"actions": [{"id": f"r{n}", "name": f"R{n}", "kind": "clipboard"} for n in range(50)]}


def _with(spec, **changes):
    merged = dict(spec)
    merged.update(changes)
    return merged


CASES.update({
    "validate_action": [
        [_LINK, {}],
        [_with(_LINK, name="  "), {}],
        [_with(_LINK, name="n" * 81), {}],
        [_with(_LINK, name="\u00e9" * 80), {}],
        [_with(_LINK, kind="shell"), {}],
        [_with(_LINK, description="d" * 301), {}],
        [_with(_LINK, id="a2", name="ask claude"), _SAVED],
        [_with(_LINK, id="a2", name="Other", spoken_names=["CLAUDE"]), _SAVED],
        [_with(_LINK, id="a2", name="Other", spoken_names=["House"]), _SAVED],
        [_with(_LINK, id="a2", name="Assistant"), _SAVED],
        [_with(_LINK, id="a1", name="Home"), _SAVED],
        [_with(_LINK, id="a1", name="Renamed", spoken_names=["claude"]), _SAVED],
        [_with(_LINK, id="new"), _FULL],
        [_with(_LINK, id="r3", name="R3 renamed"), _FULL],
        [_with(_LINK, shaping="profile:nope"), {}],
        [_with(_LINK, shaping="profile:email"), _PROFILES],
        [_with(_LINK, shaping="transform:nope"), {"text_transforms": []}],
        [_with(_LINK, shaping="transform:polish"), {}],
        [_with(_LINK, shaping="transform:t"), _TRANSFORMS],
        [_with(_LINK, shaping="fancy"), {}],
        [_with(_LINK, shaping=""), {}],
        [_with(_LINK, shaping="raw"), {}],
        [_with(_LINK, params={}), {}],
        [_with(_LINK, params={"template": "t" * 2001}), {}],
        [_with(_LINK, params={"template": "file:///{text}"}), {}],
        [_with(_LINK, params={"template": "no scheme {text}"}), {}],
        [_with(_LINK, params={"template": "obsidian://new?content={text}"}), {}],
        [{"id": "x", "name": "X", "kind": "agent", "params": {"agent": "gpt"}}, {}],
        [{"id": "x", "name": "X", "kind": "agent", "params": {"mode": "chat"}}, {}],
        [{"id": "x", "name": "X", "kind": "agent", "params": {"surface": "web"}}, {}],
        [{"id": "x", "name": "X", "kind": "agent", "params": {"folder": "relative/dir"}}, {}],
        [{"id": "x", "name": "X", "kind": "agent",
          "params": {"folder": "relative/dir", "new_folder": True}}, {}],
        [{"id": "x", "name": "X", "kind": "agent", "params": {"folder": "C:\\work"}}, {}],
        [{"id": "x", "name": "X", "kind": "agent", "params": {"output": "email"}}, {}],
        [{"id": "x", "name": "X", "kind": "agent", "params": {}}, {}],
        [{"id": "x", "name": "X", "kind": "webhook", "params": {"url": "nope"}}, {}],
        [{"id": "x", "name": "X", "kind": "webhook", "params": {}}, {}],
        [{"id": "x", "name": "X", "kind": "webhook",
          "params": {"url": "https://h.example", "output": "fax"}}, {}],
        [{"id": "x", "name": "X", "kind": "program", "params": {"program": "python"}}, {}],
        [{"id": "x", "name": "X", "kind": "program", "params": {}}, {}],
        [{"id": "x", "name": "X", "kind": "program",
          "params": {"program": _EXE, "output": "fax"}}, {}],
        [{"id": "x", "name": "X", "kind": "program",
          "params": {"program": _EXE, "timeout_s": 0}}, {}],
        [{"id": "x", "name": "X", "kind": "program",
          "params": {"program": _EXE, "timeout_s": 301}}, {}],
        [{"id": "x", "name": "X", "kind": "program",
          "params": {"program": _EXE, "timeout_s": "45"}}, {}],
        [{"id": "x", "name": "X", "kind": "program",
          "params": {"program": _EXE, "timeout_s": 12.9}}, {}],
        [{"id": "x", "name": "X", "kind": "channel", "params": {"anything": 1}}, {}],
        [_with(_LINK, hotkey="ctrl+alt+p"), _TRANSFORMS],
        [_with(_LINK, hotkey="ctrl+alt+e"), _PROFILES],
        [_with(_LINK, hotkey="kp *"), {}],
        [_with(_LINK, id="a9", name="Nine", hotkey="ctrl+alt+k"), _SAVED],
        [_with(_LINK, hotkey="ctrl+alt+k"), _SAVED],
    ],
    "save_action": [
        [{}, _with(_LINK, params={"template": "claude://claude.ai/new?q={text}", "evil": "x"},
                   spoken_names=["Claude", "claude", " ask   Claude "])],
        [{}, {"id": "ag", "name": "Agent", "kind": "agent",
              "params": {"agent": "codex", "folder": "C:\\work"}}],
        [{}, {"id": "ag", "name": "Agent", "kind": "agent",
              "params": {"agent": "codex", "folder": "C:\\work", "new_folder": True}}],
        [_SAVED, _with(_LINK, name=" Ask Claude again ", hotkey=" Alt+Ctrl+K ",
                       shaping="  raw  ", description="  short  ", enabled=False)],
        [_SAVED, {"id": "  new  ", "name": "Copy", "kind": "clipboard", "shaping": "  "}],
        [_SAVED, {"id": "  ", "name": "Copy", "kind": "clipboard"}],
        [_SAVED, {"id": "c", "name": "Copy", "kind": "clipboard", "shaping": ""}],
        [{}, {"id": "p", "name": "Prog", "kind": "program",
              "params": {"program": " " + _EXE + " ", "args": ["a", 1, " b "], "timeout_s": 45,
                         "output": "notice"}}],
        [{}, {"id": "p", "name": "Prog", "kind": "program",
              "params": {"program": _EXE, "timeout_s": 45.0}}],
        [{}, _with(_LINK, params={"template": "file:///{text}"})],
        [{}, {"id": "x", "name": "X", "kind": "shell"}],
        [{}, _with(_LINK, spoken_names=["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"])],
        [{}, _with(_LINK, description="  " + "d" * 300 + "  ")],
        [_FULL, {"id": "r0", "name": "First", "kind": "scripts"}],
    ],
    "delete_action": [[_SAVED, "a1"], [_SAVED, "nope"], [{}, "a1"], [_FULL, "r49"]],
    "find_action": [[_SAVED, "w"], [_SAVED, "W"], [{}, "a1"]],
    "link_scheme": [
        ["claude://claude.ai/new?q={text}"], ["HTTPS://x"], ["no scheme {text}"], [""],
        [":x"], ["1abc:x"], ["a+b-c.d:x"], ["a_b:x"], ["\u00e9t\u00e9:x"], ["mailto:x"],
        ["javascript:alert(1)"], ["a"], ["x y:z"], ["Obsidian://new"],
    ],
    "expand_link": [
        ["claude://claude.ai/new?q={text}", "a & b?/c"],
        ["https://chatgpt.com/?q={text}", "caf\u00e9 \U0001f600 ~._-"],
        ["https://x/{text}/{text}", "two"], ["https://x/", "unused"],
        ["https://x/?q={text}", ""], ["https://x/?q={text}", None],
        ["file:///etc/passwd", "x"], ["javascript:alert(1)", "x"], ["DATA:text/html,x", "x"],
        ["no scheme {text}", "x"], ["", "x"], ["vbscript:x", "x"], ["about:blank", "x"],
        ["blob:x", "x"], ["Blob:x", "x"], ["obsidian://new?content={text}", "line1\nline2"],
    ],
    "public_dict": [
        [{"id": "ag", "name": "Agent", "kind": "agent",
          "params": {"agent": "codex", "folder": "C:\\"}}],
        [{"id": "w", "name": "Hook", "kind": "webhook",
          "params": {"url": "https://x.example/secret"}}],
        [{"id": "a", "name": "A", "kind": "agent", "params": {}, "spoken_names": ["x"],
          "hotkey": "ctrl+alt+a", "shaping": "raw", "description": "d", "enabled": False}],
    ],
    "param": [
        [{"id": "a", "name": "A", "kind": "agent", "params": {}}, "agent"],
        [{"id": "a", "name": "A", "kind": "agent", "params": {"agent": "codex"}}, "agent"],
        [{"id": "a", "name": "A", "kind": "agent", "params": {}}, "nope"],
        [{"id": "p", "name": "P", "kind": "program", "params": {}}, "timeout_s"],
        [{"id": "p", "name": "P", "kind": "program", "params": {}}, "args"],
        [{"id": "x", "name": "X", "kind": "shell", "params": {}}, "template"],
    ],
    "shaping_ids": [
        [{"id": "a", "name": "A", "kind": "clipboard", "shaping": "profile:email"}],
        [{"id": "a", "name": "A", "kind": "clipboard", "shaping": "transform:polish"}],
        [{"id": "a", "name": "A", "kind": "clipboard", "shaping": "cleanup"}],
        [{"id": "a", "name": "A", "kind": "clipboard", "shaping": "profile:"}],
    ],
    "presets": [[]],
    "tables": [[]],
    "secret_name": [["a1"], [""]],
    "hotkey_conflict": [
        ["ctrl+alt+p", _TRANSFORMS, ""], ["ctrl+alt+k", _SAVED, ""], ["ctrl+alt+k", _SAVED, "a1"],
        ["alt+ctrl+K", _SAVED, ""], ["ctrl+alt+e", _PROFILES, ""], ["kp *", {}, ""],
        ["", {}, ""], ["ctrl+alt+z", {}, ""], ["ctrl+alt+m", {}, ""],
        ["ctrl+alt+m", {"hotkeys": {"minimize_tray": "ctrl+alt+n"}}, ""],
    ],
})
