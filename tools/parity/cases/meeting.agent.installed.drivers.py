"""Curated inputs for meeting/agent/installed/drivers.py (M6.2).

    tool_contract(tools, agent_name, prefix, closing)   the meeting tools are
                                                 mcp_tool_definitions() at the pinned commit
    _claude_error, _codex_error(message)         the message; a raise is recorded as such
    claude_command(help, request)                ClaudeCodeDriver with _help_text giving `help`:
                                                 check_ready, then build(request,
                                                 "http://127.0.0.1:1/mcp/t", "t", <workdir>) as
                                                 {"argv", "env", "files"}: the files are the
                                                 text of system.md, mcp.json, settings.json; the
                                                 workdir reads "<workdir>". A refusal is
                                                 {"unavailable": message}.
    codex_command(exec_help, features, mcp_list, request)   CodexDriver the same way, with
                                                 agent_workspace_dir() reading "<workspace>"
                                                 and the instructions file's text in "files"
    claude_events(events, tools)                 every event through handle_event, as
                                                 _read_stdout feeds them (a failing event is
                                                 skipped): {"state", "emitted"}
    codex_events(events)                         the same for CodexDriver
    claude_finish, codex_finish(state, returncode, stderr_tail)   finish(...) as
                                                 [ok, text, error, usage, canceled]
    codex_features(listing)                      _features_to_disable with features list
                                                 giving `listing`
    codex_servers(listing)                       _mcp_servers_to_disable with mcp list --json
                                                 giving `listing`, or {"unavailable": message}
    opencode_overlay(tool_free, personas, url, token)   OpenCodeDriver._overlay()
    opencode_request(method, params)             OpenCodeDriver._on_request, or
                                                 {"acp_error": message, "code": code}
    opencode_pass_model(session, model, user_default)   _pass_model as [model, error]
    opencode_failed(message, canceled)           _failed(AcpError(message), event) as
                                                 [ok, text, error, usage, canceled]
    opencode_outcome(result)                     _outcome(result), the same way
    tail_detail(stderr_tail)                     _tail_detail
Requests are PassRequest keyword arguments; the handler answers nothing.
"""

import json
import os
import shutil
import tempfile

from meeting.agent.tool_specs import mcp_tool_definitions

_MEETING_TOOLS = mcp_tool_definitions()
_URL = "http://127.0.0.1:1/mcp/t"
_WORKSPACE = "<workspace>"


def _outcome(outcome):
    return [outcome.ok, outcome.text, outcome.error, outcome.usage, outcome.canceled]


def _request(drivers, values):
    return drivers.PassRequest(handler=lambda *_a: ("", False), **values)


def _normalize(value, workdir):
    forward = workdir.replace(os.sep, "/")
    if isinstance(value, str):
        return value.replace(workdir, "<workdir>").replace(forward, "<workdir>")
    if isinstance(value, list):
        return [_normalize(item, workdir) for item in value]
    if isinstance(value, dict):
        return {key: _normalize(item, workdir) for key, item in value.items()}
    return value


def _read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def _build(driver, request, workdir, names):
    argv, env = driver.build(request, _URL, "t", workdir)
    files = {name: _read(os.path.join(workdir, name)) for name in names
             if os.path.exists(os.path.join(workdir, name))}
    return _normalize({"argv": argv, "env": env, "files": files}, workdir)


def _with_help(drivers, answer, body):
    real_help, real_workspace = drivers._help_text, drivers.agent_workspace_dir
    drivers._help_text = lambda agent, *args: answer(args)
    drivers.agent_workspace_dir = lambda: _WORKSPACE
    workdir = tempfile.mkdtemp(dir=os.environ["LOCALAPPDATA"])
    try:
        return body(workdir)
    except drivers.AgentUnavailable as exc:
        return {"unavailable": str(exc)}
    finally:
        drivers._help_text, drivers.agent_workspace_dir = real_help, real_workspace
        shutil.rmtree(workdir, ignore_errors=True)


