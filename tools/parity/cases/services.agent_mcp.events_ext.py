"""Curated inputs for services/agent_mcp/events_ext.py (M5.12 mcp-events).

The functions that refuse with EventsError are wrapped to give {"ok": result} or
{"error": {"code", "message", "data"}}, so a vector holds the JSON-RPC error a client reads.

    normalize_arguments(name, arguments)
    validate_callback_url(url)
    validate_secret(secret)
    granted_ttl_ms(params)                   _granted_ttl_ms
    subscription_id(name, arguments, url)    with OpenWhisper's principal
    descriptors()
    event_body(id, seq, type, time, data)    _event_body of an events.Event, as text
    status_category(status)                  _status_category
    retry_after(value)                       _retry_after
    iso(timestamp)                           _iso
    parse_iso(value)                         _parse_iso
    delivery(value, subscribing)             _delivery
    event_name(value)                        _event_name
    matches(arguments, data)                 _matches
    from_record(record)                      EventSubscription.from_record(...).to_record()
                                             with its origin, or None
"""


def _outcome(call):
    from services.agent_mcp.events_ext import EventsError

    try:
        return {"ok": call()}
    except EventsError as exc:
        return {"error": {"code": exc.code, "message": exc.message, "data": exc.data}}


def _event_body(m, event_id, seq, event_type, time, data):
    from services.automation import events

    event = events.Event(id=event_id, seq=seq, type=event_type, time=time, data=data)
    return m._event_body(event).decode("ascii")


def _from_record(m, record):
    subscription = m.EventSubscription.from_record(record)
    return None if subscription is None else subscription.to_record()


WRAPPERS = {
    "normalize_arguments": lambda m, name, arguments: _outcome(
        lambda: m.normalize_arguments(name, arguments)),
    "validate_callback_url": lambda m, url: _outcome(lambda: m.validate_callback_url(url)),
    "validate_secret": lambda m, secret: _outcome(lambda: m.validate_secret(secret)),
    "granted_ttl_ms": lambda m, params: _outcome(lambda: m._granted_ttl_ms(params)),
    "subscription_id": lambda m, name, arguments, url: m.subscription_id(name, arguments, url),
    "descriptors": lambda m: m.descriptors(),
    "event_body": _event_body,
    "status_category": lambda m, status: m._status_category(status),
    "retry_after": lambda m, value: m._retry_after(value),
    "iso": lambda m, timestamp: m._iso(timestamp),
    "parse_iso": lambda m, value: m._parse_iso(value),
    "delivery": lambda m, value, subscribing: _outcome(
        lambda: list(m._delivery(value, subscribing=subscribing))),
    "event_name": lambda m, value: _outcome(lambda: m._event_name(value)),
    "matches": lambda m, arguments, data: m._matches(arguments, data),
    "from_record": _from_record,
}

SECRET = "whsec_" + "QUJDREVGR0hJSktMTU5PUFFSU1RVVldY"  # 24 bytes
HOOK = "https://example.com/hook"

