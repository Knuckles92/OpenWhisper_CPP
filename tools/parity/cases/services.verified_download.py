"""Curated inputs for services/verified_download.py (M1.7).

`resume` drives download_verified against a scripted opener: the bytes an earlier attempt left in
`<destination>.part`, the responses the server gives in order, and the options. The result is
what a caller can observe afterwards: the outcome, the message, the Range header of every
request, what is left in `.part` and at the destination, and the progress events.

`describe` builds the exception a urllib call raises from a descriptor and returns
describe_network_error's text.
"""

import email.message
import io
import shutil
import ssl
import threading
import urllib.error

import ow_isolation

BODY = b"component-payload"
URL = "https://files.example.test/payload.bin"
HOST = "files.example.test"


def _message(pairs):
    message = email.message.Message()
    for name, value in pairs:
        message[name] = value
    return message


def _bytes(spec):
    """[a, b]: BODY[a:b]; {"junk": n}: n bytes of b"x"; a list of those: their concatenation."""
    if isinstance(spec, dict):
        return b"x" * spec["junk"]
    if spec and isinstance(spec[0], (list, dict)):
        return b"".join(_bytes(part) for part in spec)
    return BODY[spec[0]:spec[1]]


class _Response:
    """One scripted response: the body is read in one piece, then `fail` (if any) is raised."""

    def __init__(self, status, headers, body, fail, on_read):
        self.status = status
        self.headers = _message(headers)
        self._chunks = [body] if body else []
        self._fail = fail
        self._on_read = on_read

    def read(self, size=-1):
        if self._on_read is not None:
            self._on_read()
        if self._chunks:
            return self._chunks.pop(0)
        if self._fail is not None:
            fail, self._fail = self._fail, None
            raise TimeoutError(fail) if fail == "timed out" else ConnectionResetError(fail)
        return b""

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


def _resume(module, prefix, responses, options):
    """prefix: null (no .part), or a byte spec for the .part's content.

    A response is {"status", "reason", "headers": [[name, value]], "body": spec, "fail": text},
    or {"url_error": reason}, or {"certificate": true}. options: "size", "max_bytes",
    "keep_partial_on_cancel", "cancel_on_read" (cancel is set at the first body read).
    """
    folder = ow_isolation.SCRATCH / "verified-download-cases"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    destination = folder / "payload.bin"
    part = folder / "payload.bin.part"
    if prefix is not None:
        part.write_bytes(_bytes(prefix))

    cancel = threading.Event()
    on_read = cancel.set if options.get("cancel_on_read") else None
    script = list(responses)
    requests = []
    progress = []

    def opener(url, headers=None):
        requests.append((headers or {}).get("Range"))
        spec = script.pop(0)
        if "url_error" in spec:
            raise urllib.error.URLError(spec["url_error"])
        if spec.get("certificate"):
            raise urllib.error.URLError(ssl.SSLCertVerificationError("self-signed"))
        body = _bytes(spec.get("body", [0, 0]))
        status = spec.get("status", 200)
        if not 200 <= status < 300:
            raise urllib.error.HTTPError(url, status, spec.get("reason", ""),
                                         _message(spec.get("headers", [])), io.BytesIO(body))
        return _Response(status, spec.get("headers", []), body, spec.get("fail"), on_read)

    outcome, message = "ok", None
    try:
        module.download_verified(
            URL,
            module.hashlib.sha256(BODY).hexdigest(),
            options.get("size", len(BODY)),
            str(destination),
            lambda *event: progress.append(list(event)),
            cancel,
            opener=opener,
            describe_error=lambda exc: module.describe_network_error(exc, hosts=(HOST,)),
            max_bytes=options.get("max_bytes"),
            keep_partial_on_cancel=options.get("keep_partial_on_cancel", False),
        )
    except module.DownloadError as exc:
        outcome, message = type(exc).__name__, str(exc)
    result = {
        "outcome": outcome,
        "message": message,
        "requests": requests,
        "part": part.read_bytes().decode("latin-1") if part.exists() else None,
        "destination": destination.read_bytes().decode("latin-1")
        if destination.exists() else None,
        "progress": progress,
    }
    shutil.rmtree(folder, ignore_errors=True)
    return result