def _claude_command(drivers, help_text, values):
    def body(workdir):
        driver = drivers.ClaudeCodeDriver(
            drivers.InstalledAgent("claude_code", "claude.exe", "2.1.306"))
        driver.check_ready()
        return _build(driver, _request(drivers, values), workdir,
                      ("system.md", "mcp.json", "settings.json"))
    return _with_help(drivers, lambda args: help_text, body)


def _codex_command(drivers, exec_help, features, mcp_list, values):
    def answer(args):
        if args[-3:] == ("mcp", "list", "--json"):
            return mcp_list
        if args[:1] == ("features",):
            return features
        return exec_help

    def body(workdir):
        driver = drivers.CodexDriver(drivers.InstalledAgent("codex", "codex.exe", "0.163.0"))
        driver.check_ready()
        return _build(driver, _request(drivers, values), workdir, ("instructions.md",))
    return _with_help(drivers, answer, body)


def _events(drivers, driver, events, values):
    emitted = []
    request = _request(drivers, dict(values, on_event=lambda kind, tool: emitted.append(
        [kind, tool])))
    state = drivers._RunState()
    for event in events:
        try:
            driver.handle_event(event, state, request)
        except Exception as exc:  # _read_stdout logs and goes on
            emitted.append(["raised", type(exc).__name__])
    return {"state": {"text": state.text, "error": state.error, "fatal": state.fatal,
                      "done": state.done, "usage": state.usage},
            "emitted": emitted}


def _claude_events(drivers, events, tools):
    driver = drivers.ClaudeCodeDriver(drivers.InstalledAgent("claude_code", "claude", "2.1.306"))
    return _events(drivers, driver, events, {"system_prompt": "s", "user_prompt": "u",
                                             "tools": tools})


def _codex_events(drivers, events):
    driver = drivers.CodexDriver(drivers.InstalledAgent("codex", "codex", "0.163.0"))
    return _events(drivers, driver, events, {"system_prompt": "s", "user_prompt": "u",
                                             "tools": []})


def _state(drivers, values):
    state = drivers._RunState()
    for key, value in values.items():
        setattr(state, key, value)
    return state


def _claude_finish(drivers, state, returncode, tail):
    driver = drivers.ClaudeCodeDriver(drivers.InstalledAgent("claude_code", "claude", "2.1.306"))
    return _outcome(driver.finish(_state(drivers, state), returncode, tail))


def _codex_finish(drivers, state, returncode, tail):
    driver = drivers.CodexDriver(drivers.InstalledAgent("codex", "codex", "0.163.0"))
    return _outcome(driver.finish(_state(drivers, state), returncode, tail))


def _codex_features(drivers, listing):
    return _with_help(drivers, lambda args: listing, lambda _workdir: drivers.CodexDriver(
        drivers.InstalledAgent("codex", "codex", "0.163.0"))._features_to_disable())


def _codex_servers(drivers, listing):
    def body(_workdir):
        driver = drivers.CodexDriver(drivers.InstalledAgent("codex", "codex", "0.163.0"))
        driver._disable = ["shell_tool"]
        return driver._mcp_servers_to_disable()
    return _with_help(drivers, lambda args: listing, body)


class _Server:
    def __init__(self, url):
        self._url = url

    def url(self, _endpoint):
        return self._url


class _Router:
    def __init__(self, token):
        self.token = token


def _opencode_overlay(drivers, tool_free, personas, url, token):
    driver = drivers.OpenCodeDriver(drivers.InstalledAgent("opencode_cli", "opencode", "2.0.18"))
    driver._server, driver._router = _Server(url), _Router(token)
    driver._personas, driver._tool_free = dict(personas), tool_free
    return driver._overlay()


def _opencode_request(drivers, method, params):
    driver = drivers.OpenCodeDriver(drivers.InstalledAgent("opencode_cli", "opencode", "2.0.18"))
    try:
        return driver._on_request(method, params)
    except drivers.AcpError as exc:
        return {"acp_error": str(exc), "code": exc.code}