CASES = {
    "normalize_arguments": [
        ["history.saved", None], ["history.saved", {}], ["history.saved", []],
        ["history.saved", "x"], ["history.saved", 5], ["history.saved", {"action_id": "a"}],
        ["recording.started", {"action_id": "a"}], ["recording.started", {"modes": ["file"]}],
        ["recording.started", {"modes": ["walkthrough", "command", "dictation"],
                               "action_id": "x"}],
        ["transcription.completed", {"modes": ["action", "transform"]}],
        ["transcription.completed", {"modes": []}],
        ["transcription.completed", {"modes": ["dictation", "dictation"]}],
        ["transcription.completed", {"modes": ["Dictation"]}],
        ["transcription.completed", {"modes": [None]}],
        ["transcription.completed", {"modes": "dictation"}],
        ["transcription.completed", {"modes": {"dictation": 1}}],
        ["transcription.completed", {"action_id": ""}],
        ["transcription.completed", {"action_id": "a" * 200}],
        ["transcription.completed", {"action_id": "a" * 201}],
        ["transcription.completed", {"action_id": "é" * 200}],
        ["transcription.completed", {"action_id": "\U0001f600" * 201}],
        ["transcription.completed", {"action_id": 5}],
        ["transcription.completed", {"action_id": True}],
        ["transcription.completed", {"x": 1, "action_id": 5}],
        ["transcription.completed", {"action_id": 5, "x": 1}],
        ["transcription.completed", {"k" * 41: 1}],
        ["transcription.completed", {"é" * 45: 1}],
        ["transcription.completed", {"it's": 1}],
        ["transcription.completed", {"say \"hi\"": 1}],
        ["transcription.completed", {"both ' and \"": 1}],
        ["transcription.completed", {"tab\there": 1}],
        ["transcription.completed", {"\u0000": 1}],
        ["transcription.completed", {"": 1}],
        ["action.ran", {"action_id": "a", "modes": ["action"]}],
        ["channel.message", {"action_id": "a"}], ["route.decided", {"action_id": "a"}],
        ["meeting.started", {"modes": ["dictation"]}],
        ["nope", {}], ["nope", {"action_id": "a"}],
    ],
    "validate_callback_url": [
        [HOOK], ["http://127.0.0.1/hook"], ["http://127.0.0.1:8080/a?b=c#d"],
        ["http://localhost/hook"], ["http://LocalHost:9/hook"], ["http://[::1]/hook"],
        ["http://[::ffff:127.0.0.1]/hook"], ["http://127.255.255.254/hook"],
        ["http://[fe80::1%25eth0]/hook"], ["http://0.0.0.0/hook"], ["http://10.0.0.1/hook"],
        ["http://example.com/hook"], ["http://app.localhost/hook"], ["ftp://example.com/hook"],
        ["https://user@example.com/"], ["https://user:pass@example.com/"],
        ["https://:pass@example.com/"], ["https://example.com:99999/"],
        ["https://example.com:-1/"], ["https://example.com:abc/"], ["https://example.com:/"],
        ["https://example.com:0080/"], ["https://example.com:65535/"],
        ["https://example.com:65536/"], ["https://example.com:000000080/"],
        ["https://exa mple.com/"], ["https://example.com/é"], [""], [None], [5], [[]],
        ["https://"], ["https://[::1/"], ["https://[v1.x]/"], ["https://[1.2.3.4]/"],
        ["//example.com/hook"], ["HTTPS://EXAMPLE.COM/"], ["https://example.com/" + "a" * 1980],
        ["https://example.com/" + "a" * 1981], ["https://éxample.com/"],
        ["\x01https://example.com/"], ["https://example.com:8080:90/"],
    ],
    "validate_secret": [
        [SECRET], ["whsec_" + "QUJD" * 21 + "QUI="], ["whsec_" + "QUJD" * 22],
        ["whsec_" + "QUJD" * 7 + "QQ=="], ["whsec_" + "QUJD" * 8], ["whsec_"], ["secret"],
        ["WHSEC_" + "QUJD" * 8], ["whsec_" + "QUJD" * 8 + "="], ["whsec_" + "QUJD" * 8 + "\n"],
        ["whsec_" + "QUJD" * 7 + "QU-_"], ["whsec_" + "QUJD" * 7 + "QU+/"],
        ["whsec_" + "QUJD" * 7 + "QQ"], ["whsec_ " + "QUJD" * 8], ["whsec_" + "é" * 32],
        [None], [5], [b"x".hex()],
    ],
    "granted_ttl_ms": [
        [{}], [{"ttlMs": None}], [{"ttlMs": 1}], [{"ttlMs": 59999}], [{"ttlMs": 60000}],
        [{"ttlMs": 60001}], [{"ttlMs": 86400000}], [{"ttlMs": 604800000}],
        [{"ttlMs": 604800001}], [{"ttlMs": 10 ** 30}], [{"ttlMs": 0}], [{"ttlMs": -1}],
        [{"ttlMs": 0.5}], [{"ttlMs": 90000.7}], [{"ttlMs": -0.0}], [{"ttlMs": 1e308}],
        [{"ttlMs": float("nan")}], [{"ttlMs": float("inf")}], [{"ttlMs": float("-inf")}],
        [{"ttlMs": True}], [{"ttlMs": False}], [{"ttlMs": "5"}], [{"ttlMs": []}],
        [{"ttlMs": {}}], [{"other": 1}],
    ],
    "subscription_id": [
        ["history.saved", {}, HOOK],
        ["transcription.completed", {"modes": ["dictation"]}, "http://127.0.0.1:9/hook"],
        ["recording.started", {"action_id": "summarize", "modes": ["action", "dictation"]},
         "http://127.0.0.1:9/hook"],
        ["recording.started", {"modes": ["action", "dictation"], "action_id": "summarize"},
         "http://127.0.0.1:9/hook"],
        ["action.ran", {"action_id": "café \U0001f600"}, "https://éxample.com/h?q=1"],
        ["action.ran", {"action_id": "quote\"back\\slash"}, "https://example.com/a b"],
    ],
    "descriptors": [[]],
    "event_body": [
        ["evt_" + "1" * 24, 3, "channel.message", "2026-10-10T12:00:00.000Z",
         {"job_id": "j", "action_id": "a", "text": "café \U0001f600", "app": None}],
        ["evt_" + "2" * 24, 4, "history.saved", "2026-10-10T12:00:01.500Z",
         {"job_id": "j", "record_id": "1", "kind": "dictation"}],
        ["evt_" + "3" * 24, 5, "recording.stopped", "2026-10-10T12:00:02.000Z",
         {"job_id": "j", "duration_s": 1.25, "nested": {"a": [1, 2.0, True, None]}}],
        ["evt_" + "4" * 24, 6, "meeting.started", "t", {"title": "line\nbreak \"quoted\""}],
        ["evt_" + "5" * 24, 7, "meeting.ended", "t", {}],
    ],
    "status_category": [
        [100], [199], [200], [204], [299], [300], [302], [399], [400], [404], [408], [410],
        [412], [413], [414], [425], [429], [499], [500], [503], [599], [600],
    ],
    "retry_after": [
        [None], [""], ["0"], ["5"], ["2.5"], [" 7 "], ["-1"], ["1e3"], ["inf"], ["nan"],
        ["abc"], ["Wed, 21 Oct 2015 07:28:00 GMT"], ["1_000"], ["5, 10"], ["+3"], ["0x10"],
    ],
    "iso": [
        [0], [1.9], [1790000000], [1790000000.999], [951782400],
    ],
    "parse_iso": [
        ["2026-11-17T12:30:45Z"], ["2026-11-17t12:30:45z"], ["2026-1-5T1:2:3Z"],
        ["2026-11- 5T12:30:45Z"], ["2026-02-29T00:00:00Z"], ["2024-02-29T00:00:00Z"],
        ["2026-04-31T00:00:00Z"], ["2026-11-17T12:30:60Z"], ["2026-11-17T12:30:61Z"],
        ["2026-11-17T24:00:00Z"], ["2026-13-17T12:30:45Z"], ["0000-01-01T00:00:00Z"],
        ["0001-01-01T00:00:00Z"], ["9999-12-31T23:59:59Z"], ["2026-11-17T12:30:45"],
        ["2026-11-17T12:30:45Z "], [" 2026-11-17T12:30:45Z"], ["2026-11-17T12:30:45+00:00"],
        ["2026-11-17 12:30:45Z"], ["2026-11-17T12:30:45.5Z"], ["20261117T123045Z"],
        ["2026-11-17T12:30Z"], [""], [None], [5], ["1970-01-01T00:00:00Z"],
        ["٢٠٢٦-11-17T12:30:45Z"], ["2026-011-17T12:30:45Z"],
    ],
    "delivery": [
        [None, True], [[], True], ["webhook", True], [{}, True], [{"mode": "poll"}, True],
        [{"mode": 5, "url": HOOK}, True], [{"mode": None}, True], [{"mode": "Webhook"}, False],
        [{"mode": "webhook"}, False], [{"mode": "webhook", "url": ""}, False],
        [{"mode": "webhook", "url": 5}, False], [{"mode": "webhook", "url": "anything"}, False],
        [{"mode": "webhook", "url": HOOK, "secret": "x"}, False],
        [{"mode": "webhook", "url": HOOK}, True], [{"mode": "webhook", "url": HOOK, "secret": 5},
                                                  True],
        [{"mode": "webhook", "url": HOOK, "secret": SECRET}, True],
        [{"mode": "webhook", "url": "http://example.com/", "secret": "x"}, True],
        [{"mode": "webhook", "secret": SECRET}, True],
    ],
    "event_name": [
        ["history.saved"], ["events.gap"], ["history.deleted"], [""], [None], [5],
        ["recording.started"], ["meeting.voice_command"], ["HISTORY.SAVED"],
    ],
    "matches": [
        [{}, {}], [{}, {"action_id": "a"}], [{"action_id": "a"}, {"action_id": "a"}],
        [{"action_id": "a"}, {"action_id": "b"}], [{"action_id": "a"}, {}],
        [{"action_id": "a"}, {"action_id": None}], [{"modes": ["dictation"]}, {"mode": "dictation"}],
        [{"modes": ["dictation"]}, {"mode": "action"}], [{"modes": ["dictation"]}, {}],
        [{"modes": ["action", "dictation"], "action_id": "x"}, {"mode": "action", "action_id": "x"}],
        [{"modes": ["action", "dictation"], "action_id": "x"}, {"mode": "action", "action_id": "y"}],
        [{"action_id": "1"}, {"action_id": 1}],
    ],
    "from_record": [
        [{"id": "sub_" + "0" * 32, "name": "history.saved", "arguments": {}, "url": HOOK,
          "refresh_before": "2099-01-01T00:00:00Z", "created_at": "c", "origin": "local"}],
        [{"id": "sub_" + "0" * 32, "name": "history.saved", "arguments": {}, "url": HOOK,
          "refresh_before": "2099-01-01T00:00:00Z", "created_at": ""}],
        [{"id": "sub_" + "0" * 32, "name": "history.saved", "url": HOOK,
          "refresh_before": "2099-01-01T00:00:00Z", "origin": "remote", "created_at": 5}],
        [{"id": "sub_x", "name": "transcription.completed",
          "arguments": {"modes": ["file", "action"], "action_id": "a"}, "url": HOOK,
          "refresh_before": "2020-01-01T00:00:00Z", "origin": "tailscale"}],
        [{"id": "sub_x", "name": "transcription.completed", "arguments": {"modes": []},
          "url": HOOK, "refresh_before": "2020-01-01T00:00:00Z"}],
        [{"id": "x_1", "name": "history.saved", "url": HOOK,
          "refresh_before": "2020-01-01T00:00:00Z"}],
        [{"id": "sub_1", "name": "events.gap", "url": HOOK,
          "refresh_before": "2020-01-01T00:00:00Z"}],
        [{"id": "sub_1", "name": "history.saved", "url": 5,
          "refresh_before": "2020-01-01T00:00:00Z"}],
        [{"id": "sub_1", "name": "history.saved", "url": HOOK, "refresh_before": "soon"}],
        [{"id": 5, "name": "history.saved", "url": HOOK,
          "refresh_before": "2020-01-01T00:00:00Z"}],
        [[]], [None], ["sub_1"],
    ],
}