def _exception(kind, detail):
    if kind == "certificate":
        return urllib.error.URLError(ssl.SSLCertVerificationError(detail))
    if kind == "certificate_direct":
        return ssl.SSLCertVerificationError(detail)
    if kind == "http":
        return urllib.error.HTTPError("https://x.test/", detail[0], detail[1], None, None)
    if kind == "url":
        return urllib.error.URLError(detail)
    return OSError(detail) if detail else OSError()


def _describe(module, kind, detail, server, hosts, http_messages):
    kwargs = {"hosts": tuple(hosts), "http_messages": {code: text for code, text in http_messages}}
    if server is not None:
        kwargs["server"] = server
    return module.describe_network_error(_exception(kind, detail), **kwargs)


def _ok(headers=None, body=(0, 17), status=200, reason="OK"):
    return {"status": status, "reason": reason, "headers": headers or [],
            "body": body if isinstance(body, dict) else list(body)}


FULL = [["Content-Length", "17"]]
REST = [["Content-Range", "bytes 8-16/17"], ["Content-Length", "9"]]

CASES = {
    "resume": [
        # A fresh download.
        [None, [_ok(FULL)], {}],
        [None, [_ok()], {}],
        [None, [_ok([["Content-Length", "16"]])], {}],
        [None, [_ok([["Content-Length", "abc"]])], {}],
        [None, [_ok([["Content-Length", " 17 "]])], {}],
        [None, [_ok([["Content-Length", "1_7"]])], {}],
        [None, [_ok([["Content-Length", "+17"]])], {}],
        [None, [_ok([["Content-Length", "-17"]])], {}],
        [None, [_ok([["Content-Length", ""]])], {}],
        [None, [_ok(body={"junk": 18})], {}],
        [None, [_ok(body=[[0, 17], {"junk": 1}])], {}],
        [None, [_ok(body=(0, 16))], {}],
        [None, [_ok(FULL, body={"junk": 17})], {}],
        [None, [{"status": 206, "reason": "Partial Content", "headers": FULL, "body": [0, 17]}], {}],
        # Resuming from the eight bytes an earlier attempt left.
        [[0, 8], [_ok(REST, (8, 17), 206, "Partial Content")], {}],
        [[0, 8], [_ok([["Content-Range", "bytes 8-16/17"]], (8, 17), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes 0-9/17"]], (8, 17), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes 8-16/18"]], (8, 17), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes 8-17/17"]], (8, 17), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes  8-16/17"]], (8, 17), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes 8-16/17 "]], (8, 17), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes */17"]], (8, 17), 206)], {}],
        [[0, 8], [_ok([], (8, 17), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes 8-16/17"], ["Content-Length", "17"]],
                      (8, 17), 206)], {}],
        [[0, 8], [_ok(FULL)], {}],
        [[0, 8], [_ok()], {}],
        [[0, 8], [_ok(FULL, status=203, reason="Non-Authoritative Information")], {}],
        [[0, 8], [{"status": 416, "reason": "Range Not Satisfiable"}, _ok(FULL)], {}],
        [[0, 8], [{"status": 416, "reason": "Range Not Satisfiable"},
                  {"status": 416, "reason": "Range Not Satisfiable"}], {}],
        [[0, 8], [{"status": 416, "reason": "Range Not Satisfiable"}, _ok(REST, (8, 17), 206)],
         {}],
        [[0, 8], [{"status": 404, "reason": "Not Found"}], {}],
        [[0, 8], [{"status": 503, "reason": "Service Unavailable"}], {}],
        [[0, 8], [{"status": 304, "reason": "Not Modified"}], {}],
        [[0, 0], [{"status": 416, "reason": "Range Not Satisfiable"}], {}],
        [[0, 8], [_ok(REST, (8, 16), 206)], {}],
        [[0, 8], [_ok([["Content-Range", "bytes 8-16/17"]], [[8, 17], {"junk": 1}], 206)], {}],
        # What is already on disk decides whether the network is used at all.
        [[0, 17], [], {}],
        [{"junk": 17}, [_ok(FULL)], {}],
        [{"junk": 20}, [_ok(FULL)], {}],
        [{"junk": 8}, [_ok(REST, (8, 17), 206)], {}],
        [[0, 0], [_ok(FULL)], {}],
        # The network fails.
        [None, [{"url_error": "no route to host"}], {}],
        [[0, 8], [{"url_error": "no route to host"}], {}],
        [[0, 8], [{"certificate": True}], {}],
        [None, [{"status": 200, "reason": "OK", "headers": FULL, "body": [0, 8],
                 "fail": "timed out"}], {}],
        [[0, 8], [{"status": 206, "reason": "Partial Content", "headers": REST, "body": [8, 12],
                   "fail": "[WinError 10054] An existing connection was forcibly closed by "
                           "the remote host"}], {}],
        # Sizes.
        [None, [], {"size": 0}],
        [None, [], {"size": -1}],
        [None, [], {"max_bytes": 16}],
        [None, [_ok(FULL)], {"max_bytes": 17}],
        [[0, 8], [], {"size": 0}],
        # Cancel.
        [None, [_ok(FULL)], {"cancel_on_read": True}],
        [None, [_ok(FULL)], {"cancel_on_read": True, "keep_partial_on_cancel": True}],
        [[0, 8], [_ok(REST, (8, 17), 206)], {"cancel_on_read": True}],
        [[0, 8], [_ok(REST, (8, 17), 206)],
         {"cancel_on_read": True, "keep_partial_on_cancel": True}],
    ],
    "describe": [
        ["certificate", "self-signed", None, [], []],
        ["certificate", "self-signed", None, ["files.pythonhosted.org"], []],
        ["certificate", "self-signed", None, ["huggingface.co", "cdn-lfs.hf.co"], []],
        ["certificate", "self-signed", None, ["a.example", "b.example", "c.example"], []],
        ["certificate", "self-signed", "update server", [], []],
        ["certificate", "self-signed", "update server", ["github.com", "objects.githubusercontent.com"], []],
        ["certificate_direct", "self-signed", None, ["huggingface.co"], []],
        ["http", [404, "Not Found"], None, [], []],
        ["http", [500, ""], None, [], []],
        ["http", [503, "Service Unavailable"], "update server", [], []],
        ["http", [404, "Not Found"], "update server", [], [[404, "No release is published yet."]]],
        ["http", [403, "Forbidden"], None, [], [[404, "No release is published yet."]]],
        ["http", [403, "Forbidden"], None, [], [[403, ""]]],
        ["url", "no route to host", None, [], []],
        ["url", "[Errno 11001] getaddrinfo failed", None, ["huggingface.co"], []],
        ["url", "timed out", "update server", [], []],
        ["url", "", None, [], []],
        ["other", "timed out", None, [], []],
        ["other", "[WinError 112] There is not enough space on the disk", None, [], []],
        ["other", "", None, [], []],
        ["other", "", "update server", [], []],
    ],
}

WRAPPERS = {
    "resume": _resume,
    "describe": _describe,
}

PINS = {
    "resume_note": "download_verified(URL, sha256(BODY), size, <scratch>/payload.bin, ...) with "
                   "BODY = b'component-payload' (17 bytes), a scripted opener and "
                   "describe_error = describe_network_error(exc, hosts=('files.example.test',)); "
                   "byte contents are latin-1 strings",
    "describe_note": "certificate: URLError(SSLCertVerificationError(detail)); "
                     "certificate_direct: SSLCertVerificationError(detail); http: "
                     "HTTPError(code, reason); url: URLError(detail); other: OSError(detail)",
}