def _opencode_pass_model(drivers, session, model, user_default):
    driver = drivers.OpenCodeDriver(drivers.InstalledAgent("opencode_cli", "opencode", "2.0.18"))
    driver._user_default = user_default
    return list(driver._pass_model(session, _request(drivers, {
        "system_prompt": "s", "user_prompt": "u", "tools": [], "model": model})))


def _opencode_failed(drivers, message, canceled):
    import threading

    event = threading.Event()
    if canceled:
        event.set()
    return _outcome(drivers.OpenCodeDriver._failed(drivers.AcpError(message), event))


def _opencode_outcome(drivers, result):
    return _outcome(drivers.OpenCodeDriver._outcome(result))


def _tail_detail(drivers, tail):
    return drivers._tail_detail(tail)


WRAPPERS = {
    "claude_command": _claude_command,
    "codex_command": _codex_command,
    "claude_events": _claude_events,
    "codex_events": _codex_events,
    "claude_finish": _claude_finish,
    "codex_finish": _codex_finish,
    "codex_features": _codex_features,
    "codex_servers": _codex_servers,
    "opencode_overlay": _opencode_overlay,
    "opencode_request": _opencode_request,
    "opencode_pass_model": _opencode_pass_model,
    "opencode_failed": _opencode_failed,
    "opencode_outcome": _opencode_outcome,
    "tail_detail": _tail_detail,
}

_CLAUDE_HELP = ("--mcp-config --strict-mcp-config --tools --system-prompt --system-prompt-file "
                "--permission-prompts --no-session-persistence --disable-slash-commands "
                "--effort --include-partial-messages")
_ONE_TOOL = [{"name": "search", "inputSchema": {"type": "object", "properties": {
    "query": {"type": "string"}, "limit": {"type": "integer"}}, "required": ["query"]}}]
_ODD_TOOLS = [
    {"name": "plain"},
    {"name": "typed", "inputSchema": {"properties": {
        "a": {"type": ["string", "null"]}, "b": "not a dict", "c": {}, "d": {"type": None}},
        "required": ["a", "zzz"]}},
    {"name": "patch_state", "inputSchema": {"properties": {}}},
    {"inputSchema": None},
    {"name": "spaced name", "inputSchema": {"properties": {"x y": {"type": "number"}},
                                            "required": "x y"}},
]
_MEETING = {"system_prompt": "COPILOT CHARTER\nline two \u00e9", "user_prompt": "transcript",
            "tools": _MEETING_TOOLS, "model": "haiku", "effort": "low"}
_READING = {"system_prompt": "Write it up.", "user_prompt": "the pdf", "tools": [],
            "model": "", "effort": "medium", "read_folder": "C:\\captures\\2026-11-16 run"}
_VIEWING = {"system_prompt": "Write it up.", "user_prompt": "the words", "tools": [],
            "model": "gpt-6-sol", "effort": "medium",
            "images": ["C:\\run\\shot-1.png", "C:\\run\\shot 2.png"]}
_CODEX_FEATURES = ("shell_tool   stable   true\napps   stable   true\nsqlite   removed   true\n"
                   "memories  stable  false\nview_image stable true\nunknown_new stable true\n")
_SERVERS = json.dumps([{"name": "other", "enabled": True}, {"name": "openwhisper", "enabled": True},
                       {"name": "already_off", "enabled": False}, {"name": "b-2"},
                       {"name": "other", "enabled": True}])

_INIT = {"type": "system", "subtype": "init", "tools": [],
         "mcp_servers": [{"name": "openwhisper", "status": "connected"}]}
_RESULT = {"type": "result", "subtype": "success", "is_error": False, "result": "Done.",
           "num_turns": 2, "total_cost_usd": 0.001,
           "usage": {"input_tokens": 100, "cache_read_input_tokens": 50,
                     "cache_creation_input_tokens": 7, "output_tokens": 20}}

