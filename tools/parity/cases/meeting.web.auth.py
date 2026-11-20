"""Curated inputs for meeting/web/auth.py (M7.13 meeting-web-hub).

    resolve_role(token, host_token, guest_token)    "host", "guest" or None

Tokens of other types are the rows a corrupt meeting could hold; generate_token is random and is
checked by shape in tests/meeting_web/test_auth.cpp.
"""

HOST = "host-token-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
GUEST = "guest-token-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

CASES = {
    "resolve_role": [
        [HOST, HOST, GUEST],
        [GUEST, HOST, GUEST],
        ["nope", HOST, GUEST],
        ["", HOST, GUEST],
        [None, HOST, GUEST],
        ["same", "same", "same"],
        [HOST, None, GUEST],
        [GUEST, HOST, None],
        [HOST, "", GUEST],
        [GUEST, HOST, ""],
        ["", "", ""],
        [HOST[:-1], HOST, GUEST],
        [HOST + "x", HOST, GUEST],
        [HOST.upper(), HOST, GUEST],
        [" " + HOST, HOST, GUEST],
        ["été", "été", GUEST],
        ["42", 42, None],
        ["4", 42, None],
        [HOST, 0, GUEST],
        ["True", True, None],
        [42, "42", None],
    ],
}
