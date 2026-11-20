"""Curated inputs for services/installed_agents.py (M5.6).

The sign-in parsers run with ``_run`` replaced for the length of one call by a function that
returns the canned (exit code, output). The configuration readers run against files a case
writes into a fresh folder under the isolated LOCALAPPDATA: ``files`` maps "claude/...",
"codex/..." and "xdg/..." to their text, and CLAUDE_CONFIG_DIR, CODEX_HOME and XDG_CONFIG_HOME
point at those three subfolders for the length of the call.
"""

import os
import tempfile

_CONFIG_VARIABLES = {
    "CLAUDE_CONFIG_DIR": "claude",
    "CODEX_HOME": "codex",
    "XDG_CONFIG_HOME": "xdg",
}


def _with_config(files, call):
    base = os.environ["LOCALAPPDATA"]
    with tempfile.TemporaryDirectory(dir=base) as root:
        for name in _CONFIG_VARIABLES.values():
            os.makedirs(os.path.join(root, name), exist_ok=True)
        for relative, text in files.items():
            path = os.path.join(root, *relative.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(text.encode("utf-8"))
        saved = {name: os.environ.get(name) for name in _CONFIG_VARIABLES}
        try:
            for name, folder in _CONFIG_VARIABLES.items():
                os.environ[name] = os.path.join(root, folder)
            return call()
        finally:
            for name, value in saved.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value


def _sign_in(function, code, output):
    def run(module):
        real = module._run
        module._run = lambda cmd, timeout_s: (code, output)
        try:
            return list(getattr(module, function)("agent"))
        finally:
            module._run = real
    return run


def _claude_sign_in(module, code, output):
    return _sign_in("_claude_sign_in", code, output)(module)


def _codex_sign_in(module, code, output):
    return _sign_in("_codex_sign_in", code, output)(module)


def _parse_version(module, text):
    key, printed = module.parse_version(text)
    return [list(key), printed]


def _configured_default_model(module, files, agent_id):
    return _with_config(files, lambda: module.configured_default_model(agent_id))


def _default_model_label(module, files, agent_id):
    return _with_config(files, lambda: module.default_model_label(agent_id))


def _codex_models(module, files):
    return _with_config(files, lambda: [[m.value, m.label] for m in module._codex_models()])


def _codex_model_efforts(module, files):
    return _with_config(files, lambda: {k: list(v) for k, v in module.codex_model_efforts().items()})


def _model_display_name(module, files, agent_id, model, models):
    entries = None if models is None else [module.AgentModel(v, l) for v, l in models]
    return _with_config(files, lambda: module.model_display_name(agent_id, model, entries))


def _describe_choice(module, files, agent_id, model):
    return _with_config(files, lambda: module.describe_choice(agent_id, model))


def _agent_specs(module):
    return [
        {"id": spec.id, "name": spec.name, "vendor": spec.vendor,
         "commands": list(spec.commands), "transport": spec.transport,
         "install_url": spec.install_url, "install_hint": spec.install_hint,
         "monogram": spec.monogram, "accent": spec.accent,
         "min_version": list(spec.min_version), "update_hint": spec.update_hint}
        for spec in (module.AGENT_SPECS[agent_id] for agent_id in module.AGENT_ORDER)
    ]


def _usable(module, problem, signed_in):
    return module.InstalledAgent("codex", "codex", "0.158.0", signed_in=signed_in,
                                 problem=problem).usable


_CACHE = (
    '{"models": ['
    '{"slug": "gpt-6-sol", "display_name": "GPT-6-Sol", "visibility": "list",'
    ' "supported_reasoning_levels": [{"effort": "low"}, {"effort": "high"}]},'
    '{"slug": "internal", "display_name": "Hidden", "visibility": "hide",'
    ' "supported_reasoning_levels": [{"effort": "medium"}]},'
    '{"slug": "gpt-6-mini", "visibility": null, "supported_reasoning_levels": []},'
    '{"slug": "", "display_name": "No slug"},'
    '{"slug": "no-name", "display_name": ""},'
    '{"slug": 7, "display_name": "Number"},'
    '"not an entry",'
    '{"slug": "odd-levels", "supported_reasoning_levels":'
    ' [{"effort": ""}, {"effort": null}, "high", {"effort": 3}, {"other": "x"}, {"effort": "xhigh"}]},'
    '{"slug": "gpt-6-sol", "display_name": "Again", "visibility": "list",'
    ' "supported_reasoning_levels": [{"effort": "minimal"}]}'
    ']}'
)

_TOML = [
    'model = "gpt-6-sol"\n',
    "model = 'literal\\path'\n",
    '# comment\nmodel = "  padded  " # trailing\n',
    'model = "esc\\u00e9\\t\\"q\\""\n',
    'model = """\nmulti\nline"""\n',
    'model = """one \\\n    two"""\n',
    "model = '''\nraw\\n'''\n",
    'model_reasoning_effort = "high"\nmodel = "o5"\n',
    '"model" = "quoted-key"\n',
    "'model' = 'literal-key'\n",
    '[profiles.fast]\nmodel = "inside a table"\n',
    'approval = "never"\n[tools]\nweb = true\n',
    'model = "first"\n[model_providers.x]\nname = "x"\n',
    'model = 5\n',
    'model = ["a"]\n',
    'model = { name = "x" }\n',
    'model.name = "dotted"\n',
    'model = "a"\nmodel = "b"\n',
    'model = "unterminated\n',
    'model = "bad \\q escape"\n',
    'just junk\n',
    'model = "ok"\nbroken = \n',
    'list = [\n  1,\n  2, # c\n]\nmodel = "after array"\n',
    'when = 1979-05-27T07:32:00Z\nday = 1979-05-27 07:32:00\nmodel = "dates"\n',
    'n = +1_000\nf = 6.626e-34\nh = 0xDEAD_beef\ni = inf\nmodel = "numbers"\n',
    'model = "crlf"\r\nother = 1\r\n',
    '﻿model = "bom"\n',
    '[model]\nname = "table"\n',
    'model = "x"\n[model]\n',
    '',
    'model = ""\n',
    'model = "tab\there"\n',
    'a = { b = 1, c = [1, 2] }\nmodel = "inline"\n',
    'model = "two"  extra\n',
    'model = """quotes"""""\n',
    'model = "abc\\',
    'model = "\\uD800"\n',
    'model = "\\U0001F600 \\u00E9"\n',
    "model = 'it''s'\n",
    'model = """a\n',
    '  model   =   "indented"   \n',
    'model="tight"',
    'a.b = 1\na.c = 2\nmodel = "dotted siblings"\n',
    'a = 1\na.b = 2\nmodel = "value then table"\n',
    '[a]\n[a]\nmodel = "x"\n',
    '[[items]]\nx = 1\n[[items]]\nx = 2\n',
    'model = "with arrays"\n[[mcp]]\nname = "a"\n[[mcp]]\nname = "b"\n[mcp.env]\nk = 1\n',
    'model = "array then table"\n[[mcp]]\nname = "a"\n[mcp]\n',
    'model = "x" # comment with \x01 control\n',
    'model = true\n',
    'model = 1979-05-27\n',
]

CASES = {
    "parse_version": [
        ["2.1.281 (Claude Code)"], ["codex-cli 0.158.0-alpha.2.1"], ["opencode 2.0"],
        ["no version here"], [""], ["v1.2"], ["1.2.3-beta+build"], ["abc 10.20.30-rc.1 more"],
        ["1.2.3.4"], ["١٢.٣"], ["2.0-"], ["1.2.-3"], ["0.0.0-0"], ["12.x 3.4"],
        ["1.2.3-"], ["9999999999.1.2"], ["0.146.0"], ["0.158.0-alpha.2"], ["0.158.0"],
        ["codex-cli 0.120.0\n"], ["1.2.3-a_b"], ["1.2.3--x"],
    ],
    "_strip_jsonc": [
        ['{"url": "https://a//b", /* c */ "x": [1, 2,],}'],
        ['{\n  // their favourite\n  "model": "x",\n  "provider": {"x": 1}, /* trailing */\n}\n'],
        ['{"a": ",}", "b": "x, ]"}'],
        ['{"a": "say \\"hi\\" // not a comment", "b": 1 // comment\n}'],
        ['{"a": 1 /* unterminated'],
        ['{"a": 1} // no newline at the end'],
        ['{"a": [1, 2 ,\n\t]}'],
        ['{"a": "\\\\"}, // after an escaped backslash'],
        ['/**/{"a"/* x */:/**/1}'],
        ['{"e": "é // ü", "f": 2, }'],
        ['{"s": "a\\'],
        ['"/* inside */"'],
        [''],
    ],
    "_claude_sign_in": [
        [0, '{"loggedIn": true, "authMethod": "claude.ai", "subscriptionType": "team"}'],
        [0, '{"loggedIn": true, "apiProvider": "bedrock"}'],
        [0, '{"loggedIn": true, "apiProvider": "vertex", "subscriptionType": "max"}'],
        [0, '{"loggedIn": true, "apiProvider": "foundry"}'],
        [0, '{"loggedIn": true, "authMethod": "api_key"}'],
        [0, '{"loggedIn": false}'],
        [0, '{"loggedIn": false, "subscriptionType": "pro"}'],
        [0, '{"loggedIn": true}'],
        [0, '{"loggedIn": 1, "subscriptionType": "PRO"}'],
        [0, '{"loggedIn": 0}'],
        [0, '{}'],
        [0, '{"subscriptionType": "Enterprise"}'],
        [0, '{"subscriptionType": "free", "loggedIn": true}'],
        [0, '{"subscriptionType": "unknown-plan", "loggedIn": true}'],
        [0, '{"authMethod": ["key"], "loggedIn": null}'],
        [0, '{"authMethod": {"Key": 1}}'],
        [0, '{"authMethod": "OAuth Token"}'],
        [0, '{"apiProvider": "firstParty", "authMethod": "claude.ai", "loggedIn": true}'],
        [1, 'Warning: something\n{"loggedIn": true, "subscriptionType": "pro"}\nbye'],
        [0, 'no json at all'],
        [None, ''],
        [0, '{"loggedIn": true'],
        [0, '} {"loggedIn": true'],
        [0, '[{"loggedIn": true}]'],
        [0, '{"loggedIn": true} trailing } brace'],
        [0, '{"loggedIn": NaN}'],
        [0, '{"apiProvider": 0, "subscriptionType": false, "authMethod": "", "loggedIn": "yes"}'],
    ],
    "_codex_sign_in": [
        [0, "Logged in using ChatGPT"],
        [0, "Logged in using an API key"],
        [1, "Not logged in"],
        [0, "Not logged in"],
        [2, "Logged in using ChatGPT"],
        [None, "Logged in using ChatGPT"],
        [None, ""],
        [0, "Logged in"],
        [0, "LOGGED IN USING CHATGPT"],
        [0, "Signed in with an API KEY"],
        [0, ""],
    ],
    "sign_in_hint": [["claude_code"], ["codex"], ["opencode_cli"], ["pi"], [""]],
    "configured_default_model": (
        [[{"claude/settings.json": '{"model": "opus"}'}, "claude_code"],
         [{"claude/settings.json": '{"model": "  sonnet \\n"}'}, "claude_code"],
         [{"claude/settings.json": '{"model": 4}'}, "claude_code"],
         [{"claude/settings.json": '["opus"]'}, "claude_code"],
         [{"claude/settings.json": '{"model": "opus",}'}, "claude_code"],
         [{"claude/settings.json": '﻿{"model": "opus"}'}, "claude_code"],
         [{"claude/settings.json": '{"model": "opus"} // comment'}, "claude_code"],
         [{}, "claude_code"],
         [{"xdg/opencode/opencode.jsonc":
               '{\n  // their favourite\n  "model": "openrouter/deepseek/deepseek-v4.1-flash",\n'
               '  "provider": {"x": {"baseURL": "https://example.com/v1"}}, /* trailing */\n}\n'},
          "opencode_cli"],
         [{"xdg/opencode/opencode.jsonc": '{"model": 1}',
           "xdg/opencode/opencode.json": '{"model": "second"}',
           "xdg/opencode/config.json": '{"model": "third"}'}, "opencode_cli"],
         [{"xdg/opencode/opencode.jsonc": 'not json',
           "xdg/opencode/config.json": '{"model": "third",}'}, "opencode_cli"],
         [{"xdg/opencode/opencode.json": '{"model": "a//b"}'}, "opencode_cli"],
         [{"xdg/opencode/opencode.json": '{"model": "x"}\r// windows ending'}, "opencode_cli"],
         [{"codex/config.toml": 'model = "o5"\n'}, "pi"]]
        + [[{"codex/config.toml": text}, "codex"] for text in _TOML]
    ),
    "default_model_label": [
        [{"codex/config.toml": 'model = "gpt-6-sol"\n'}, "codex"],
        [{}, "codex"],
        [{"claude/settings.json": '{"model": "opus"}'}, "claude_code"],
        [{}, "opencode_cli"],
    ],
    "_codex_models": [
        [{"codex/models_cache.json": _CACHE}],
        [{"codex/models_cache.json": '{"models": null}'}],
        [{"codex/models_cache.json": '{"models": {"slug": "x"}}'}],
        [{"codex/models_cache.json": '[]'}],
        [{}],
    ],
    "codex_model_efforts": [
        [{"codex/models_cache.json": _CACHE}],
        [{"codex/models_cache.json": '{"models": []}'}],
        [{}],
    ],
    "model_display_name": [
        [{}, "claude_code", "haiku", None],
        [{}, "claude_code", "opus", None],
        [{}, "claude_code", "claude-opus-9", None],
        [{}, "codex", "haiku", None],
        [{}, "opencode_cli", "openrouter/deepseek/v4", None],
        [{}, "opencode_cli", "openrouter/deepseek/v4", [["openrouter/deepseek/v4", "DeepSeek V4 (OpenRouter)"]]],
        [{}, "codex", "gpt-6-sol", [["gpt-6-sol", "GPT-6-Sol"]]],
        [{}, "codex", "x", [["x", "Name (a) (b)"]]],
        [{}, "codex", "x", [["x", "(only)"]]],
        [{}, "codex", "x", [["x", "Trailing (paren) text"]]],
        [{}, "codex", "x", [["x", "Line (one)\n"]]],
        [{}, "claude_code", "", None],
        [{"claude/settings.json": '{"model": "opus"}'}, "claude_code", "", None],
        [{"xdg/opencode/opencode.json": '{"model": "openrouter/deepseek/v4"}'}, "opencode_cli", "", None],
        [{"codex/config.toml": 'model = "gpt-6-sol"\n'}, "codex", "", None],
        [{}, "codex", "a/b/", None],
        [{}, "unknown", "m/n", None],
    ],
    "describe_choice": [
        [{}, "claude_code", "haiku"],
        [{}, "opencode_cli", "openrouter/deepseek/v4"],
        [{}, "claude_code", ""],
        [{"codex/config.toml": 'model = "gpt-6-sol"\n'}, "codex", ""],
        [{}, "pi", "x"],
        [{}, "codex", "o5"],
    ],
    "AGENT_SPECS": [[]],
    "usable": [["", None], ["", True], ["", False], ["too old", None], ["too old", True]],
}

WRAPPERS = {
    "parse_version": _parse_version,
    "_claude_sign_in": _claude_sign_in,
    "_codex_sign_in": _codex_sign_in,
    "configured_default_model": _configured_default_model,
    "default_model_label": _default_model_label,
    "_codex_models": _codex_models,
    "codex_model_efforts": _codex_model_efforts,
    "model_display_name": _model_display_name,
    "describe_choice": _describe_choice,
    "AGENT_SPECS": _agent_specs,
    "usable": _usable,
}

PINS = {
    "sign_in_note": "args of _claude_sign_in and _codex_sign_in are the canned (exit code, "
                    "output) _run returns; the result is [signed_in, account]",
    "config_note": "args start with the files written under a fresh folder: claude/ is "
                   "CLAUDE_CONFIG_DIR, codex/ is CODEX_HOME, xdg/ is XDG_CONFIG_HOME",
    "parse_version_note": "the result is [sort key as a list, the version as printed]",
}