CASES = {
    "tool_contract": [
        [_MEETING_TOOLS, "OpenCode", "openwhisper_",
         "When the work is done, reply with one short sentence."],
        [_MEETING_TOOLS, "Codex", "", "Reply with the report."],
        [_MEETING_TOOLS, "Claude Code", "mcp__openwhisper__",
         "When the work is done, reply with one short sentence."],
        [[], "Claude Code", "mcp__openwhisper__", "Reply with the write-up."],
        [[], "OpenCode", "", ""],
        [_ONE_TOOL, "Codex", "", "Reply with the finished report."],
        [_ODD_TOOLS, "Claude Code", "mcp__openwhisper__", "Done."],
        [["not a dict"], "Codex", "", "x"],
    ],
    "_claude_error": [
        ["Not logged in \u00b7 Please run /login"], ["  Authentication failed  "],
        ["Please LOGIN again"], ["error_max_turns"], [""], ["   "], ["x" * 400],
        ["\u00e9" * 350], ["API Error: 529 overloaded"],
    ],
    "_codex_error": [
        [json.dumps({"type": "error", "status": 400, "error": {
            "message": "The 'x' model is not supported when using Codex with a ChatGPT account."}})],
        ['{"error": {"message": ""}}'], ['{"error": {"message": 5}}'], ['{"error": null}'],
        ['{"error": "a string"}'], ["[1, 2]"], ["plain words"], ["  Not logged in  "],
        ["unexpected status 401 Unauthorized"], ['{"error": {"message": "HTTP 401"}}'],
        ["y" * 400], [""],
    ],
    "claude_command": [
        [_CLAUDE_HELP, _MEETING],
        [_CLAUDE_HELP, dict(_MEETING, effort="medium", model="")],
        [_CLAUDE_HELP + " --restricted --add-dir", _READING],
        [_CLAUDE_HELP + " --restricted --add-dir", _MEETING],
        ["--mcp-config --strict-mcp-config --tools --system-prompt", _MEETING],
        ["--mcp-config --strict-mcp-config --tools --system-prompt --effort", _READING],
        ["--mcp-config only", _MEETING],
        ["", _MEETING],
    ],
    "codex_command": [
        ["--json --ephemeral --disable", _CODEX_FEATURES, _SERVERS, dict(_MEETING, model="")],
        ["--json", _CODEX_FEATURES, "[]", _VIEWING],
        ["--json --ephemeral", "", "[]", dict(_VIEWING, effort="")],
        ["", "Error: config.toml line 3", "[]", _MEETING],
        ["exec runs a prompt", _CODEX_FEATURES, "[]", _MEETING],
        ["--json", _CODEX_FEATURES, "not json", _MEETING],
    ],
    "claude_events": [
        [[_INIT, {"type": "assistant", "message": {"content": [
            {"type": "tool_use", "name": "mcp__openwhisper__patch_state", "input": {}},
            {"type": "text", "text": "hi"}, "junk"]}}, _RESULT], _MEETING_TOOLS],
        [[{"type": "system", "subtype": "init", "mcp_servers": [
            {"name": "openwhisper", "status": "failed"}]}], _MEETING_TOOLS],
        [[{"type": "system", "subtype": "init", "mcp_servers": []}], []],
        [[{"type": "system", "subtype": "init", "mcp_servers": []}], _ONE_TOOL],
        [[{"type": "system", "subtype": "init", "mcp_servers": [
            {"name": "openwhisper", "status": "pending"}, "x"]}], _ONE_TOOL],
        [[{"type": "system", "subtype": "init", "mcp_servers": "openwhisper"}], _ONE_TOOL],
        [[{"type": "system", "subtype": "init", "mcp_servers": 5}], _ONE_TOOL],
        [[{"type": "system", "subtype": "thinking_started"},
          {"type": "system", "subtype": "api_retry"}, {"type": "system", "subtype": 7},
          {"type": "system"}], []],
        [[{"type": "stream_event", "event": {"content_block": {"type": "thinking"}}},
          {"type": "stream_event", "event": {"delta": {"type": "text_delta"}}},
          {"type": "stream_event", "event": {"content_block": {"type": "tool_use",
                                                               "name": "x"}}},
          {"type": "stream_event", "event": {"delta": {"type": "input_json_delta"}}},
          {"type": "stream_event", "event": {"content_block": {}, "delta": {"type": "other"}}},
          {"type": "stream_event", "event": "junk"},
          {"type": "stream_event"}, {"type": "stream_event", "event": {"delta": "x"}}], []],
        [[{"type": "result", "subtype": "error_max_turns", "is_error": True, "result": ""}], []],
        [[{"type": "result", "subtype": "success", "is_error": True,
           "result": "Not logged in \u00b7 Please run /login"}], []],
        [[{"type": "result", "subtype": None, "result": "ok"}], []],
        [[{"type": "result", "result": "ok", "usage": None, "num_turns": 0}], []],
        [[{"type": "result", "subtype": "success", "result": 5, "total_cost_usd": True,
           "usage": {"input_tokens": "12", "output_tokens": 2.9}}], []],
        [[{"type": "result", "subtype": "", "is_error": False, "result": ""}], []],
        [[{"type": "result", "subtype": "success", "usage": {"input_tokens": "lots"}}], []],
        [[{"type": "result", "subtype": "success", "usage": "junk"}], []],
        [[{"type": "assistant", "message": "junk"}, {"type": "assistant"},
          {"type": "assistant", "message": {"content": [{"type": "tool_use"}]}}], []],
    ],
    "codex_events": [
        [[{"type": "thread.started", "thread_id": "t1"}, {"type": "turn.started"},
          {"type": "item.started", "item": {"type": "reasoning"}},
          {"type": "item.completed", "item": {"type": "mcp_tool_call", "tool": "patch_state"}},
          {"type": "item.updated", "item": {"type": "agent_message", "text": "partial"}},
          {"type": "item.completed", "item": {"type": "agent_message", "text": "Done."}},
          {"type": "item.completed", "item": {"type": "command_execution"}},
          {"type": "item.completed", "item": {"type": "error", "message": "x"}},
          {"type": "turn.completed", "usage": {"input_tokens": 300, "cached_input_tokens": 200,
                                               "output_tokens": 30}}]],
        [[{"type": "turn.failed", "error": {"message": "boom"}}]],
        [[{"type": "turn.failed"}, {"type": "error"}]],
        [[{"type": "turn.failed", "error": "a string"}]],
        [[{"type": "error", "message": 5}]],
        [[{"type": "item.completed", "item": "junk"},
          {"type": "item.completed", "item": {"type": "agent_message", "text": None}},
          {"type": "turn.completed"}]],
        [[{"type": "turn.completed", "usage": {"input_tokens": None, "output_tokens": "7"}}]],
    ],
    "claude_finish": [
        [{"error": "Not logged in"}, 1, ""],
        [{"error": "boom", "usage": {"total_tokens": 3}}, 0, ""],
        [{"done": True, "text": "Done."}, 0, ""],
        [{}, 2, "first\n\n  \nlast line here  \n"],
        [{}, None, ""],
        [{}, 1, "z" * 300],
    ],
    "codex_finish": [
        [{"error": '{"error": {"message": "The model is overloaded."}}'}, 1, ""],
        [{"done": True, "text": "Done."}, 0, ""],
        [{}, 3, "warning\nfatal: no"],
        [{}, None, "   "],
    ],
    "codex_features": [
        [_CODEX_FEATURES], [""], ["   \n"], ["Error: config.toml line 3"],
        ["\n".join(name + " stable true" for name in (
            "worktrees", "js_repl", "hooks", "shell_tool", "apps", "personality"))],
        ["shell_tool removed true\napps stable false\nunified_exec beta true extra"],
    ],
    "codex_servers": [
        [_SERVERS], ["[]"], [""], ["not json"], ["{}"], ['[{"name":"x.y","enabled":true}]'],
        ['["x"]'], ['[{"enabled": true}]'], ['[{"name": "a", "enabled": 0}]'],
        ['[{"name": "openwhisper.extra"}]'], ['[{"name": "z"}, {"name": "A"}, {"name": "_"}]'],
    ],
    "opencode_overlay": [
        [False, {"build": "COPILOT", "plan": "NOTE TAKER"}, "http://127.0.0.1:5/mcp/tok", "tok"],
        [False, {"build": "COPILOT"}, "http://127.0.0.1:5/mcp/tok", "tok"],
        [True, {"build": "Return only the cleaned text."}, "http://127.0.0.1:6/mcp/a", "a"],
        [True, {}, "http://127.0.0.1:6/mcp/a", "a"],
    ],
    "opencode_request": [
        ["session/request_permission", {"toolCall": {"title": "openwhisper_patch_state"},
                                        "options": [{"optionId": "a", "kind": "allow_once"},
                                                    {"optionId": "r", "kind": "reject_once"}]}],
        ["session/request_permission", {"toolCall": {"title": "bash"},
                                        "options": [{"optionId": "a", "kind": "allow_once"},
                                                    {"optionId": "r", "kind": "reject_once"}]}],
        ["session/request_permission", {"toolCall": {"rawInput": {"tool": "openwhisper_x"}},
                                        "options": [{"optionId": "aa", "kind": "allow_always"}]}],
        ["session/request_permission", {"toolCall": {"title": "bash"},
                                        "options": [{"optionId": "ra", "kind": "reject_always"},
                                                    {"optionId": "a", "kind": "allow_once"}]}],
        ["session/request_permission", {"toolCall": {"title": "bash"},
                                        "options": [{"optionId": "a", "kind": "allow_once"}]}],
        ["session/request_permission", {"toolCall": None, "options": None}],
        ["session/request_permission", {"options": ["x", 5, {"kind": None},
                                                    {"kind": "reject_once"}]}],
        ["session/request_permission", {"options": {"reject_once": 1}}],
        ["fs/read_text_file", {"path": "C:/secrets"}],
        ["terminal/create", {}],
    ],
    "opencode_pass_model": [
        [{"configOptions": [{"id": "model", "type": "select", "currentValue": "prov/slow",
                             "options": [{"value": "prov/fast", "name": "prov/Fast"},
                                         {"value": "prov/slow", "name": "prov/Slow"}]}]},
         "prov/fast", ""],
        [{"configOptions": [{"id": "model", "type": "select", "currentValue": "prov/slow",
                             "options": [{"value": "prov/fast"}]}]}, "gone/model", ""],
        [{"configOptions": [{"id": "model", "type": "select", "currentValue": "prov/slow",
                             "options": [{"value": "prov/fast"}]}]}, "", "prov/fast"],
        [{"configOptions": [{"id": "model", "type": "select", "currentValue": "prov/slow",
                             "options": [{"value": "prov/fast"}]}]}, "", "gone/default"],
        [{"configOptions": []}, "", ""],
        [{}, "it's/model", ""],
    ],
    "opencode_failed": [
        ["Provider auth required", False], ["session/prompt timed out after 30s", False],
        ["OpenCode exited", False], ["boom", True], ["", False], ["AUTHORIZATION", False],
    ],
    "opencode_outcome": [
        [{"stopReason": "end_turn", "usage": {"inputTokens": 40, "outputTokens": 5,
                                              "totalTokens": 45}}],
        [{"stopReason": "end_turn", "usage": {"inputTokens": 40, "cachedReadTokens": 10,
                                              "outputTokens": 5}}],
        [{"stopReason": "cancelled"}], [{"stopReason": "refusal", "usage": None}],
        [{"stopReason": "max_tokens"}], [None], ["text"], [{}],
        [{"usage": {"totalTokens": 0, "inputTokens": 3}}],
    ],
    "tail_detail": [
        [""], ["only"], ["first\nsecond\n\n  \n"], ["x" * 300], ["a\r\nb\rc"],
    ],
}

PINS = {
    "outcome_note": "outcomes are [ok, text, error, usage, canceled]",
    "path_note": "a command's working folder reads <workdir>, the agent workspace <workspace>",
}
