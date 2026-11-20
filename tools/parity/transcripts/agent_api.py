"""The read-only History API (services/agent_api/app.py) over tests/fixtures/agentapi/history.db.

Covers every route, every 4xx and 5xx answer the standalone app gives, the cursor flows, search
citations, the remote opt-in, the parameter parsing that decides 422 against 400, and what the
federation layer answers without paired clients. tests/agentapi/test_history_api.cpp replays it
against agentapi::HistoryApi::handle.
"""
from __future__ import annotations

import base64
import json
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "tests" / "fixtures" / "agentapi" / "history.db"
TOKEN = "test-history-token-" + "a" * 32
AUTH = f"Bearer {TOKEN}"
HOST = "127.0.0.1"

CONTEXT = {"token": TOKEN, "fixture": "tests/fixtures/agentapi/history.db",
           "database_name": "history # café.db"}
NORMALIZE = [
    # Paged reads that name a device keep their position on the server under a random id.
    {"pointer": "/next_cursor", "pattern": "clients-[0-9a-f]{32}"},
]


def build(tmp: Path):
    from services.agent_api.app import create_app

    database = tmp / CONTEXT["database_name"]
    shutil.copyfile(FIXTURE, database)
    return create_app(database, TOKEN)


def get(path, query=None, headers=None, **extra):
    request = {"method": "GET", "path": path, "query": query or [],
               "headers": {"Host": HOST, "Authorization": AUTH, **(headers or {})}}
    request.update(extra)
    return request


def crafted(keys, scope=None, *, operation="transcriptions", pad=True, raw=None):
    """A cursor made by hand: base64url of {"scope", "keys"}, the scope fingerprint of an
    unfiltered list unless given."""

    def make():
        from services.agent_api.store import _fingerprint

        if raw is not None:
            payload = raw
        else:
            fingerprint = scope if scope is not None else _fingerprint(
                [operation, None, {"include_remote": False, "since": None, "before": None}])
            payload = json.dumps({"scope": fingerprint, "keys": keys}).encode()
        text = base64.urlsafe_b64encode(payload).decode()
        return text if pad else text.rstrip("=")

    return make


def standard_alphabet():
    from services.agent_api.store import _fingerprint

    fingerprint = _fingerprint(
        ["transcriptions", None, {"include_remote": False, "since": None, "before": None}])
    # Keys chosen so the standard alphabet's '+' and '/' appear in the encoding.
    payload = json.dumps({"scope": fingerprint, "keys": [2461285.0, "h_a?>?>"]}).encode()
    return base64.b64encode(payload).decode()


REQUESTS = []

# --- The gate: host, origin and token on every route (test_every_route_requires_token) ---
for path, query in [("/v1/status", []), ("/v1/transcriptions", []), ("/v1/meetings", []),
                    ("/v1/search", [["q", "launch"]]), ("/openapi.json", [])]:
    for authorization in ["", "Bearer wrong", f"Basic {TOKEN}"]:
        REQUESTS.append(get(path, query, {"Authorization": authorization}))
REQUESTS += [
    get("/v1/status", headers={"Authorization": None}),
    get("/v1/status", headers={"Authorization": f"bearer {TOKEN}"}),
    get("/v1/status", headers={"Authorization": f"BEARER {TOKEN}"}),
    get("/v1/status", headers={"Authorization": f"Bearer  {TOKEN}"}),
    get("/v1/status", headers={"Authorization": f"Bearer {TOKEN} "}),
    get("/v1/status", headers={"Authorization": "Bearer"}),
    get("/v1/status", headers={"Authorization": f"Bearer {TOKEN}x"}),
    # test_token_is_not_accepted_in_query_or_cookie
    get("/v1/status", [["token", TOKEN]], {"Authorization": "", "Cookie": f"token={TOKEN}"}),
    # test_host_and_browser_origin_are_rejected
    get("/v1/status", headers={"Host": "evil.example"}),
    get("/v1/status", headers={"Origin": "https://evil.example"}),
    get("/v1/status", headers={"Origin": "null"}),
    get("/v1/status", headers={"Origin": ""}),
    get("/v1/status", headers={"Host": "localhost:8766"}),
    get("/v1/status", headers={"Host": "LOCALHOST"}),
    get("/v1/status", headers={"Host": "127.0.0.1:1"}),
    get("/v1/status", headers={"Host": "[::1]:8766"}),
    get("/v1/status", headers={"Host": ""}),
    get("/v1/status", headers={"Host": "evil.example", "Authorization": None}),
    get("/v1/nowhere", headers={"Authorization": None}),
    get("/v1/status"),
]

