"""Curated inputs for services/automation/api.py (M5.16 live-api).

Text that may hold a lone surrogate travels as JSON text, which the wrappers json.loads, so the
vectors stay valid UTF-8. A refusal (_Invalid) is a result, not a raise, so its message, status
and code are pinned:

    parse_types(raw)                  {"value": sorted names or None} or {"invalid", "status", "code"}
    parse_last_event_id(raw)          {"value": int or None} or the refusal
    sse_frame(event, with_id)         the frame as text; event = {id, seq, type, time, data_json}
    gap_event(missed, reason)         the event without its random id and its time:
                                      {seq, type, data, id_prefix, id_length}
    string(body_json, key, required, max_chars, label)
                                      _string: {"value"} or the refusal
    is_unicode_text(text_json)        bool
    peer_is_local(host)               bool; "" is a request without a client
"""
import json


def _outcome(module, fn, *args, **kwargs):
    try:
        return {"value": fn(*args, **kwargs)}
    except module._Invalid as exc:
        return {"invalid": exc.message, "status": exc.status, "code": exc.code}


def _parse_types(module, raw):
    outcome = _outcome(module, module.parse_types, raw)
    if outcome.get("value") is not None:
        outcome["value"] = sorted(outcome["value"])
    return outcome


def _sse_frame(module, event, with_id):
    from services.automation.events import Event

    built = Event(id=event["id"], seq=event["seq"], type=event["type"], time=event["time"],
                  data=json.loads(event["data_json"]))
    return module.sse_frame(built, with_id=with_id).decode("ascii")


def _gap_event(module, missed, reason):
    event = module.gap_event(missed, reason)
    return {"seq": event.seq, "type": event.type, "data": event.data,
            "id_prefix": event.id[:4], "id_length": len(event.id)}


def _peer_is_local(module, host):
    class Request:
        client = type("Client", (), {"host": host})() if host else None

    return module.peer_is_local(Request())


WRAPPERS = {
    "parse_types": _parse_types,
    "parse_last_event_id": lambda m, raw: _outcome(m, m.parse_last_event_id, raw),
    "sse_frame": _sse_frame,
    "gap_event": _gap_event,
    "string": lambda m, body_json, key, required, max_chars, label: _outcome(
        m, m._string, json.loads(body_json), key, required=required, max_chars=max_chars,
        label=label),
    "is_unicode_text": lambda m, text_json: m.is_unicode_text(json.loads(text_json)),
    "peer_is_local": _peer_is_local,
}


def _event(seq, type_, data_json, time="2026-10-10T12:00:00.000Z"):
    return {"id": "evt_" + "0123456789abcdef01234567", "seq": seq, "type": type_, "time": time,
            "data_json": data_json}


CASES = {
    "parse_types": [
        [None], [""], [" , "], [",,"], ["recording.started"], ["recording.started, history.saved"],
        ["history.saved,recording.started,history.saved"], [" events.gap "], ["action.ran,"],
        ["recording.started,nope"], ["nope"], ["Recording.Started"], ["recording.started;x"],
        [" recording.started "], ["meeting.voice_command,channel.message,route.decided"],
        ["transcription.preview,filter.failed,meeting.segments"],
    ],
    "parse_last_event_id": [
        [None], [""], ["   "], ["0"], ["42"], [" 42 "], ["\t7\n"], ["9" * 18], ["9" * 19],
        ["1" * 19], ["-1"], ["+1"], ["1.5"], ["1e3"], ["evt_1"], ["0x10"], ["1_000"],
        ["000000000000000001"], ["0000000000000000001"], [" 12 "],
    ],
    "sse_frame": [
        [_event(1, "recording.started", '{"job_id": "j"}'), True],
        [_event(7, "transcription.completed",
                '{"job_id": "j", "text": "two\\nlines\\r\\n \\u2028 \\u2029 \\u0085 caf\\u00e9 '
                '\\ud83d\\ude00 \\"q\\" </script> \\\\ \\t"}'), True],
        [_event(7, "transcription.completed", '{"text": "half \\ud83d an emoji \\udc00"}'), True],
        [_event(0, "events.gap", '{"missed_from_seq": 2, "reason": "evicted"}'), False],
        [_event(1756728000123, "history.saved", '{}'), True],
        [_event(3, "action.ran", '{"nested": {"a": [1, 2.5, null, true, false]}, "n": -0.0}'),
         True],
        [_event(4, "x", '{"big": 12345678901234567890, "f": 1e-7, "g": 1e22}'), True],
    ],
    "gap_event": [
        [1, "evicted"], [2, "restarted"], [1756728000124, "slow"], [0, "evicted"],
    ],
    "string": [
        ['{"text": "hi"}', "text", True, 20000, ""],
        ['{}', "text", True, 20000, ""],
        ['{}', "title", False, 100, ""],
        ['{"title": null}', "title", False, 100, ""],
        ['{"text": null}', "text", True, 20000, ""],
        ['{"text": ""}', "text", True, 20000, ""],
        ['{"title": ""}', "title", False, 100, ""],
        ['{"text": 5}', "text", True, 500, ""],
        ['{"title": 5}', "title", False, 100, ""],
        ['{"title": true}', "title", False, 100, ""],
        ['{"text": ["x"]}', "text", True, 2048, "url"],
        ['{"text": "%s"}' % ("x" * 64), "text", True, 64, ""],
        ['{"text": "%s"}' % ("x" * 65), "text", True, 64, ""],
        ['{"text": "%s"}' % ("x" * 65), "text", False, 64, ""],
        ['{"text": "%s"}' % ("\\u00e9" * 64), "text", True, 64, ""],
        ['{"text": "%s"}' % ("\\ud83d\\ude00" * 64), "text", True, 64, ""],
        ['{"text": "%s"}' % ("\\ud83d\\ude00" * 65), "text", True, 64, ""],
        ['{"text": "fix this \\ud83d"}', "text", True, 20000, ""],
        ['{"title": "\\udc00"}', "title", False, 100, ""],
        ['{"action_id": "a\\ud800"}', "action_id", False, 64, ""],
        ['{"text": "%s"}' % ("x" * 1001), "text", True, 1000, ""],
        ['{"text": ""}', "text", True, 1000000, "words"],
        ['{"text": 1}', "text", False, 1234567, ""],
        ['{"text": "   "}', "text", True, 500, ""],
    ],
    "is_unicode_text": [
        ['""'], ['"plain"'], ['"caf\\u00e9 \\ud83d\\ude00"'], ['"\\ud83d"'], ['"\\ude00"'],
        ['"x\\udc00y"'], ['"\\ude00\\ud83d"'], ['"\\u2028"'],
    ],
    "peer_is_local": [
        ["127.0.0.1"], ["127.8.9.10"], ["::1"], ["::ffff:127.0.0.1"], ["::1%eth0"], ["::1%3"],
        ["100.65.148.53"], ["192.168.1.10"], ["testclient"], [""], ["localhost"],
        ["0:0:0:0:0:0:0:1"], ["::ffff:7f00:1"], ["127.000.0.1"], ["::ffff:192.168.1.10"],
        ["fe80::1%3"], ["::"], ["0.0.0.0"],
    ],
}
