"""Curated inputs for services/actions/agents.py (M5.7).

    app_links(agent_id, text, folder, home)      [[scheme, link], ...] with HOME and USERPROFILE
                                                 set to `home` for the call
    terminal_argv(agent_id, program, text)       the argv
    npm_shim_command(source, files, node_on_path, shim_name)
        writes npm/<shim_name> with `source` and every relative path in `files` (empty) under a
        fresh folder; with node_on_path, nodejs/node.exe exists there and shutil.which finds it,
        else which finds nothing. The result's paths are relative to that folder, with "/".
    _claude_answer, _codex_answer(stdout, stderr)   [ok, message, output, stopped] of a run
                                                 that ended by itself ("done", exit code 0)
    _stopped(agent_id, stderr)                   the message for that run
    _opencode_result(result, notifications)      [ok, message, output, stopped] after the
                                                 (method, params) notifications reached an
                                                 _AcpAnswer
    _opencode_model(session, model, configured)  with configured_default_model giving
                                                 `configured`
    _codex_tool_features(listing)                with _help giving `listing` for features list
"""

import os
import tempfile

_WORDS = "Fix the build & ship it? 100% #now / ünïcödé 日本 \"q\" 'x' --help\nline two"
_HOME = "C:\\Users\\someone"


def _result(result):
    return [result.ok, result.message, result.output, result.stopped]


