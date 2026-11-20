"""Exercise both remote speech directions against pinned OpenWhisper on 127.0.0.1."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


CPP_CLIENT = "remote[.]live OpenWhisper host pairs and serves synthetic audio"
CPP_HOST = "remote_host[.]live OpenWhisper client pairs and sends synthetic audio"


def ctest(args, test):
    return ["ctest", "--test-dir", str(args.build.resolve()), "-C", args.config,
            "-R", f"^{test}$", "--no-tests=error", "--output-on-failure"]


def python_peer(role, data):
    script = Path(__file__).with_name("remote_peer.py")
    return [sys.executable, "-I", "-B", str(script), "--role", role, "--data", str(data)]


def stop(process):
    if process.poll() is not None:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def cpp_client(args, root):
    with subprocess.Popen(
        [*python_peer("host", root / "python-host"), "--port", "0"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8",
    ) as host:
        try:
            lines = queue.Queue()
            threading.Thread(target=lambda: lines.put(host.stdout.readline()), daemon=True).start()
            line = lines.get(timeout=20)
            if not line:
                raise RuntimeError("The isolated OpenWhisper host exited during startup.")
            endpoint = json.loads(line)
            env = {**os.environ, "OPENWHISPER_CPP_LOOPBACK_INTEROP": json.dumps(endpoint)}
            subprocess.run(ctest(args, CPP_CLIENT), env=env, timeout=45, check=True)
        finally:
            host.stdin.close()
            stop(host)


def cpp_host(args, root):
    env = {**os.environ, "OPENWHISPER_CPP_LOOPBACK_INTEROP_DIR": str(root)}
    with subprocess.Popen(ctest(args, CPP_HOST), env=env) as host:
        try:
            endpoint_file = root / "endpoint.json"
            deadline = time.monotonic() + 15
            while not endpoint_file.exists():
                if host.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("The isolated native host did not publish its endpoint.")
                time.sleep(0.025)
            endpoint = json.loads(endpoint_file.read_text(encoding="utf-8"))
            commands = [
                {"op": "connect"},
                {"op": "request", "name": "transcribe", "seconds": 0.1},
                {"op": "close"},
            ]
            client = subprocess.run(
                [*python_peer("client", root / "python-client"), "--port", str(endpoint["port"]),
                 "--code", endpoint["code"]],
                input="".join(json.dumps(command) + "\n" for command in commands),
                capture_output=True, text=True, encoding="utf-8", timeout=20, check=True,
            )
            paired, ready, transcription, closed = map(json.loads, client.stdout.splitlines())
            if (paired.get("event") != "paired" or ready.get("ready", {}).get("type") != "ready"
                    or transcription.get("result", {}).get("text") != "heard 1600"
                    or closed.get("closed") is not True):
                raise RuntimeError("OpenWhisper did not pair, decode, and close on the native host.")
            (root / "confirmed").touch()
            if host.wait(timeout=8) != 0:
                raise RuntimeError("The native host detected an interoperability failure.")
        finally:
            stop(host)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True, help="configured CTest build tree")
    parser.add_argument("--config", default="RelWithDebInfo")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="fv-remote-interop-") as scratch:
        root = Path(scratch)
        cpp_client(args, root)
        cpp_host(args, root)


if __name__ == "__main__":
    main()
