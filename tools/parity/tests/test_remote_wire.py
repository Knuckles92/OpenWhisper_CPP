"""Self-tests of remote_wire.py's tap and normalizer on synthetic exchanges, and of the committed
remote_* transcripts' shape (no OpenWhisper needed; CTest support.parity_tools)."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import remote_wire  # noqa: E402

TRANSCRIPTS = TOOLS.parents[1] / "tests" / "fixtures" / "transcripts"
PLACEHOLDER = re.compile(r'"<([a-z_]+)#([0-9]+)>"')


def request(header: dict, payload: bytes = b"") -> bytes:
    body = json.dumps(header, separators=(",", ":")).encode()
    return len(body).to_bytes(4, "big") + body + payload


class TapTests(unittest.TestCase):
    def test_frames_are_grouped_by_connection_in_send_order(self):
        tap = remote_wire.Tap()
        tap.step = "one"
        first_client = tap.register(5000, "client")
        first_server = tap.register(5000, "server")
        self.assertIs(first_client, first_server)
        second = tap.register(5001, "client")
        tap.frame(first_client, "client", "text", b'{"type": "probe"}')
        tap.frame(second, "client", "text", b"hello")
        tap.step = "two"
        tap.frame(first_server, "server", "close", b"", close=(1000, ""))
        tap.frame(first_client, "client", "close", b"", echo=True, close=(1000, ""))
        records = tap.records()
        self.assertEqual([(r["conn"], r["dir"], r["kind"]) for r in records], [
            (0, "c2s", "text"), (0, "s2c", "close"), (0, "c2s", "close"), (1, "c2s", "text")])
        self.assertEqual([r["step"] for r in records], ["one", "two", "two", "one"])
        self.assertTrue(records[2]["echo"])
        self.assertNotIn("echo", records[1])

    def test_a_port_used_again_is_a_new_connection(self):
        tap = remote_wire.Tap()
        a = tap.register(6000, "client")
        tap.register(6000, "server")
        b = tap.register(6000, "client")
        self.assertIsNot(a, b)
        self.assertEqual(tap.connection_count(), 2)

    def test_http_is_kept_only_for_a_refused_upgrade(self):
        tap = remote_wire.Tap()
        ok = tap.register(7000, "client")
        tap.http_request(ok, "/openwhisper/v1/engine")
        tap.http_response(ok, 101, b"")
        refused = tap.register(7001, "client")
        tap.http_request(refused, "/x")
        tap.http_response(refused, 404, b"Not an OpenWhisper remote engine endpoint\n")
        records = tap.records()
        self.assertEqual(records, [
            {"step": "", "conn": 1, "dir": "c2s", "kind": "http", "text": "/x"},
            {"step": "", "conn": 1, "dir": "s2c", "kind": "http", "status": 404,
             "body": "Not an OpenWhisper remote engine endpoint\n"}])

    def test_binary_frames_split_into_header_and_payload(self):
        self.assertEqual(remote_wire.split_binary(request({"id": 1}, b"\x01\x02")),
                         {"header": '{"id":1}', "payload_hex": "0102"})
        self.assertEqual(remote_wire.split_binary(b"\x00\x01"), {"payload_hex": "0001"})
        self.assertEqual(remote_wire.split_binary(b"\x00\x00\x00\x09{}"),
                         {"payload_hex": "000000097b7d"})
        self.assertEqual(remote_wire.split_binary(b"\x00\x00\x00\x01\xff"),
                         {"payload_hex": "00000001ff"})
        big = b"\x00" * (remote_wire.PAYLOAD_HEX_LIMIT + 2)
        split = remote_wire.split_binary(request({"id": 2}, big))
        self.assertEqual(split["payload_len"], len(big))
        self.assertEqual(split["payload_sha256"], hashlib.sha256(big).hexdigest())
        self.assertEqual(split["payload_fill"], "00")
        self.assertNotIn("payload_hex", split)
        mixed = big + b"\x01"
        self.assertNotIn("payload_fill", remote_wire.split_binary(request({"id": 3}, mixed)))


TOKEN = "A" * 43
NONCE = "ab" * 32
FINGERPRINT = "0F" * 32


def text(direction: str, value: dict, compact: bool) -> dict:
    style = {"separators": (",", ":")} if compact else {}
    return {"step": "s", "conn": 0, "dir": direction, "kind": "text",
            "text": json.dumps(value, **style)}


class NormalizerTests(unittest.TestCase):
    RULES = [
        {"pointer": "/token", "as": "token", "when": {"type": "paired"}},
        {"pointer": "/token", "as": "token", "when": {"type": "hello"}, "keep": ["unpaired"]},
        {"pointer": "/host/fingerprint", "as": "fingerprint"},
        {"pointer": "/commit", "as": "commit", "when": {"type": "pair_commit"}},
        {"pointer": "/nonce", "as": "nonce", "when": {"type": "pair_reveal"}},
        {"pointer": "/host_ms", "as": "host_ms", "dir": "s2c"},
    ]

    def run_normalizer(self, frames):
        normalizer = remote_wire.Normalizer(self.RULES)
        return normalizer, *normalizer.frames(frames)

    def test_one_value_is_one_placeholder_across_frames_and_styles(self):
        frames = [
            text("s2c", {"type": "paired", "token": TOKEN,
                         "host": {"fingerprint": FINGERPRINT}}, compact=True),
            text("c2s", {"type": "hello", "protocol": 1, "token": TOKEN}, compact=False),
            text("c2s", {"type": "hello", "protocol": 1, "token": "unpaired"}, compact=False),
        ]
        normalizer, out, entries = self.run_normalizer(frames)
        self.assertEqual(out[0]["text"], '{"type":"paired","token":"<token#1>",'
                                         '"host":{"fingerprint":"<fingerprint#1>"}}')
        self.assertEqual(out[1]["text"], '{"type": "hello", "protocol": 1, "token": "<token#1>"}')
        self.assertEqual(out[2]["text"], frames[2]["text"])
        self.assertEqual(entries, [{"path": "0/token", "as": "token"},
                                   {"path": "0/host/fingerprint", "as": "fingerprint"},
                                   {"path": "1/token", "as": "token"}])
        self.assertEqual(normalizer.value({"t": [TOKEN], "f": FINGERPRINT}),
                         {"t": ["<token#1>"], "f": "<fingerprint#1>"})

    def test_host_ms_gets_a_placeholder_per_occurrence(self):
        frames = [text("s2c", {"id": 1, "result": {}, "host_ms": 0.4}, compact=True),
                  text("s2c", {"id": 2, "result": {}, "host_ms": 0.4}, compact=True),
                  text("c2s", {"id": 3, "host_ms": 0.4}, compact=True)]
        _, out, entries = self.run_normalizer(frames)
        self.assertEqual(out[0]["text"], '{"id":1,"result":{},"host_ms":"<host_ms#1>"}')
        self.assertEqual(out[1]["text"], '{"id":2,"result":{},"host_ms":"<host_ms#2>"}')
        self.assertEqual(out[2]["text"], frames[2]["text"])
        self.assertEqual(len(entries), 2)

    def test_a_commit_names_the_nonce_it_commits_to(self):
        digest = hashlib.sha256(remote_wire.COMMIT_DOMAIN + bytes.fromhex(NONCE)).hexdigest()
        frames = [text("s2c", {"type": "pair_commit", "commit": digest}, compact=True),
                  text("s2c", {"type": "pair_reveal", "nonce": NONCE}, compact=True),
                  text("s2c", {"type": "pair_commit", "commit": "cd" * 32}, compact=True)]
        _, _, entries = self.run_normalizer(frames)
        self.assertEqual(entries[0], {"path": "0/commit", "as": "commit", "of": "<nonce#1>"})
        self.assertEqual(entries[2], {"path": "2/commit", "as": "commit"})

    def test_product_names_are_listed_and_left_as_they_are(self):
        frames = [text("s2c", {"id": 1, "error": "Update OpenWhisper on the host."}, True)]
        _, out, entries = self.run_normalizer(frames)
        self.assertEqual(out, frames)
        self.assertEqual(entries, [{"path": "0/error", "as": "product"}])

    def test_binary_headers_are_normalized_too(self):
        frame = {"step": "s", "conn": 0, "dir": "s2c", "kind": "binary",
                 "header": json.dumps({"type": "paired", "token": TOKEN},
                                      separators=(",", ":")), "payload_hex": ""}
        _, out, _ = self.run_normalizer([frame])
        self.assertEqual(out[0]["header"], '{"type":"paired","token":"<token#1>"}')

    def test_a_value_of_the_wrong_shape_is_refused(self):
        frames = [text("s2c", {"type": "paired", "token": "short"}, compact=True)]
        with self.assertRaises(remote_wire.RecordingError):
            self.run_normalizer(frames)

    def test_text_json_dumps_cannot_write_again_is_refused(self):
        frame = {"step": "s", "conn": 0, "dir": "s2c", "kind": "text",
                 "text": '{"type":"paired",  "token":"' + TOKEN + '"}'}
        with self.assertRaises(remote_wire.RecordingError):
            self.run_normalizer([frame])

    def test_unknown_kinds_are_refused(self):
        with self.assertRaises(remote_wire.RecordingError):
            remote_wire.Normalizer([{"pointer": "/x", "as": "secret"}])

    def test_the_hosts_ports_in_outcomes_are_replaced_and_other_ports_kept(self):
        normalizer = remote_wire.Normalizer([])
        self.assertEqual(normalizer.value("127.0.0.1:50685 answered (HTTP 404).", {50685}),
                         "127.0.0.1:<port> answered (HTTP 404).")
        self.assertEqual(normalizer.value({"url": ["http://127.0.0.1:8765/mcp",
                                                   "127.0.0.1:506851"]}, {50685}),
                         {"url": ["http://127.0.0.1:8765/mcp", "127.0.0.1:506851"]})


class PeerIsolation(unittest.TestCase):
    def test_a_data_folder_inside_the_checkout_is_refused_before_anything_is_written(self):
        import os
        import tempfile
        from unittest import mock

        import ow_isolation
        import remote_peer

        with tempfile.TemporaryDirectory() as tmp:
            checkout = Path(tmp) / "checkout"
            checkout.mkdir()
            (checkout / "config.py").write_text("", encoding="utf-8")
            data = checkout / "peer"
            with mock.patch.dict(os.environ, {ow_isolation.ROOT_ENV: str(checkout)}), \
                    mock.patch.object(ow_isolation, "check_reference_commit"):
                with self.assertRaises(ow_isolation.IsolationError):
                    remote_peer.isolate(data, allow_other_commit=False)
            self.assertFalse(data.exists())


def load_scenario(path: Path):
    spec = importlib.util.spec_from_file_location(f"scenario_{path.stem}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CommittedTranscripts(unittest.TestCase):
    """Each remote_* scenario file has its transcript, and the transcript is self-consistent."""

    def scenarios(self):
        return sorted((TOOLS / "transcripts").glob("remote_*.py"))

    def test_every_scenario_is_recorded_with_its_steps(self):
        names = [p.stem for p in self.scenarios()]
        self.assertEqual(len(names), 9)
        for path in self.scenarios():
            module = load_scenario(path)
            self.assertTrue(callable(module.build), path.name)
            steps = [name for name, fn in module.STEPS]
            self.assertEqual(len(steps), len(set(steps)), path.name)
            remote_wire.Normalizer(list(module.NORMALIZE))
            document = json.loads((TRANSCRIPTS / f"{path.stem}.json").read_text("utf-8"))
            self.assertEqual(document["scenario"], path.stem)
            self.assertEqual([s["name"] for s in document["setup"]["steps"]], steps)

    def test_placeholders_are_listed_where_they_stand(self):
        for path in self.scenarios():
            document = json.loads((TRANSCRIPTS / f"{path.stem}.json").read_text("utf-8"))
            listed = {}
            for entry in document["normalize"]:
                index, pointer = remote_wire._split_path(entry["path"])
                frame = document["frames"][index]
                value = remote_wire.pointer_get(
                    json.loads(frame["text"] if frame["kind"] == "text" else frame["header"]),
                    pointer)
                if entry["as"] == "product":
                    self.assertIn(remote_wire.PRODUCT, value, entry)
                    continue
                self.assertRegex(value, rf"^<{entry['as']}#[0-9]+>$", entry)
                listed.setdefault(index, []).append(value)
            for index, frame in enumerate(document["frames"]):
                found = [f"<{kind}#{n}>" for kind, n in
                         PLACEHOLDER.findall(frame.get("text", "") + frame.get("header", ""))]
                self.assertEqual(sorted(found), sorted(listed.get(index, [])),
                                 f"{path.stem} frame {index}")

    def test_connections_are_whole_and_in_order(self):
        for path in self.scenarios():
            document = json.loads((TRANSCRIPTS / f"{path.stem}.json").read_text("utf-8"))
            seen = []
            for frame in document["frames"]:
                self.assertIn(frame["dir"], ("c2s", "s2c"))
                self.assertIn(frame["kind"], ("text", "binary", "close", "http"))
                if not seen or seen[-1] != frame["conn"]:
                    self.assertNotIn(frame["conn"], seen, path.stem)
                    seen.append(frame["conn"])
            self.assertEqual(seen, sorted(seen), path.stem)


if __name__ == "__main__":
    unittest.main()