def _app_links(module, agent_id, text, folder, home):
    saved = {name: os.environ.get(name) for name in ("HOME", "USERPROFILE")}
    try:
        os.environ["HOME"] = home
        os.environ["USERPROFILE"] = home
        return [list(pair) for pair in module.app_links(agent_id, text, folder)]
    finally:
        for name, value in saved.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def _npm_shim_command(module, source, files, node_on_path, shim_name):
    base = os.environ["LOCALAPPDATA"]
    with tempfile.TemporaryDirectory(dir=base) as root:
        npm = os.path.join(root, "npm")
        os.makedirs(npm)
        with open(os.path.join(npm, shim_name), "wb") as handle:
            handle.write(source.encode("utf-8"))
        for relative in files:
            path = os.path.join(root, *relative.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb"):
                pass
        node = os.path.join(root, "nodejs", "node.exe")
        if node_on_path:
            os.makedirs(os.path.dirname(node))
            with open(node, "wb"):
                pass
        real = module.shutil.which
        module.shutil.which = lambda name, *a, **k: (
            node if node_on_path and name == "node.exe" else None)
        try:
            command = module.npm_shim_command(os.path.join(npm, shim_name))
        finally:
            module.shutil.which = real
        if command is None:
            return None
        return [os.path.relpath(part, root).replace(os.sep, "/") for part in command]


def _run(module, stdout, stderr):
    return module._Run("done", 0, stdout, stderr)


def _claude_answer(module, stdout, stderr):
    return _result(module._claude_answer(_run(module, stdout, stderr)))


def _codex_answer(module, stdout, stderr):
    return _result(module._codex_answer(_run(module, stdout, stderr)))


def _stopped(module, agent_id, stderr):
    return module._stopped(agent_id, _run(module, "", stderr))


def _opencode_result(module, result, notifications):
    answer = module._AcpAnswer()
    for method, params in notifications:
        answer.on_notification(method, params)
    return _result(module._opencode_result(result, answer))


def _opencode_model(module, session, model, configured):
    agents = module.installed_agents
    real = agents.configured_default_model
    agents.configured_default_model = lambda agent_id: configured
    try:
        return module._opencode_model(session, model)
    finally:
        agents.configured_default_model = real


def _codex_tool_features(module, listing):
    real = module._help
    module._help = lambda agent, *args: listing if args == ("features", "list") else ""
    try:
        return module._codex_tool_features(None)
    finally:
        module._help = real


def _codex_events(module, stdout):
    return list(module._codex_events(stdout))


WRAPPERS = {
    "app_links": _app_links,
    "npm_shim_command": _npm_shim_command,
    "_claude_answer": _claude_answer,
    "_codex_answer": _codex_answer,
    "_stopped": _stopped,
    "_opencode_result": _opencode_result,
    "_opencode_model": _opencode_model,
    "_codex_tool_features": _codex_tool_features,
    "_codex_events": _codex_events,
}

_CODEX_SHIM = "\r\n".join([
    "@ECHO off", "GOTO start", ":find_dp0", "SET dp0=%~dp0", "EXIT /b", ":start", "SETLOCAL",
    "CALL :find_dp0", "", 'IF EXIST "%dp0%\\node.exe" (', '  SET "_prog=%dp0%\\node.exe"',
    ") ELSE (", '  SET "_prog=node"', ")", "",
    "endLocal & goto #_undefined_# 2>NUL || title %COMSPEC% & set PATHEXT=%PATHEXT:;.JS;=;% & "
    '"%_prog%"  "%dp0%\\node_modules\\@openai\\codex\\bin\\codex.js" %*', "",
])
_JS = "npm/node_modules/@openai/codex/bin/codex.js"
_PACKAGE = "npm/node_modules/@openai/codex/package.json"
_EXE = ("npm/node_modules/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/"
        "x86_64-pc-windows-msvc/bin/codex.exe")
_OPENCODE_SHIM = (
    '@ECHO off\r\nSETLOCAL\r\nset "_prog=node"\r\n'
    '"%_prog%" "%dp0%\\node_modules\\opencode-ai\\bin\\opencode" %*\r\n')

_RESULT = '{"type": "result", "subtype": "success", "is_error": false, "result": "Lima."}'

_CHUNK = "agent_message_chunk"


def _update(kind, text=None):
    update = {"sessionUpdate": kind}
    if text is not None:
        update["content"] = {"type": "text", "text": text}
    return ["session/update", {"sessionId": "s1", "update": update}]


_SESSION = {"sessionId": "s1", "configOptions": [
    {"id": "model", "type": "select", "currentValue": "prov/fast",
     "options": [{"value": "prov/fast"}, {"value": "prov/slow", "name": "Slow"}]}]}

CASES = {
    "app_links": [
        ["claude_code", _WORDS, "", _HOME],
        ["claude_code", _WORDS, "C:\\work\\my project", _HOME],
        ["claude_code", _WORDS, "C:\\Users\\José\\código", _HOME],
        ["claude_code", "~ keep -._ and / slash", "", "C:\\Users\\a b"],
        ["claude_code", "", "", _HOME],
        ["claude_code", "a" * 5000, "", _HOME],
        ["claude_code", "a" * 5001, "", _HOME],
        ["claude_code", "word " * 2800, "", _HOME],
        ["claude_code", "word " * 2801, "", _HOME],
        ["claude_code", "日" * 3331, "", _HOME],
        ["claude_code", "日" * 3332, "", _HOME],
        ["claude_code", "日本語" * 1334, "", _HOME],
        ["codex", _WORDS, "", _HOME],
        ["codex", _WORDS, "D:\\proj\\new folder", _HOME],
        ["codex", "a" * 30000, "", _HOME],
        ["opencode_cli", _WORDS, "", _HOME],
        ["pi", _WORDS, "", _HOME],
    ],
    "terminal_argv": [
        ["claude_code", "C:\\agents\\claude.exe", "purge"],
        ["claude_code", "C:\\agents\\claude.exe", "two words"],
        ["claude_code", "C:\\agents\\claude.exe", "tab\there"],
        ["claude_code", "C:\\agents\\claude.exe", "no\u00a0break"],
        ["claude_code", "C:\\agents\\claude.exe", "ideographic\u3000space"],
        ["claude_code", "C:\\agents\\claude.exe", "zero\u200bwidth"],
        ["claude_code", "C:\\agents\\claude.exe", ""],
        ["claude_code", "C:\\agents\\claude.exe", _WORDS],
        ["codex", "C:\\agents\\codex.exe", "exec"],
        ["codex", ["C:\\npm\\node.exe", "C:\\npm\\codex.js"], "--help"],
        ["opencode_cli", "C:\\agents\\opencode.exe", "hi there"],
        ["opencode_cli", "C:\\agents\\opencode.exe", "--prompt"],
        ["pi", "C:\\agents\\pi.exe", "x"],
    ],
    "npm_shim_command": [
        [_CODEX_SHIM, ["npm/node.exe", _JS, _PACKAGE], False, "codex.cmd"],
        [_CODEX_SHIM, [_JS, _PACKAGE], True, "codex.cmd"],
        [_CODEX_SHIM, ["npm/node.exe", _JS, _PACKAGE], True, "codex.cmd"],
        [_CODEX_SHIM, [_JS, _PACKAGE, _EXE], False, "codex.cmd"],
        [_CODEX_SHIM, [_JS, _PACKAGE], False, "codex.cmd"],
        [_CODEX_SHIM, ["npm/node.exe", _PACKAGE], False, "codex.cmd"],
        [_CODEX_SHIM, [_JS, _EXE], False, "codex.cmd"],
        ["@ECHO off\r\npowershell -File evil.ps1 %*\r\n", ["npm/node.exe", _JS, _PACKAGE],
         False, "codex.cmd"],
        [_CODEX_SHIM.replace("node.exe", "sh.exe").replace('"_prog=node"', '"_prog=sh"'),
         ["npm/node.exe", _JS, _PACKAGE], False, "codex.cmd"],
        [_CODEX_SHIM.replace("SET", "set"), ["npm/node.exe", _JS, _PACKAGE], False, "codex.cmd"],
        [_OPENCODE_SHIM, ["npm/node_modules/opencode-ai/bin/opencode",
                          "npm/node_modules/opencode-ai/package.json",
                          "npm/node_modules/opencode-ai/node_modules/opencode-windows-x64/bin/"
                          "opencode.exe"], False, "opencode.cmd"],
        [_OPENCODE_SHIM, ["npm/node_modules/opencode-ai/bin/opencode",
                          "npm/node_modules/opencode-ai/package.json",
                          "npm/node_modules/opencode-ai/node_modules/.hidden/bin/opencode.exe"],
         False, "opencode.cmd"],
        ['SET "_prog=node"\r\n"%_prog%" "%dp0%\\..\\lib\\cli.js" %*\r\n',
         ["lib/cli.js", "npm/node.exe"], False, "tool.cmd"],
        ['SET "_prog=node"\r\n"%_prog%" "%dp0%\\..\\lib\\cli.js" %*\r\n',
         ["lib/cli.js", "package.json"], False, "tool.cmd"],
        ["", [], False, "codex.cmd"],
    ],
    "_claude_result": [
        [_RESULT],
        ['[{"type": "system", "subtype": "init"}, ' + _RESULT + "]"],
        ['[\n  {"type": "system"},\n  ' + _RESULT + "\n]\n"],
        ['[' + _RESULT + ', {"type": "assistant"}]'],
        ['[{"type": "system", "subtype": "init"}]'],
        ["Warning: something\n" + _RESULT + "\n"],
        [_RESULT + "\ntrailing junk"],
        ['{"type": "Result", "result": "x"}'],
        ['{"type": "result"}\n{"type": "result", "result": "second"}'],
        ["5"], ["null"], [""], ["not json"],
        ['[1, "x", {"type": "result", "result": 2}]'],
        ['[[{"type": "result"}]]'],
    ],
    "_claude_answer": [
        [_RESULT, ""],
        ['{"type": "result", "subtype": "error_max_turns", "is_error": true, "result": ""}', ""],
        ['{"type": "result", "subtype": "success", "is_error": true, '
         '"result": "Not logged in · Please run /login"}', ""],
        ['{"type": "result", "subtype": null, "result": "ok"}', ""],
        ['{"type": "result", "result": "ok"}', ""],
        ['{"type": "result", "subtype": "success"}', ""],
        ['{"type": "result", "subtype": "success", "result": 5}', ""],
        ['{"type": "result", "subtype": "success", "result": null}', ""],
        ['{"type": "result", "subtype": "success", "result": "   "}', ""],
        ['{"type": "result", "subtype": "success", "is_error": "yes", "result": "  many   '
         'spaces\\nand lines  "}', ""],
        ['{"type": "result", "subtype": 7, "result": ""}', ""],
        ['{"type": "result", "subtype": "error", "result": "' + "é" * 250 + '"}', ""],
        ['{"type": "result", "subtype": "error", "result": "HTTP 401 from the API"}', ""],
        ["", "boom"],
        ["", ""],
        ["", "first\n\n   \nsecond line\n  "],
        ["", "Please log in again"],
        ["", "Invalid API key\nretrying"],
        ["", "x" * 300],
    ],
    "_codex_events": [
        ['{"type": "a"}\n[1]\nnot json\n\n{"type": "b"}\r\n5'],
        [""],
    ],
    "_codex_answer": [
        ['{"type": "thread.started"}\n{"type": "error", "message": "Reconnecting... 1/5"}\n'
         '{"type": "item.completed", "item": {"type": "agent_message", "text": "Answer"}}\n'
         '{"type": "turn.completed"}\n', ""],
        ['{"type": "turn.failed", "error": {"message": '
         '"{\\"error\\": {\\"message\\": \\"The \'x\' model is not supported.\\"}}"}}\n', ""],
        ['{"type": "turn.failed", "error": {}}\n', ""],
        ['{"type": "turn.failed"}\n', ""],
        ['{"type": "error"}\n', ""],
        ['{"type": "error", "message": 5}\n', ""],
        ['{"type": "error", "message": "Not logged in"}\n', ""],
        ['{"type": "error", "message": "boom"}\n{"type": "turn.completed"}\n', ""],
        ['{"type": "turn.completed"}\n', ""],
        ['{"type": "item.completed", "item": {"type": "agent_message", "text": "  "}}\n'
         '{"type": "turn.completed"}\n', ""],
        ['{"type": "item.completed", "item": {"type": "agent_message", "text": "first"}}\n'
         '{"type": "item.completed", "item": {"type": "reasoning", "text": "no"}}\n'
         '{"type": "item.completed", "item": "not a dict"}\n'
         '{"type": "item.completed", "item": {"type": "agent_message", "text": 42}}\n'
         '{"type": "turn.completed"}\n', ""],
        ['{"type": "item.completed", "item": {"type": "agent_message", "text": "half"}}\n',
         "Error: stream closed"],
        ["", ""],
        ["junk\n[]\n", "warning\nunauthorized"],
        ['{"type": "turn.failed", "error": "a string"}\n', ""],
    ],
    "_codex_message": [
        ["plain words"],
        ['{"error": {"message": "inner"}}'],
        ['{"error": {"message": ""}}'],
        ['{"error": {"message": 5}}'],
        ['{"error": {}}'],
        ['{"error": null}'],
        ['{"other": 1}'],
        ["[1, 2]"],
        ["5"],
        [""],
        ['{"error": "a string"}'],
    ],
    "_failure": [
        ["codex", "Not logged in"],
        ["claude_code", "  many   spaces\nand lines "],
        ["claude_code", "x" * 300],
        ["claude_code", "é" * 250],
        ["opencode_cli", ""],
        ["opencode_cli", "   "],
        ["codex", "401 Unauthorized"],
        ["codex", "Please LOG IN"],
        ["opencode_cli", "Provider auth required"],
        ["claude_code", "Authentication failed"],
        ["codex", "The model is overloaded"],
    ],
    "_signed_out": [["claude_code"], ["codex"], ["opencode_cli"]],
    "_stopped": [
        ["claude_code", ""],
        ["claude_code", "boom"],
        ["codex", "first\nsecond"],
        ["codex", "error: you are not logged in\n"],
        ["claude_code", "login required\nthen something else"],
        ["claude_code", "  \n\t\n"],
        ["opencode_cli", "y" * 300],
    ],
    "_opencode_result": [
        [{"stopReason": "end_turn"}, [_update(_CHUNK, "Answer: "), _update(_CHUNK, "four")]],
        [{"stopReason": "cancelled"}, [_update(_CHUNK, "partial")]],
        [{"stopReason": "refusal"}, []],
        [None, [_update(_CHUNK, "still answered")]],
        ["text", []],
        [{"stopReason": "end_turn"}, []],
        [{"stopReason": "end_turn"}, [_update(_CHUNK, "Let me look."),
                                      _update("tool_call"), _update(_CHUNK, "Final.")]],
        [{"stopReason": "end_turn"}, [_update(_CHUNK, "a"), _update("tool_call_update"),
                                      _update("plan"), _update(_CHUNK, "b")]],
        [{"stopReason": "max_tokens"}, [_update(_CHUNK, "cut")]],
        [{"stopReason": "end_turn"}, [["other/method", {"update": {"sessionUpdate": _CHUNK,
                                                                     "content": {"text": "x"}}}],
                                      ["session/update", {"update": {"sessionUpdate": _CHUNK,
                                                                     "content": {"text": 5}}}],
                                      ["session/update", {}],
                                      ["session/update", {"update": None}],
                                      _update(_CHUNK, "  kept  ")]],
    ],
    "_refuse_acp_request": [
        ["session/request_permission", {"options": [
            {"optionId": "yes", "kind": "allow_once"}, {"optionId": "no", "kind": "reject_once"}]}],
        ["session/request_permission", {"options": [
            {"optionId": "a", "kind": "allow_always"}, {"optionId": "r", "kind": "reject_always"},
            {"optionId": "n", "kind": "reject_once"}]}],
        ["session/request_permission", {"options": [{"optionId": "yes", "kind": "allow_once"}]}],
        ["session/request_permission", {"options": None}],
        ["session/request_permission", {}],
        ["session/request_permission", {"options": ["reject", 5, {"optionId": "k"}]}],
        ["session/request_permission", {"options": [{"kind": "reject_once"}]}],
        ["session/request_permission", {"options": [{"optionId": "x", "kind": None}]}],
        ["session/request_permission", {"options": {"reject": {"kind": "reject_once"}}}],
        ["fs/read_text_file", {"path": "C:\\secret.txt"}],
        ["terminal/create", {}],
    ],
    "_opencode_model": [
        [_SESSION, "prov/slow", ""],
        [_SESSION, "prov/missing", ""],
        [_SESSION, "", "prov/slow"],
        [_SESSION, "", "gone/model"],
        [_SESSION, "", ""],
        [_SESSION, "prov/fast", "gone/model"],
        [{"sessionId": "s1"}, "", "prov/slow"],
        [{"models": {"availableModels": [{"modelId": "old/m"}]}}, "old/m", ""],
    ],
    "_codex_tool_features": [
        ["shell_tool    stable   true\napps          stable   false\n"
         "code_mode_host stable  true\njs_repl       removed  false\nfast_mode     stable   true\n"],
        [""],
        ["Error: config.toml line 3: expected a value"],
        ["fast_mode stable true\n"],
        ["\n".join(name + " stable true" for name in (
            "js_repl", "hooks", "shell_tool", "apps", "plugins", "memories", "goals"))],
        ["  shell_tool\tstable\n\n   \nunified_exec"],
    ],
}

PINS = {
    "result_note": "_claude_answer, _codex_answer and _opencode_result give [ok, message, "
                   "output, stopped]; app_links gives [scheme, link] pairs",
    "npm_note": "npm_shim_command's paths are relative to the case's folder, with /",
}