# --- Transcriptions: ties, normalized time, previews, details ---
REQUESTS += [
    get("/v1/transcriptions", [["limit", "1"]], label="first_page", paginate=True),
    get("/v1/transcriptions", [["limit", "7"]], paginate=True),
    get("/v1/transcriptions"),
    get("/v1/transcriptions/h_a"),
    get("/v1/transcriptions/h_cleaned"),
    get("/v1/transcriptions/h_long"),
    get("/v1/transcriptions/h%20space%20%C3%A9"),
    get("/v1/transcriptions/h_remote"),
    get("/v1/transcriptions/h_missing"),
    get("/v1/transcriptions/h_baddate"),
]

# --- Cursors are validated and bound to their filters ---
REQUESTS += [
    get("/v1/transcriptions", [["cursor", "bad"]]),
    get("/v1/transcriptions", [["cursor", {"$from": ["first_page", "/next_cursor"]}], ["q", "launch"]]),
    get("/v1/transcriptions",
        [["cursor", {"$from": ["first_page", "/next_cursor"]}], ["include_remote", "true"]]),
    get("/v1/meetings", [["cursor", {"$from": ["first_page", "/next_cursor"]}]]),
    get("/v1/search", [["q", "launch"], ["cursor", {"$from": ["first_page", "/next_cursor"]}]]),
    get("/v1/transcriptions", [["cursor", base64.urlsafe_b64encode(b"null").decode()]]),
    get("/v1/transcriptions", [["cursor", {"$from": ["first_page", "/next_cursor"]}], ["limit", "3"]]),
    get("/v1/transcriptions", [["cursor", crafted([2461285, "h_a"])], ["limit", "3"]]),
    get("/v1/transcriptions", [["cursor", crafted([2461285.0, "h_a"], pad=False)]]),
    get("/v1/transcriptions", [["cursor", crafted([True, "h_a"])]]),
    get("/v1/transcriptions", [["cursor", crafted(["2461285.0", "h_a"])]]),
    get("/v1/transcriptions", [["cursor", crafted([float("inf"), "h_a"])]]),
    get("/v1/transcriptions", [["cursor", crafted([float("nan"), "h_a"])]]),
    get("/v1/transcriptions", [["cursor", crafted([2461285.0])]]),
    get("/v1/transcriptions", [["cursor", crafted([2461285.0, 5])]]),
    get("/v1/transcriptions", [["cursor", crafted([2461285.0, None])]]),
    get("/v1/transcriptions", [["cursor", crafted({"a": 1, "b": 2})]]),
    get("/v1/transcriptions", [["cursor", crafted([2461285.0, "h_a"], scope=["x"])]]),
    get("/v1/transcriptions", [["cursor", crafted([2461285.0, "h_a"], scope="0" * 24)]]),
    get("/v1/transcriptions", [["cursor", crafted(None, raw=b"[1, 2]")]]),
    get("/v1/transcriptions", [["cursor", crafted(None, raw=b'{"scope": 1}')]]),
    get("/v1/transcriptions", [["cursor", crafted(None, raw=b"\xff\xfe")]]),
    get("/v1/transcriptions", [["cursor", standard_alphabet]]),
    get("/v1/transcriptions", [["cursor", "eyJ zY29wZSI6"]]),
    get("/v1/transcriptions", [["cursor", "A" * 2048]]),
    get("/v1/transcriptions", [["cursor", "A" * 2049]]),
    get("/v1/transcriptions", [["cursor", ""]]),
    get("/v1/transcriptions", [["cursor", crafted([1e308 * 10, "h_a"])]]),
    get("/v1/transcriptions", [["cursor", crafted([10 ** 400, "h_a"])]]),
]

