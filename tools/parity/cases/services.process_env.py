"""Curated inputs for services/process_env.py (M5.1).

Each case runs set_for_app / load_for_app / prepend_for_app steps against a fake os.environ that
behaves as Windows' does (names upper-cased, putenv's refusals, an assignment of "" kept in the
mapping), then reads external_env, or installed_agents.agent_child_env for the "child_env"
cases. The fake replaces os.environ only for the length of one case; the recorded originals are
forgotten before and after it.
"""

import os
from collections.abc import MutableMapping

#: Windows' _MAX_ENV: the longest "name=value" putenv accepts.
_MAX_ENV = 32767


class _WindowsEnviron(MutableMapping):
    """os.environ as Python 3.12 has it on Windows, held in memory (no putenv)."""

    def __init__(self, items):
        self._data = {}
        for name, value in items:
            self[name] = value

    @staticmethod
    def _check(name, value):
        # posixmodule.c win32_putenv: "=" is allowed only as the first character.
        if not name or "=" in name[1:]:
            raise ValueError("illegal environment variable name")
        if "\0" in name or "\0" in value:
            raise ValueError("embedded null character")
        units = len((name + "=" + value).encode("utf-16-le")) // 2
        if units > _MAX_ENV:
            raise ValueError(f"the environment variable is longer than {_MAX_ENV} characters")

    def __getitem__(self, name):
        return self._data[name.upper()]

    def __setitem__(self, name, value):
        key = name.upper()
        self._check(key, value)
        self._data[key] = value

    def __delitem__(self, name):
        del self._data[name.upper()]

    def __iter__(self):
        return iter(list(self._data))

    def __len__(self):
        return len(self._data)


def _apply(module, steps):
    """Runs the steps; returns what they refused: [name, error] for set, names for load."""
    refused = []
    for step in steps:
        op = step[0]
        if op == "set":
            try:
                module.set_for_app(step[1], step[2], keep_existing=step[3])
            except ValueError as exc:
                refused.append([step[1], str(exc)])
        elif op == "load":
            refused.extend(module.load_for_app(dict(step[1])))
        elif op == "prepend":
            module.prepend_for_app(step[1], step[2])
        else:
            raise ValueError(f"unknown step {op}")
    return refused


def _run(module, environ, steps, extra, read):
    fake = _WindowsEnviron(environ)
    real = os.environ
    module.forget()
    os.environ = fake
    try:
        refused = _apply(module, steps)
        env = read(dict(extra) if extra is not None else None)
        return {
            "env": sorted([name, value] for name, value in env.items()),
            "environ": sorted([name, value] for name, value in fake.items()),
            "refused": refused,
        }
    finally:
        os.environ = real
        module.forget()


def _external_env(module, environ, steps, extra):
    return _run(module, environ, steps, extra, module.external_env)


def _child_env(module, environ, steps, extra):
    from services import installed_agents

    return _run(module, environ, steps, extra, installed_agents.agent_child_env)


USER_PATH = "C:\\Users\\u\\bin;C:\\Tools"

CASES = {
    "external_env": [
        # Variables the app set are restored; one that was unset goes.
        [[["QT_WAYLAND_DISABLE_WINDOWDECORATION", "0"], ["PATH", USER_PATH]],
         [["set", "QT_QPA_PLATFORMTHEME", "", False],
          ["set", "QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", False]], None],
        # keep_existing leaves the user's value, and sets an unset one.
        [[["QT_WAYLAND_USE_DATA_CONTROL", "0"]],
         [["set", "QT_WAYLAND_USE_DATA_CONTROL", "1", True],
          ["set", "QT_SCALE_FACTOR_ROUNDING_POLICY", "PassThrough", True]], None],
        # The first change is the one remembered.
        [[["FV_GOLDEN", "user"]],
         [["set", "FV_GOLDEN", "one", False], ["set", "FV_GOLDEN", "two", False]], None],
        # Names in any case are one variable on Windows.
        [[["Path", "C:\\a"], ["home", "C:\\Users\\u"]],
         [["set", "path", "C:\\b", False], ["set", "HOME", "C:\\x", False]], None],
        # Path entries the app put first are removed again.
        [[["PATH", USER_PATH]],
         [["prepend", "PATH", ["C:\\cuda", "C:\\icu"]], ["prepend", "PATH", ["C:\\late"]]], None],
        # An entry already on the list moves first; empty entries are ignored.
        [[["PATH", "C:\\a;;C:\\b;C:\\a"]],
         [["prepend", "PATH", ["C:\\b", ""]], ["prepend", "PATH", ["", ""]]], None],
        # Prepending to a variable that was unset.
        [[], [["prepend", "Fv_Golden_Path", ["C:\\only"]]], None],
        # .env values: added and recorded, kept when the user's differs, recorded when equal.
        [[["OPENAI_API_KEY", "sk-user-own"], ["SAME_VALUE", "x"], ["USER_OWN_VAR", "kept"]],
         [["load", [["OPENAI_API_KEY", "sk-from-file"], ["ANTHROPIC_API_KEY", "sk-a"],
                    ["Same_Value", "x"], ["Anthropic_Api_Key", "sk-a"]]]], None],
        # What no environment can hold is refused, and the rest still loads.
        [[], [["load", [["A=B", "1"], ["", "2"], ["=HIDDEN", "3"], ["NUL\0NAME", "4"],
                        ["GOOD", "5"], ["LONG", "x" * 40000], ["NULVALUE", "a\0b"]]]], None],
        [[], [["set", "BAD=NAME", "v", False], ["set", "FV_GOLDEN_NUL", "a\0b", False],
              ["set", "", "v", False], ["set", "FV_GOLDEN_LONG", "y" * 32760, False],
              ["set", "FV_GOLDEN_OK", "y" * 32752, False]], None],
        # Non-ASCII names are upper-cased as str.upper does.
        [[["stra\u00dfe", "1"]], [["set", "\u00e9t\u00e9", "2", False]], None],
        # extra goes on top, after the originals come back.
        [[["FV_GOLDEN", "user"]], [["set", "FV_GOLDEN", "app", False]],
         [["FV_GOLDEN", "extra"], ["OPENWHISPER_EVENT_TYPE", "x"], ["Mixed_Case", "1"]]],
        # Another program's virtualenv is the user's.
        [[["VIRTUAL_ENV", "C:\\work\\their-venv"]], [], None],
    ],
    "child_env": [
        # The app's internal names never reach a child, whatever their case.
        [[["OPENWHISPER_SIDECAR_TOKEN", "internal"], ["OPENWHISPER_LLM_API_KEY", "internal"],
          ["Openwhisper_Llm_Base_Url", "http://127.0.0.1:9"], ["MY_OWN_SETTING", "kept"]],
         [["set", "QT_QPA_PLATFORMTHEME", "", False]],
         [["OPENWHISPER_EVENT_TYPE", "x"]]],
        # .env keys go too; the user's own stay.
        [[["USER_OWN_VAR", "kept"], ["ANTHROPIC_API_KEY", "sk-from-dotenv"]],
         [["load", [["Anthropic_Api_Key", "sk-from-dotenv"], ["OPENAI_API_KEY", "sk-o"]]]],
         None],
    ],
}

WRAPPERS = {
    "external_env": _external_env,
    "child_env": _child_env,
}

PINS = {
    "environ_note": "os.environ is replaced for each case by an in-memory mapping with Windows' "
                    "rules; args are [initial variables as [name, value]], [steps], extra as "
                    "[name, value] or null; the result's lists are sorted",
}
