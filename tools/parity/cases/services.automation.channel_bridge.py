"""Curated inputs for services/automation/channel_bridge.py (M5.20 channel-and-cli).

Text that may hold a lone surrogate travels as JSON text, which the wrappers json.loads, so the
vectors stay valid UTF-8. Dicts whose key order Claude Code sees are given as [key, value] pairs.

    sse_feed(lines, last_id)        SseParser(last_id) fed each line in turn:
                                    {"events": [[type, data, last_id], ...], "last_id"}
    clean_meta(meta_json)           json.dumps of the kept entries as [[key, value], ...], in
                                    order (json.dumps: a lone surrogate cannot be a vector's text)
    origin_of(data)                 "dictated" or "program"
    channel_params(event_json)      json.dumps of {"content", "meta": [[key, value], ...]}, or None
    reply_arguments(arguments_json) {"text_json", "title_json"} (json.dumps of each) or
                                    {"error": the ValueError's message}
    reply_refusal(status, body)     the message; body is the answer's text
    stream_refusal(status, body)    [state, message]
    error_message(body)             _error_message of the answer's text
    parse_url(url)                  {"host", "port", "https", "base_path"} or {"error": state,
                                    "message"}; urlsplit's own ValueError raises
    target(host, port, https)       {"label", "on_this_computer"}
    link_target(url, token, saved)  AppLink(url, token, read_settings=saved).target() as
                                    parse_url's answer; saved None reads as an unreadable file
    configured_port(settings)       the port the channel connects to
    unreachable_message(url, saved) AppLink(url).unreachable_message(); saved None: unreadable
    headers(version, token, extra)  AppLink(version=...).headers(token, **extra) as pairs
    gap_reason(data)                _gap_reason of an events.gap whose data is that text
    tool_result(text, error)        _tool_result
    initialize(params_json, version) ChannelServer._initialize
    constants()                     every constant the server sends or prints
"""
import json


def _sse_feed(module, lines, last_id):
    parser = module.SseParser(last_id)
    events = []
    for line in lines:
        event = parser.feed(line)
        if event is not None:
            events.append([event.type, event.data, event.last_id])
    return {"events": events, "last_id": parser.last_id}


def _pairs(mapping):
    return [[key, value] for key, value in mapping.items()]


def _channel_params(module, event_json):
    params = module.channel_params(json.loads(event_json))
    if params is None:
        return None
    return json.dumps({"content": params["content"], "meta": _pairs(params["meta"])})


def _reply_arguments(module, arguments_json):
    try:
        text, title = module._reply_arguments(json.loads(arguments_json))
    except ValueError as exc:
        return {"error": str(exc)}
    return {"text_json": json.dumps(text), "title_json": json.dumps(title)}


def _target_answer(module, make):
    try:
        target = make()
    except module.LinkError as exc:
        return {"error": exc.state, "message": exc.message}
    return {"host": target.host, "port": target.port, "https": target.https,
            "base_path": target.base_path}


def _settings_reader(saved):
    def read():
        if saved is None:
            raise OSError("unreadable")
        return saved

    return read


def _link_target(module, url, token, saved):
    link = module.AppLink(url=url, token=token, read_settings=_settings_reader(saved),
                          read_token=lambda: None)
    return _target_answer(module, link.target)


def _unreachable_message(module, url, saved):
    return module.AppLink(url=url, read_settings=_settings_reader(saved),
                          read_token=lambda: None).unreachable_message()


def _target(module, host, port, https):
    target = module.Target(host, port, https)
    return {"label": target.label, "on_this_computer": target.on_this_computer}


def _gap_reason(module, data):
    return module._gap_reason(module.SseEvent("events.gap", data, ""))


def _initialize(module, params_json, version):
    server = module.ChannelServer(None, None, None, version=version)
    return server._initialize(json.loads(params_json))


def _constants(module):
    return {
        "PROTOCOL_VERSIONS": list(module.PROTOCOL_VERSIONS),
        "CLIENT_HEADER": module.CLIENT_HEADER,
        "CLIENT_NAME": module.CLIENT_NAME,
        "EVENTS_PATH": module.EVENTS_PATH,
        "NOTIFY_PATH": module.NOTIFY_PATH,
        "URL_ENV": module.URL_ENV,
        "TOKEN_ENV": module.TOKEN_ENV,
        "BACKOFF_FIRST_S": module.BACKOFF_FIRST_S,
        "BACKOFF_MAX_S": module.BACKOFF_MAX_S,
        "CONNECT_TIMEOUT_S": module.CONNECT_TIMEOUT_S,
        "READ_TIMEOUT_S": module.READ_TIMEOUT_S,
        "NOTIFY_TIMEOUT_S": module.NOTIFY_TIMEOUT_S,
        "MAX_REPLY_CHARS": module.MAX_REPLY_CHARS,
        "MAX_TITLE_CHARS": module.MAX_TITLE_CHARS,
        "MAX_META_CHARS": module.MAX_META_CHARS,
        "PARSE_ERROR": module.PARSE_ERROR,
        "INVALID_REQUEST": module.INVALID_REQUEST,
        "METHOD_NOT_FOUND": module.METHOD_NOT_FOUND,
        "INVALID_PARAMS": module.INVALID_PARAMS,
        "ORIGIN_OPENWHISPER": module.ORIGIN_OPENWHISPER,
        "GAP_NOTICE": module.GAP_NOTICE,
        "INSTRUCTIONS": module.INSTRUCTIONS,
        "REPLY_TOOL": module.REPLY_TOOL,
        "USAGE": module._USAGE,
    }