# --- Search: citations, raw text, excerpts, literal matching, kinds ---
REQUESTS += [
    get("/v1/search", [["q", "launch"], ["limit", "1"]], paginate=True),
    get("/v1/search", [["q", "launch"], ["limit", "4"]], paginate=True),
    get("/v1/search", [["q", "raw phrase"]]),
    get("/v1/transcriptions", [["q", "raw phrase"]]),
    get("/v1/search", [["q", "needle"]]),
    get("/v1/search", [["q", "memo"]]),
    get("/v1/search", [["q", "roadmap"]]),
    get("/v1/search", [["q", "Launch memo"], ["kind", "transcription"]]),
    get("/v1/transcriptions", [["q", "launch memo"]]),
    get("/v1/search", [["q", "é"]]),
    get("/v1/search", [["q", "Weather"]]),
    get("/v1/search", [["q", " launch "]]),
    get("/v1/transcriptions", [["q", "  "]]),
    get("/v1/search", [["q", "x" * 500]]),
    get("/v1/search", [["q", "x" * 501]]),
]
for query in ["50%_", "' OR 1=1 --", "%", "_", "/", "path/like", "100%", "_ is"]:
    REQUESTS.append(get("/v1/search", [["q", query]]))
for query in ["ÉTÉ", "東京", "été", "ÉtÉ", "tōkyō", "TŌKYŌ"]:
    REQUESTS.append(get("/v1/search", [["q", query]]))

