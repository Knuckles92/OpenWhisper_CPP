"""OpenWhisper's reference numbers, measured on this machine (lead decision 5). NOT RUN in the
session that wrote it: lead decision 5 was not approved there, and baseline-openwhisper.json
holds only the numbers OpenWhisper's committed documents give. The owner runs it through
`python tools/perf/run_perf.py --openwhisper` once the decision is approved.

    FIORIVOCE_OW_APPROVED=1 FIORIVOCE_OW_ROOT=<checkout> FIORIVOCE_OW_PYTHON=<python 3.12> \\
        python -I -B tools/perf/ow_probes/run_probes.py [--out out/perf]

Each probe runs OpenWhisper's own entry point as a child of tools/parity/check_isolation.py
(which fails the probe if anything under the checkout or the owner's %LOCALAPPDATA%\\OpenWhisper
changed), with tools/parity/ow_isolation.py's scratch environment: OPENWHISPER_DATA_DIR,
LOCALAPPDATA, APPDATA and XDG_DATA_HOME under out/perf/ow-scratch, no proxy, HF_HUB_OFFLINE. The
working folder is the scratch folder, never the checkout. Nothing is installed and nothing but
127.0.0.1 is contacted (both scripts below only use loopback servers and fixture decoders).

- `main.py --workflow-smoke <report>` five times: stop_to_result_p95_ms, stop_to_result_p50_ms,
  cancel_ms, restart_ms, peak_rss_mb (medians of the five runs).
- `scripts/bench_dictation_overhead.py`: --only stop --seconds 30 (dictation_stop_overhead_ms
  and its parts), --only remote --seconds 10 (remote_round_trip_ms, remote_request_ms),
  --only api --seconds 10 (api_upload_ms, api_upload_cold_ms).

The agent-loop numbers (OpenWhisper's Pi sidecar against the same loopback provider, M8.8's
recorder setup) are not scripted here; their budgets stay absolute.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "parity"))
import ow_isolation  # noqa: E402

CHECK_ISOLATION = REPO / "tools" / "parity" / "check_isolation.py"
SMOKE_RUNS = 5
SMOKE = {
    "stop_to_result_p95_s": ("stop_to_result_p95_ms", 1000.0, "ms"),
    "stop_to_result_p50_s": ("stop_to_result_p50_ms", 1000.0, "ms"),
    "cancel_s": ("cancel_ms", 1000.0, "ms"),
    "restart_s": ("restart_ms", 1000.0, "ms"),
    "peak_rss_mb": ("peak_rss_mb", 1.0, "MB"),
}
# (part, label in bench_dictation_overhead.py's results) -> metric.
BENCH = {
    ("stop", "total"): "dictation_stop_overhead_ms",
    ("stop", "journal's last write"): "dictation_stop_journal_ms",
    ("stop", "WAV save"): "dictation_stop_wav_save_ms",
    ("stop", "conversion to 16 kHz windows"): "dictation_stop_convert_ms",
    ("remote", "transcribe, 10 s of audio"): "remote_round_trip_ms",
    ("remote", "request without audio"): "remote_request_ms",
    ("api", "upload, worker readied while recording"): "api_upload_ms",
    ("api", "upload, worker started for it"): "api_upload_cold_ms",
}
BENCH_RUNS = (("stop", "30", "25"), ("remote", "10", "50"), ("api", "10", "8"))


def isolated(command: list[str], env: dict[str, str], cwd: Path) -> subprocess.CompletedProcess:
    wrapped = [sys.executable, "-I", "-B", str(CHECK_ISOLATION), "--", *command]
    print("ow_probes:", " ".join(command))
    return subprocess.run(wrapped, env=env, cwd=cwd, capture_output=True, text=True,
                          check=False)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=REPO / "out" / "perf")
    args = parser.parse_args(argv)
    if os.environ.get("FIORIVOCE_OW_APPROVED") != "1":
        raise SystemExit("ow_probes: lead decision 5 is not approved (FIORIVOCE_OW_APPROVED=1).")
    python = os.environ.get(ow_isolation.PYTHON_ENV, "").strip()
    if not python:
        raise SystemExit(f"ow_probes: set {ow_isolation.PYTHON_ENV} to OpenWhisper's Python.")
    root = ow_isolation.checkout()
    commit = ow_isolation.check_reference_commit(root)

    scratch = (args.out / "ow-scratch").resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    for name in list(env):
        if name.lower() in ow_isolation.PROXY_VARIABLES:
            del env[name]
    env.setdefault("FIORIVOCE_REAL_LOCALAPPDATA", os.environ.get("LOCALAPPDATA", ""))
    env.update(ow_isolation.scratch_environment(scratch))

    metrics: dict[str, dict] = {}
    failures: list[str] = []

    smoke: dict[str, list[float]] = {}
    for run in range(SMOKE_RUNS):
        report = scratch / f"ow-smoke-{run}.json"
        result = isolated([python, "-I", "-B", str(root / "main.py"), "--workflow-smoke",
                           str(report)], env, scratch)
        if result.returncode != 0 or not report.is_file():
            failures.append(f"workflow smoke {run + 1}: exit {result.returncode}: "
                            f"{(result.stderr or result.stdout).strip()[-400:]}")
            continue
        values = json.loads(report.read_text(encoding="utf-8")).get("metrics", {})
        for name, (metric, scale, _) in SMOKE.items():
            if isinstance(values.get(name), (int, float)):
                smoke.setdefault(metric, []).append(float(values[name]) * scale)
    for name, (metric, _, unit) in SMOKE.items():
        if smoke.get(metric):
            metrics[metric] = {"value": statistics.median(smoke[metric]), "unit": unit,
                               "statistic": "p50", "samples": smoke[metric],
                               "source": f"measured: main.py --workflow-smoke, median of "
                                         f"{len(smoke[metric])} runs"}

    for part, seconds, runs in BENCH_RUNS:
        output = scratch / f"ow-bench-{part}.json"
        result = isolated([python, "-I", "-B", str(root / "scripts" / "bench_dictation_overhead.py"),
                           "--only", part, "--seconds", seconds, "--runs", runs, "--output",
                           str(output)], env, scratch)
        if result.returncode != 0 or not output.is_file():
            failures.append(f"bench {part}: exit {result.returncode}: "
                            f"{(result.stderr or result.stdout).strip()[-400:]}")
            continue
        medians = json.loads(output.read_text(encoding="utf-8")).get("medians_ms", {})
        for (bench_part, label), metric in BENCH.items():
            value = medians.get(bench_part, {}).get(label)
            if bench_part == part and isinstance(value, (int, float)):
                metrics[metric] = {"value": float(value), "unit": "ms", "statistic": "p50",
                                   "source": f"measured: scripts/bench_dictation_overhead.py "
                                             f"--only {part} --seconds {seconds}: {label}"}

    document = {"schema": 1, "openwhisper_commit": commit,
                "machine": {"platform": platform.system().lower(),
                            "os": f"{platform.system()} {platform.release()}",
                            "logical_cores": os.cpu_count()},
                "metrics": metrics, "failures": failures}
    target = args.out / "openwhisper-measured.json"
    target.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"ow_probes: {len(metrics)} metric(s) -> {target}")
    for failure in failures:
        print(f"ow_probes: FAILED {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