WRAPPERS = {
    "sse_feed": _sse_feed,
    "clean_meta": lambda m, meta_json: json.dumps(_pairs(m.clean_meta(json.loads(meta_json)))),
    "origin_of": lambda m, data: m.origin_of(data),
    "channel_params": _channel_params,
    "reply_arguments": _reply_arguments,
    "reply_refusal": lambda m, status, body: m._reply_refusal(status, body.encode("utf-8")),
    "stream_refusal": lambda m, status, body: list(m._stream_refusal(status,
                                                                      body.encode("utf-8"))),
    "error_message": lambda m, body: m._error_message(body.encode("utf-8")),
    "parse_url": lambda m, url: _target_answer(m, lambda: m.parse_url(url)),
    "target": _target,
    "link_target": _link_target,
    "configured_port": lambda m, settings: m.configured_port(settings),
    "unreachable_message": _unreachable_message,
    "headers": lambda m, version, token, extra: _pairs(
        m.AppLink(version=version).headers(token, **extra)),
    "gap_reason": _gap_reason,
    "tool_result": lambda m, text, error: m._tool_result(text, error=error),
    "initialize": _initialize,
    "constants": _constants,
}


def _gap(reason_json):
    return ('{"id": "", "seq": 0, "type": "events.gap", "v": 1, "data": {"missed_from_seq": 5, '
            '"reason": ' + reason_json + '}}')