# --- Filters: dates, kinds, titles ---
REQUESTS += [
    get("/v1/search", [["q", "launch"], ["kind", "transcription"], ["since", "2026-09-01T00:00:00Z"],
                       ["before", "2026-09-02T00:00:00Z"]], paginate=True),
    get("/v1/meetings", [["q", "review"]]),
    get("/v1/meetings", [["q", "Friday"]]),
    get("/v1/search", [["q", "Friday"], ["kind", "meeting"]]),
    get("/v1/search", [["q", "launch"], ["kind", "meeting"], ["limit", "2"]], paginate=True),
    get("/v1/meetings", [["limit", "2"]], paginate=True),
    get("/v1/meetings", [["since", "2026-08-01T00:00:00+00:00"], ["before", "2026-09-01T12:00:00Z"]]),
    get("/v1/meetings", [["since", "2026-09-01T14:00:00+02:00"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T12:00:00.25Z"]]),
    get("/v1/transcriptions", [["since", "1756728000"]]),
    get("/v1/transcriptions", [["before", "1756728000000"]]),
    get("/v1/transcriptions", [["since", "2026-08-15T10:00:00Z"], ["limit", "3"]], paginate=True),
    get("/v1/transcriptions", [["before", "2026-08-15 10:00:00.000001+00:00"]]),
    get("/v1/transcriptions", [["since", "2026-08-15t12:00:00+0200"], ["before", "2026-08-16"]]),
    get("/v1/transcriptions", [["since", "2026-08-15_10:00Z"]]),
    get("/v1/search", [["q", "tie"], ["since", "-1.5"], ["limit", "2"]]),
    get("/v1/search", [["q", "tie"], ["since", ".5"]]),
    get("/v1/transcriptions", [["since", "1700000000.123456789"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T00:00:00.1234567+01:00"]]),
    get("/v1/transcriptions", [["since", "2026-09-01"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T00:00"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T00"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T00:00:00+05"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T24:00:00Z"]]),
    get("/v1/transcriptions", [["since", "2026-02-29T00:00:00Z"]]),
    get("/v1/transcriptions", [["since", "2024-02-29T00:00:00Z"]]),
    get("/v1/transcriptions", [["since", "1e3"]]),
    get("/v1/transcriptions", [["since", "1.7e9"]]),
    get("/v1/transcriptions", [["since", " 2026-09-01T00:00:00Z"]]),
    get("/v1/transcriptions", [["since", "99999999999999999"]]),
    get("/v1/transcriptions", [["since", "0001-01-01T00:00:00+01:00"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T00:00:00+23:59"]]),
    get("/v1/transcriptions", [["since", "2026-09-01T00:00:00+24:00"]]),
]

# --- Remote copies need an explicit opt-in on every surface ---
for path in ["/v1/transcriptions/h_remote", "/v1/meetings/m_remote", "/v1/meetings/m_remote/segments",
             "/v1/meetings/m_remote/segments/sg_remote", "/v1/meetings/m_remote/insights"]:
    REQUESTS.append(get(path))
    REQUESTS.append(get(path, [["include_remote", "true"]]))
REQUESTS += [
    get("/v1/transcriptions", [["include_remote", "true"], ["limit", "10"]], paginate=True),
    get("/v1/meetings", [["include_remote", "true"]]),
    get("/v1/search", [["q", "launch"], ["include_remote", "true"], ["limit", "5"]], paginate=True),
    get("/v1/search", [["q", "remote"], ["include_remote", "true"]]),
]
for value in ["yes", "on", "1", "TRUE", "t", "Y", "no", "off", "0", "F", "2", "", "true ", "maybe"]:
    REQUESTS.append(get("/v1/transcriptions/h_remote", [["include_remote", value]]))

# --- Segments: ranges, cursors, meeting scope ---
REQUESTS += [
    get("/v1/meetings/m_a/segments", [["limit", "1"]], label="segment_page", paginate=True),
    get("/v1/meetings/m_a/segments", [["start_s", "0"], ["end_s", "10"]], paginate=True),
    get("/v1/meetings/m_a/segments", [["start_s", "3.5"]]),
    get("/v1/meetings/m_a/segments", [["end_s", "3.5"]]),
    get("/v1/meetings/m_a/segments", [["start_s", "-0"]]),
    get("/v1/meetings/m_a/segments", [["start_s", " 1e0 "], ["end_s", "1_0"]]),
    get("/v1/meetings/m_a/segments",
        [["cursor", {"$from": ["segment_page", "/next_cursor"]}], ["include_remote", "true"]]),
    get("/v1/meetings/m_a/segments",
        [["cursor", {"$from": ["segment_page", "/next_cursor"]}], ["start_s", "0"]]),
    get("/v1/meetings/m_b/segments", [["cursor", {"$from": ["segment_page", "/next_cursor"]}]]),
    get("/v1/meetings/m_b/segments"),
    get("/v1/meetings/m_a/segments/sg_a"),
    get("/v1/meetings/m_a/segments/sg_remote"),
    get("/v1/meetings/m_b/segments/sg_a"),
    get("/v1/meetings/m_empty/segments/sg_a"),
    get("/v1/meetings/m_missing/segments"),
    get("/v1/meetings/m_empty/segments"),
    get("/v1/meetings/m_a/segments", [["start_s", "-1"]]),
    get("/v1/meetings/m_a/segments", [["end_s", "nan"]]),
    get("/v1/meetings/m_a/segments", [["end_s", "inf"]]),
    get("/v1/meetings/m_a/segments", [["end_s", "1e400"]]),
    get("/v1/meetings/m_a/segments", [["start_s", "0x1"]]),
    get("/v1/meetings/m_a/segments", [["start_s", "10"], ["end_s", "5"]]),
    get("/v1/meetings/m_a/segments", [["start_s", "5"], ["end_s", "5"]]),
]

# --- Insights ---
for meeting in ["m_a", "m_b", "m_empty", "m_corrupt", "m_odd", "m_badtype", "m_list", "m_active",
                "m_missing"]:
    REQUESTS.append(get(f"/v1/meetings/{meeting}/insights"))
REQUESTS += [get("/v1/meetings/m_a"), get("/v1/meetings/m_active"), get("/v1/meetings/m_missing")]

# --- Private fields never appear (test_private_fields_are_never_serialized) ---
for path, query in [("/v1/meetings", []), ("/v1/meetings/m_a/segments", []),
                    ("/v1/search", [["q", "a"], ["limit", "100"]])]:
    REQUESTS.append(get(path, query))

# --- Parameter bounds (422) and semantic checks (400) ---
for path, query in [
    ("/v1/search", [["q", ""]]),
    ("/v1/search", []),
    ("/v1/search", [["q", "x"], ["limit", "101"]]),
    ("/v1/search", [["q", "x"], ["kind", "bad"]]),
    ("/v1/search", [["q", "x"], ["kind", "ALL"]]),
    ("/v1/transcriptions", [["limit", "0"]]),
    ("/v1/transcriptions", [["limit", "abc"]]),
    ("/v1/transcriptions", [["limit", "5.5"]]),
    ("/v1/transcriptions", [["limit", "2e1"]]),
    ("/v1/transcriptions", [["limit", "20."]]),
    ("/v1/transcriptions", [["limit", "-1"]]),
    ("/v1/transcriptions", [["limit", "99999999999999999999"]]),
    ("/v1/transcriptions", [["q", ""]]),
    ("/v1/transcriptions", [["device_id", ""]]),
    ("/v1/transcriptions", [["device_id", "d" * 65]]),
    ("/v1/meetings", [["since", "bad"]]),
    ("/v1/meetings", [["before", ""]]),
    ("/v1/meetings", [["include_clients", "perhaps"]]),
    ("/v1/search", [["q", "%20%20"]]),
    ("/v1/search", [["q", "　"]]),
    ("/v1/search", [["q", " launch "]]),
    ("/v1/meetings", [["since", "2026-09-01T00:00:00"]]),
    ("/v1/meetings", [["since", "2026-09-02T00:00:00Z"], ["before", "2026-09-01T00:00:00Z"]]),
    ("/v1/meetings", [["since", "2026-09-01T00:00:00Z"], ["before", "2026-09-01T02:00:00+02:00"]]),
    ("/v1/meetings", [["before", "2026-09-01T00:00:00"], ["limit", "0"]]),
    ("/v1/meetings", [["since", "bad"], ["limit", "0"]]),
    ("/v1/search", [["q", ""], ["since", "2026-09-01T00:00:00"]]),
    ("/v1/transcriptions", [["limit", " 5"]]),
    ("/v1/transcriptions", [["limit", "5.0"]]),
    ("/v1/transcriptions", [["limit", "1_0"]]),
    ("/v1/transcriptions", [["limit", "+3"]]),
    ("/v1/transcriptions", [["limit", "1"], ["limit", "2"]]),
    ("/v1/transcriptions", [["unknown", "x"], ["limit", "2"]]),
]:
    REQUESTS.append(get(path, query))
REQUESTS.append(get("/v1/search", [["q", "%20%20"]], note="q is the literal text '%20%20'"))
REQUESTS.append(get("/v1/search?q=%20%20", note="q is two spaces"))

# --- The OpenAPI document, methods and unknown paths ---
REQUESTS += [
    get("/openapi.json"),
    {**get("/v1/transcriptions/h_a"), "method": "DELETE"},
    {**get("/v1/status"), "method": "POST", "body": "{}"},
    {**get("/v1/status"), "method": "HEAD"},
    {**get("/openapi.json"), "method": "POST"},
    {**get("/nothing"), "method": "PUT"},
    get("/docs"),
    get("/redoc"),
    get("/v1/unknown"),
    get("/"),
    get("/v1/transcriptions/"),
    get("/v1/status/", [["x", "1"]]),
    get("/v1/meetings/m_a/segments/"),
    get("/v1/transcriptions//"),
    get("/v1/transcriptions/a%2Fb"),
    get("/v1/meetings/m%20a"),
]

# --- Paired clients: the standalone app has none ---
REQUESTS += [
    get("/v1/transcriptions", [["include_clients", "true"]]),
    get("/v1/search", [["q", "launch"], ["include_clients", "1"]]),
    get("/v1/meetings", [["include_clients", "true"], ["device_id", "device_a"]]),
    get("/v1/transcriptions", [["device_id", "device_a"]]),
    get("/v1/transcriptions", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/meetings", [["device_id", "device_a"], ["include_remote", "true"], ["limit", "1"]]),
    get("/v1/search", [["q", "launch"], ["device_id", "device_a"], ["include_remote", "true"],
                       ["limit", "1"]], label="client_search", paginate=True),
    get("/v1/search", [["q", "Launch"], ["device_id", "device_a"], ["include_remote", "true"],
                       ["limit", "1"], ["cursor", {"$from": ["client_search", "/next_cursor"]}]]),
    get("/v1/search", [["q", "launch"], ["device_id", "device_a"], ["include_remote", "true"],
                       ["limit", "1"], ["cursor", "clients-" + "0" * 32]]),
    get("/v1/search", [["q", "launch"], ["cursor", {"$from": ["client_search", "/next_cursor"]}]]),
    get("/v1/search", [["q", "launch"], ["device_id", "device_b"], ["include_remote", "true"]]),
    get("/v1/transcriptions/h_remote", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/transcriptions/h_remote", [["device_id", "device_b"], ["include_remote", "true"]]),
    get("/v1/transcriptions/h_remote", [["device_id", "device_a"]]),
    get("/v1/transcriptions/h_a", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/transcriptions/h_missing", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/transcriptions/h_a", [["device_id", ""], ["include_remote", "true"]]),
    get("/v1/meetings/m_remote", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/meetings/m_remote/segments", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/meetings/m_remote/segments/sg_remote",
        [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/meetings/m_remote/insights", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/meetings/m_a/insights", [["device_id", "device_a"], ["include_remote", "true"]]),
    get("/v1/meetings/m_missing/segments", [["device_id", "device_a"], ["include_remote", "true"]]),
]
