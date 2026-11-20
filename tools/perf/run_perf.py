"""Measure OpenWhisper_CPP's performance metrics and write the report.

    python tools/perf/run_perf.py [--preset NAME] [--metric NAME ...] [--cold-runs N]
                                  [--openwhisper] [--skip-build] [--ignore-load]
                                  [--report-to FILE]

1. Refuses to start while the machine is busy (a measurement made while builds run is noise):
   the one-minute load average per core above 30 % (Linux, macOS), or the processor time over
   3 s above 30 % (Windows). --ignore-load overrides it, for a smoke run only.
2. Builds fv_perf_tests and openwhisper-cpp-cli with the build preset (default: the platform's
   release preset, windows-release or linux-release).
3. Runs the measuring tests, one at a time, through the platform's perf test preset
   (`ctest --preset windows-perf -C <configuration> -R '^perf\\.' -L perf -j1`), each
   writing out/perf/<metric>.json. The tests that measure a cold start (first construction,
   startup) run --cold-runs times in all (default 5), each in a fresh process, and their samples
   are merged.
4. Runs `openwhisper-cpp-cli --workflow-smoke` 5 times for the stop_to_result, cancel, restart and peak
   RSS metrics.
5. Reads M10.4's benchmark reports out/perf/bench-*.json when present.
6. With --openwhisper (lead decision 5 only: OPENWHISPER_CPP_OW_APPROVED=1 and OPENWHISPER_CPP_OW_ROOT
   set), runs OpenWhisper's probes under tools/parity/ow_isolation.py and check_isolation.py and
   writes out/perf/openwhisper-measured.json.
7. Writes out/perf/performance-report.md (perf_report.py), or --report-to.

It never judges a budget: tools/perf/check_budgets.py does. Exit status 1 when a measuring test
or the workflow smoke failed.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_lib  # noqa: E402
import perf_report  # noqa: E402

REPO = perf_lib.REPO
LOAD_LIMIT_PERCENT = 30.0
SMOKE_RUNS = 5
WORKFLOW_SMOKE = "openwhisper-cpp-cli --workflow-smoke (5 runs)"
# --workflow-smoke's metrics (seconds) and the metric each one is recorded as (milliseconds).
SMOKE_METRICS = {
    "stop_to_result_p95_s": ("stop_to_result_p95_ms", 1000.0, "ms"),
    "stop_to_result_p50_s": ("stop_to_result_p50_ms", 1000.0, "ms"),
    "cancel_s": ("cancel_ms", 1000.0, "ms"),
    "restart_s": ("restart_ms", 1000.0, "ms"),
    "peak_rss_mb": ("peak_rss_mb", 1.0, "MB"),
}
CMAKE_REGEX_SPECIAL = set(".[]()*+?^$|\\")


def default_preset() -> str | None:
    system = platform.system()
    if system == "Windows":
        return "windows-release"
    if system == "Linux":
        return "linux-release"
    return None


def cmake_regex_escape(text: str) -> str:
    return "".join("\\" + char if char in CMAKE_REGEX_SPECIAL else char for char in text)


# --- load ------------------------------------------------------------------------------------

def cpu_busy_percent() -> float | None:
    """How busy the processors are, in percent of all of them."""
    if platform.system() == "Windows":
        try:
            result = subprocess.run(
                ["typeperf", r"\Processor(_Total)\% Processor Time", "-si", "1", "-sc", "3"],
                capture_output=True, text=True, timeout=30, check=False)
        except (OSError, subprocess.SubprocessError):
            return None
        values = []
        for line in result.stdout.splitlines():
            parts = [part.strip().strip('"') for part in line.split(",")]
            if len(parts) == 2:
                try:
                    values.append(float(parts[1]))
                except ValueError:
                    continue
        return sum(values) / len(values) if values else None
    if hasattr(os, "getloadavg"):
        cores = os.cpu_count() or 1
        return os.getloadavg()[0] / cores * 100.0
    return None


def check_load(ignore: bool) -> None:
    busy = cpu_busy_percent()
    if busy is None:
        print("run_perf: could not read the processor load; measuring anyway")
        return
    if busy > LOAD_LIMIT_PERCENT:
        message = (f"run_perf: the processors are {busy:.0f} % busy (over "
                   f"{LOAD_LIMIT_PERCENT:.0f} %): numbers measured now would be noise. Wait for "
                   "other builds to finish")
        if not ignore:
            raise SystemExit(message + ", or pass --ignore-load for a smoke run.")
        print(message + "; measuring anyway (--ignore-load).")
    else:
        print(f"run_perf: processors {busy:.0f} % busy")


# --- presets ---------------------------------------------------------------------------------

def preset_paths(preset: str) -> tuple[Path, str]:
    """The build folder and configuration a build preset uses (CMakePresets.json)."""
    presets = perf_lib.read_json(REPO / "CMakePresets.json")
    build = next((p for p in presets.get("buildPresets", []) if p["name"] == preset), None)
    if build is None:
        raise SystemExit(f"run_perf: no build preset {preset!r} in CMakePresets.json.")
    configure_name = build["configurePreset"]
    configures = {p["name"]: p for p in presets.get("configurePresets", [])}
    binary_dir = None
    name = configure_name
    while name and binary_dir is None:
        entry = configures[name]
        binary_dir = entry.get("binaryDir")
        inherits = entry.get("inherits")
        name = inherits[0] if isinstance(inherits, list) else inherits
    if binary_dir is None:
        raise SystemExit(f"run_perf: configure preset {configure_name!r} has no binaryDir.")
    binary_dir = (binary_dir.replace("${sourceDir}", str(REPO))
                  .replace("${presetName}", configure_name))
    return Path(binary_dir), build.get("configuration", "")


def perf_test_preset(preset: str) -> tuple[str, list[str]]:
    """The test preset that runs the measuring tests of a build preset, and its extra arguments.

    The default test presets exclude the perf label (lead decision 3), so the measuring tests
    run through <configure preset>-perf, told which configuration the build preset built.
    A tree whose presets have no such test preset falls back to the build preset's own name.
    """
    presets = perf_lib.read_json(REPO / "CMakePresets.json")
    build = next((p for p in presets.get("buildPresets", []) if p["name"] == preset), None)
    if build is not None:
        name = f"{build['configurePreset']}-perf"
        if any(test["name"] == name for test in presets.get("testPresets", [])):
            configuration = build.get("configuration", "")
            return name, (["-C", configuration] if configuration else [])
    return preset, []


def find_cli(binary_dir: Path, configuration: str) -> Path | None:
    names = ("openwhisper-cpp-cli.exe", "openwhisper-cpp-cli")
    for folder in (binary_dir / "bin" / configuration, binary_dir / "bin", binary_dir):
        for name in names:
            if (folder / name).is_file():
                return folder / name
    for name in names:
        for found in binary_dir.rglob(name):
            if found.is_file():
                return found
    return None


# --- git -------------------------------------------------------------------------------------

def head_commit() -> str:
    try:
        commit = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True,
                                text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain",
                                "--untracked-files=no"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return commit + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


# --- merging ---------------------------------------------------------------------------------

def merge_runs(run_dirs: list[Path], out: Path) -> list[str]:
    """Merges each metric's samples from every pass into out/<metric>.json; returns the
    metrics written."""
    by_metric: dict[str, list[dict]] = {}
    for run_dir in run_dirs:
        for record in perf_lib.load_measured(run_dir).values():
            by_metric.setdefault(record["metric"], []).append(record)
    written = []
    for metric, records in sorted(by_metric.items()):
        merged = dict(records[-1])
        samples = [sample for record in records for sample in record.get("samples", [])]
        merged.update(perf_lib.summarize(samples))
        merged["samples"] = samples
        merged["runs"] = len(records)
        write_metric(out, merged)
        written.append(metric)
    return written


def write_metric(out: Path, record: dict) -> None:
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"{record['metric']}.json"
    partial = target.with_suffix(".json.partial")
    partial.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    partial.replace(target)


def machine() -> dict:
    """What a number measured outside the C++ tests was measured on (no host or user name)."""
    system = platform.system()
    made = {"platform": {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}.get(
                system, system.lower()),
            "os": f"{system} {platform.release()}", "logical_cores": os.cpu_count()}
    cpu = platform.processor()
    if system == "Linux":
        try:
            for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
        except OSError:
            pass
    elif system == "Windows":
        cpu = os.environ.get("PROCESSOR_IDENTIFIER", cpu)
    if cpu:
        made["cpu"] = cpu
    if os.environ.get("OPENWHISPER_CPP_PERF_MACHINE"):
        made["label"] = os.environ["OPENWHISPER_CPP_PERF_MACHINE"]
    return made


def record_from_samples(metric: str, unit: str, samples: list[float], commit: str, build: str,
                        test: str, notes: dict | None = None) -> dict:
    return {"schema": 1, "metric": metric, "unit": unit, "samples": samples,
            **perf_lib.summarize(samples), "commit": commit, "machine": machine(),
            "build": build, "test": test, "notes": notes or {}}


# --- steps -----------------------------------------------------------------------------------

def build(preset: str) -> None:
    command = ["cmake", "--build", "--preset", preset, "--target", "fv_perf_tests",
               "openwhisper-cpp-cli"]
    print("run_perf:", " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise SystemExit("run_perf: the build failed.")


def run_ctest(preset: str, regex: str, out_dir: Path, commit: str) -> bool:
    env = dict(os.environ, OPENWHISPER_CPP_PERF_OUT=str(out_dir), OPENWHISPER_CPP_PERF_COMMIT=commit)
    test_preset, configuration = perf_test_preset(preset)
    command = ["ctest", "--preset", test_preset, *configuration, "-R", regex, "-L", "perf",
               "-j1", "--output-on-failure"]
    print("run_perf: ctest --preset", test_preset, *configuration, "-R", repr(regex),
          "-L perf -j1 --output-on-failure")
    return subprocess.run(command, env=env, check=False).returncode == 0


def tests_regex(tests: list[str]) -> str:
    if not tests:
        return r"^perf\."
    return "^(" + "|".join(cmake_regex_escape(test) for test in sorted(set(tests))) + ")$"


def run_workflow_smoke(cli: Path, out: Path, commit: str, build_name: str) -> bool:
    samples: dict[str, list[float]] = {}
    ok = True
    with tempfile.TemporaryDirectory(prefix="fv-perf-smoke-") as scratch:
        env = dict(os.environ, OPENWHISPER_CPP_DATA_DIR=str(Path(scratch) / "data"))
        for run in range(SMOKE_RUNS):
            report = Path(scratch) / f"smoke-{run}.json"
            result = subprocess.run([str(cli), "--workflow-smoke", str(report)], env=env,
                                    capture_output=True, text=True, check=False)
            if result.returncode != 0 or not report.is_file():
                print(f"run_perf: workflow smoke run {run + 1} failed: "
                      f"{result.stderr.strip()[:400]}")
                ok = False
                continue
            metrics = perf_lib.read_json(report).get("metrics", {})
            for name, (metric, scale, _) in SMOKE_METRICS.items():
                value = metrics.get(name)
                if isinstance(value, (int, float)) and not math.isnan(value):
                    samples.setdefault(metric, []).append(float(value) * scale)
    for name, (metric, _, unit) in SMOKE_METRICS.items():
        if samples.get(metric):
            write_metric(out, record_from_samples(
                metric, unit, samples[metric], commit, build_name, WORKFLOW_SMOKE,
                {"runs": SMOKE_RUNS, "fixture": "synthetic, fixture decoder"}))
    return ok


def ingest_bench_reports(out: Path, commit: str) -> list[str]:
    """M10.4's `openwhisper-cpp-bench ... --output out/perf/bench-<kind>.json` reports: each numeric
    metric becomes <kind>_<name> (one sample)."""
    written = []
    for path in sorted(out.glob("bench-*.json")):
        try:
            report = perf_lib.read_json(path)
        except (OSError, ValueError):
            continue
        kind = str(report.get("kind") or path.stem[len("bench-"):]).replace("-", "_")
        provenance = report.get("provenance") or {}
        for name, value in (report.get("metrics") or {}).items():
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                continue
            metric = f"{kind}_{name}"
            record = record_from_samples(metric, "", [float(value)],
                                         str(provenance.get("commit") or commit),
                                         "bench", f"openwhisper-cpp-bench ({path.name})",
                                         {"provenance": provenance})
            if isinstance(provenance.get("machine"), dict):
                record["machine"] = provenance["machine"]
            write_metric(out, record)
            written.append(metric)
    return written


def run_openwhisper(out: Path) -> bool:
    if os.environ.get("OPENWHISPER_CPP_OW_APPROVED") != "1":
        raise SystemExit(
            "run_perf: --openwhisper runs OpenWhisper's code, which needs lead decision 5. Set "
            "OPENWHISPER_CPP_OW_APPROVED=1 once it is approved, with OPENWHISPER_CPP_OW_ROOT (the read-only "
            "checkout) and OPENWHISPER_CPP_OW_PYTHON (its Python 3.12).")
    probes = Path(__file__).resolve().parent / "ow_probes"
    command = [sys.executable, "-I", "-B", str(probes / "run_probes.py"), "--out", str(out)]
    print("run_perf:", " ".join(command))
    return subprocess.run(command, check=False).returncode == 0


# --- main ------------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--preset", default=default_preset(),
                        help="build and test preset (default: the platform's release preset)")
    parser.add_argument("--metric", action="append", default=[],
                        help="measure only this metric (repeatable)")
    parser.add_argument("--cold-runs", type=int, default=5,
                        help="processes for each cold-start test (default 5)")
    parser.add_argument("--openwhisper", action="store_true",
                        help="also measure OpenWhisper (lead decision 5)")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--ignore-load", action="store_true")
    parser.add_argument("--out", type=Path, default=perf_lib.DEFAULT_OUT)
    parser.add_argument("--report-to", type=Path,
                        help="the report (default <out>/performance-report.md)")
    args = parser.parse_args(argv)
    if not args.preset:
        raise SystemExit("run_perf: no release preset for this platform; pass --preset.")

    budgets = perf_lib.load_budgets()
    by_metric = {budget["metric"]: budget for budget in budgets}
    unknown = [metric for metric in args.metric if metric not in by_metric]
    if unknown:
        raise SystemExit(f"run_perf: unknown metric(s): {', '.join(unknown)}")
    selected = [by_metric[m] for m in args.metric] if args.metric else budgets
    ctest_tests = sorted({b["test"] for b in selected if b["test"].startswith("perf.")})
    cold_tests = sorted({b["test"] for b in selected
                         if b.get("cold") and b["test"].startswith("perf.")})
    wants_smoke = any(b["test"] == WORKFLOW_SMOKE for b in selected)

    check_load(args.ignore_load)
    binary_dir, configuration = preset_paths(args.preset)
    if not args.skip_build:
        build(args.preset)

    commit = head_commit()
    out = args.out
    runs_root = out / "runs"
    shutil.rmtree(runs_root, ignore_errors=True)
    if not args.metric:
        # A full run replaces every number, so none from an earlier commit is left in the report.
        for stale in perf_lib.load_measured(out):
            (out / f"{stale}.json").unlink(missing_ok=True)
    ok = True
    run_dirs: list[Path] = []
    if ctest_tests:
        first = runs_root / "1"
        regex = tests_regex([] if not args.metric else ctest_tests)
        ok &= run_ctest(args.preset, regex, first, commit)
        run_dirs.append(first)
        for index in range(2, max(args.cold_runs, 1) + 1):
            if not cold_tests:
                break
            folder = runs_root / str(index)
            ok &= run_ctest(args.preset, tests_regex(cold_tests), folder, commit)
            run_dirs.append(folder)
            time.sleep(1)
    written = merge_runs(run_dirs, out)
    print(f"run_perf: {len(written)} metric(s) from the measuring tests")

    if wants_smoke:
        cli = find_cli(binary_dir, configuration)
        if cli is None:
            print(f"run_perf: no openwhisper-cpp-cli under {binary_dir}; the workflow smoke is skipped")
            ok = False
        else:
            ok &= run_workflow_smoke(cli, out, commit, configuration)
    bench = ingest_bench_reports(out, commit)
    if bench:
        print(f"run_perf: {len(bench)} metric(s) from benchmark reports")
    if args.openwhisper:
        ok &= run_openwhisper(out)

    output = args.report_to or out / "performance-report.md"
    print(f"run_perf: wrote {perf_report.write_report(out, output)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