CASES = {
    "sse_feed": [
        [[": ping", "", "id: 7", "event: channel.message", 'data: {"a":1}', "", "id: 8",
          "data: x", ""], ""],
        [["data:  two spaces", "data:none", "data", ""], ""],
        [["id: 4", "data: half an event"], "3"],
        [["id: 4", "data: half an event", ""], "3"],
        [["retry: 2000", "", "event: events.gap", 'data: {"seq": 0}', ""], "12"],
        [["id: 9", "", "id: bad\0id", "data: y", ""], ""],
        [["id", "data: empty id", ""], "5"],
        [["id:", "data: empty id", ""], "5"],
        [["id: 1", "id: 2", "data: last id wins", ""], ""],
        [["event: a", "event: b", "data: last type wins", ""], ""],
        [["event:", "data: an empty type is a message", ""], ""],
        [["event: x", "", "data: the type does not carry over", ""], ""],
        [[":comment only", "", ":", ""], "1"],
        [["data", ""], ""],
        [["data:", "data:", ""], ""],
        [["data: a: b: c", ""], ""],
        [["DATA: case matters", "Data: x", ""], ""],
        [["unknown: field", "data:x", "retry: 5", ""], ""],
        [["data:\tleading tab", ""], ""],
        [["data:   ", ""], ""],
        [["id: 10", "data: cafÃ© âœ“ ðŸ˜€", ""], "9"],
        [["id: Ã¼nÃ¯code", "data: x", ""], ""],
        [["data: no blank line yet"], "2"],
        [["", "", ""], "4"],
        [["id: 3", "", "", "data: z", ""], ""],
        [[" data: leading space is a field name", ""], ""],
        [["data : spaced name", ""], ""],
    ],
    "clean_meta": [
        [json.dumps({"job_id": "abc", "app": "  Visual\nStudio   Code ", "with-hyphen": "dropped",
                     "9lives": "dropped", "_ok": "kept", "empty": "   ", "number": 5,
                     "none": None, "long": "x" * 500})],
        ['{}'],
        [json.dumps({"origin": "dictated", "job_id": None, "action": None, "app": None})],
        [json.dumps({"a": "Ã©tÃ©", "B_2": "\txã€€yâ€¨z ", "Ã¼": "non-ascii key",
                     "ok_": "y" * 199 + "Ã©" + "z"})],
        [json.dumps({"a b": "space", "": "empty", "_": "underscore", "a1": "Â ", "c": True,
                     "d": ["list"], "e": {"k": "v"}, "f": 1.5})],
        [json.dumps({"long_word": "w" * 201, "spaced": "a  " * 100})],
        ['{"surrogate": "\\ud800 alone"}'],
        [json.dumps({"emoji": "\U0001f600" * 250})],
        [json.dumps({"newline_key\n": "x", "tab": "a\tb\nc\rd\x0be\x0cf\x1cg\x1dh\x1ei\x1fj\x85k"})],
    ],
    "origin_of": [
        [{"origin": "dictated"}], [{"origin": "program"}], [{"origin": ""}], [{"origin": None}],
        [{}], [{"origin": "Dictated"}], [{"origin": "something-new"}], [{"origin": 1}],
    ],
    "channel_params": [
        [json.dumps({"type": "channel.message", "data": {
            "job_id": "0123456789abcdef", "source": "hotkey", "origin": "dictated",
            "action_id": "claude", "text": "  fix the flaky test\n",
            "app": {"id": "code", "name": "Visual Studio Code", "category": "editor"}}})],
        [json.dumps({"data": {"text": "hi", "app": "Terminal"}})],
        [json.dumps({"data": {"text": "hi", "app": None, "action_id": None}})],
        ['null'], ['[]'], ['{}'], ['{"data": null}'], ['{"data": {"text": ""}}'],
        ['{"data": {"text": "  \\n"}}'], ['{"data": {"text": 5}}'], ['{"data": "text"}'],
        ['{"data": []}'], ['"string"'], ['5'],
        [json.dumps({"data": {"text": "x", "app": {"name": 5}, "job_id": 7, "action_id": ""}})],
        [json.dumps({"data": {"text": "x", "app": {"id": "code"}, "origin": "program"}})],
        [json.dumps({"data": {"text": "x", "app": ["list"], "job_id": "  j  ob  "}})],
        [json.dumps({"data": {"text": "Â xã€€", "origin": "dictated"}})],
        [json.dumps({"data": {"text": "ã€€", "origin": "dictated"}})],
        ['{"data": {"text": "half \\ud83d emoji"}}'],
        [json.dumps({"data": {"text": "a\nb", "job_id": "j" * 300, "app": {"name": "n" * 300}}})],
    ],
    "reply_arguments": [
        ['null'], ['{}'], ['{"text": "   "}'], ['{"text": 5}'], [json.dumps({"text": "x" * 501})],
        [json.dumps({"text": "x" * 500})], [json.dumps({"text": "  " + "x" * 500 + "  "})],
        ['{"text": "ok", "title": 5}'], [json.dumps({"text": "ok", "title": "t" * 81})],
        [json.dumps({"text": "ok", "title": "t" * 80})], ['"text"'], ['[]'], ['5'],
        ['{"text": "  Done.  ", "title": " Tests \\n passed "}'],
        ['{"text": "Done.", "title": null}'], ['{"text": "Done.", "title": ""}'],
        ['{"text": "Done.", "title": "   "}'], ['{"text": null}'], ['{"title": "only"}'],
        [json.dumps({"text": "\U0001f600" * 500})], [json.dumps({"text": "\U0001f600" * 501})],
        [json.dumps({"text": "x", "title": "a " * 41})],
        [json.dumps({"text": "ã€€xã€€", "title": "ã€€tã€€"})],
        ['{"text": "lone \\udc00 surrogate"}'], ['{"text": "x", "title": true}'],
        ['{"text": "x", "extra": 1}'],
    ],
    "reply_refusal": [
        [401, ""], [403, ""], [404, ""], [503, '{"error": {"code": "x", "message": "Not ready."}}'],
        [500, ""], [500, "not json"], [422, '{"error": {"message": 5}}'],
        [422, '{"error": "flat"}'], [422, '{"detail": "x"}'], [422, '[]'], [400, '"text"'],
        [429, '{"error": {"code": "busy"}}'], [502, '{"error": {"message": ""}}'],
        [503, '{"error": {"message": "caf\\u00e9"}}'], [500, "Ã©"],
        [409, '{"error": null}'],
    ],
    "stream_refusal": [
        [200, ""], [401, ""], [403, ""], [403, '{"error": {"message": "Allow live events."}}'],
        [403, '{"error": {"message": ""}}'], [404, ""], [500, ""], [503, '{"error": {}}'],
        [302, ""], [204, ""],
    ],
    "error_message": [
        [""], ["{}"], ['{"error": {"message": "x"}}'], ['{"error": {"message": null}}'],
        ['{"error": []}'], ['[1]'], ["5"], ["nope"], ['{"error": {"message": "cafÃ©"}}'],
    ],
    "parse_url": [
        ["http://127.0.0.1:9000"], ["http://localhost:9000/"], ["https://host.example/base/"],
        ["http://[::1]:8767"], ["127.0.0.1:8767"], ["ftp://host"], ["http://"],
        ["http://host:99999"], ["  http://127.0.0.1:8767  "], ["HTTP://Host.Example:80/A/B//"],
        ["https://host.example"], ["http://host:0"], ["http://host:"], ["http://user:pw@host:5/x"],
        ["http://host:abc"], ["http://host:-1"], ["http://host:+80"], ["http://host:ï¼˜ï¼"],
        ["http://[::1"], ["http://::1]"], ["http://[fe80::1%25eth0]:8"], ["//host:5"],
        [""], ["https:///path"], ["http://host/a?b=c#d"], ["http://host?x"], ["http:host"],
        ["http://ho\tst:8\n0"], ["http://[v1.x]:9"], ["mailto:x@y"], ["http://h:65535"],
        ["http://h:65536"], ["http://h: 5"], ["http://%41:5"], ["https://100.82.22.3:8767"],
        ["http://[::ffff:127.0.0.1]:1"], ["http://0x7f.1:2"],
    ],
    "target": [
        ["127.0.0.1", 8767, False], ["localhost", 1, False], ["LOCALHOST", 1, False],
        ["::1", 2, False], ["127.0.0.2", 3, True], ["::ffff:127.0.0.1", 4, False],
        ["10.0.0.1", 5, False], ["host.example", 443, True], ["0.0.0.0", 6, False],
        ["127.1", 7, False], ["fe80::1%3", 8, False], ["::ffff:10.0.0.1", 9, False],
        ["localhost.", 10, False], ["", 11, False], ["0:0:0:0:0:0:0:1", 12, False],
    ],
    "link_target": [
        ["", "", {"mcp_port": 9123}], ["", "", {}], ["", "", {"mcp_port": True}],
        ["", "", {"mcp_port": 70000}], ["", "", {"mcp_port": "9"}], ["", "", None],
        ["http://127.0.0.1:9000", "", None], ["http://localhost:9000/", "t", None],
        ["https://host.example/base/", "t", None], ["http://[::1]:8767", "", None],
        ["https://openwhisper.example/", "", None], ["https://100.82.22.3:8767", "", None],
        ["http://203.0.113.5:8767", "explicit", None], ["http://openwhisper.example/", "x", None],
        ["https://openwhisper.example/base", "explicit", None], ["http://127.0.0.2:9", "", None],
        ["   ", "", {"mcp_port": 1}], ["127.0.0.1:8767", "", None], ["  ", " ", {}],
        ["https://host.example", "   ", None],
    ],
    "configured_port": [
        [{"mcp_port": 9123}], [{}], [{"mcp_port": True}], [{"mcp_port": 70000}],
        [{"mcp_port": "9"}], [{"mcp_port": 1}], [{"mcp_port": 65535}], [{"mcp_port": 0}],
        [{"mcp_port": 8767.0}], [{"mcp_port": None}], [{"mcp_port": -5}],
    ],
    "unreachable_message": [
        ["", {"mcp_enabled": False}], ["", {"mcp_enabled": True}], ["", {}], ["", None],
        ["http://127.0.0.1:1", {"mcp_enabled": False}], ["", {"mcp_enabled": 0}],
        ["", {"mcp_enabled": "false"}], ["  ", {"mcp_enabled": False}],
    ],
    "headers": [
        ["2.7.0", "tok", {"Accept": "text/event-stream"}], ["", "tok", {}],
        ["9.9.9", "t", {"Accept": "text/event-stream", "Cache-Control": "no-cache"}],
        ["1", "t", {"Content-Type": "application/json"}],
    ],
    "gap_reason": [
        [_gap('"evicted"')], [_gap('"restarted"')], [_gap('"slow"')], [_gap('5')],
        [_gap('null')], ['{"data": {}}'], ['{"data": []}'], ['[]'], ['not json'], [''],
        ['{"data": {"reason": "evicted"}}'], ['{"reason": "evicted"}'],
    ],
    "tool_result": [["Shown to the user near their pointer.", False], ["x", True]],
    "initialize": [
        ['{"protocolVersion": "2025-11-25"}', "9.9.9"], ['{"protocolVersion": "2025-06-18"}', ""],
        ['{"protocolVersion": "2025-03-26"}', "1"], ['{"protocolVersion": "2024-11-05"}', "1"],
        ['{"protocolVersion": "2026-07-28"}', "1"], ['{"protocolVersion": "1999-01-01"}', "1"],
        ['{"protocolVersion": null}', "1"], ['{}', "1"], ['{"protocolVersion": 5}', "1"],
        ['{"protocolVersion": ["2025-11-25"]}', "1"],
        ['{"clientInfo": {"name": "claude-code", "version": "2.1.296"}}', "2.8.92"],
    ],
    "constants": [[]],
}
