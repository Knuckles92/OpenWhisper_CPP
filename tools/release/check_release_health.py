"""Check the synthetic lifecycle report of ``openwhisper-cpp-cli --workflow-smoke`` against the release
gate: every data-safety check passed, every metric present, finite and within its deadline.

Give the report to check, or ``--executable`` to run the smoke into ``--output`` first under a
process exit deadline. ``--baseline`` rejects timing or RSS regressions against a previously
accepted report from equivalent hardware; synthetic results are not ASR benchmarks::

    python tools/release/check_release_health.py out/smoke.json
    python tools/release/check_release_health.py --executable <stage>/openwhisper-cpp-cli.exe \\
        --output out/release-health.json [--baseline previous-release-health.json]
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

LIMITS = {"fixture_ready_s": 15, "stop_to_result_p95_s": 2, "cancel_s": 2,
          "restart_s": 2, "cleanup_s": 2, "peak_rss_mb": 1500}
REQUIRED_CHECKS = ("recording_recovery", "backup_restore", "previous_data_preserved")


def failures(report, baseline=None) -> list[str]:
    """Why the report fails the gate; empty when it passes."""
    if (not isinstance(report, dict) or report.get("kind") != "synthetic_service_lifecycle"
            or report.get("passed") is not True):
        return ["Missing successful synthetic lifecycle result"]
    metrics = report.get("metrics")
    if not isinstance(metrics, dict):
        return ["Missing lifecycle metrics"]
    failed = []
    checks = report.get("checks", {})
    for name in REQUIRED_CHECKS:
        if not isinstance(checks, dict) or checks.get(name) is not True:
            failed.append(f"{name}: missing successful data-safety check")
    for key, limit in LIMITS.items():
        value = metrics.get(key)
        if (isinstance(value, bool) or not isinstance(value, (int, float))
                or not math.isfinite(value) or value < 0):
            failed.append(f"{key}: missing or invalid measurement")
        elif value > limit:
            failed.append(f"{key}: {value:.3f} exceeds {limit}")
    if baseline is not None:
        if failures(baseline):
            failed.append("Baseline is not a valid passing synthetic report")
        else:
            for key in LIMITS:
                old = baseline["metrics"][key]
                # Avoid unstable ratios on sub-millisecond synthetic work.
                allowance = 50 if key == "peak_rss_mb" else .1
                value = metrics.get(key)
                if isinstance(value, (int, float)) and value > max(old * 1.5, old + allowance):
                    failed.append(f"{key}: regression versus baseline ({old:.3f} -> {value:.3f})")
    return failed


def _read_json(path: Path, what: str):
    """The JSON document in the file, or None (with the reason on stderr) when there is none."""
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        print(f"{what} not found: {path}", file=sys.stderr)
    except (OSError, UnicodeDecodeError, ValueError):
        print(f"{what} is not readable JSON: {path}", file=sys.stderr)
    return None


def _run_smoke(executable: Path, output: Path, timeout: float) -> bool:
    """Run the smoke into ``output``; False (with the reason on stderr) when it did not pass."""
    output.parent.mkdir(parents=True, exist_ok=True)
    # Remove a stale report so a failed child cannot appear to have passed.
    if output.exists():
        output.unlink()
    try:
        completed = subprocess.run([str(executable.resolve()), "--workflow-smoke",
                                    str(output.resolve())], timeout=timeout, check=False)
    except subprocess.TimeoutExpired:
        print("Release lifecycle exceeded the process exit deadline", file=sys.stderr)
        return False
    except OSError as error:
        print(f"Release lifecycle could not start: {error}", file=sys.stderr)
        return False
    if completed.returncode != 0 or not output.is_file():
        print("Release lifecycle failed or produced no report", file=sys.stderr)
        return False
    return True


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", nargs="?", type=Path,
                        help="A report openwhisper-cpp-cli --workflow-smoke saved")
    parser.add_argument("--executable", type=Path,
                        help="openwhisper-cpp-cli to run the smoke with, instead of giving a report")
    parser.add_argument("--output", type=Path, default=Path("release-health.json"),
                        help="Where --executable's report goes (default: release-health.json)")
    parser.add_argument("--baseline", type=Path,
                        help="Previously accepted report from equivalent hardware")
    parser.add_argument("--timeout", type=float, default=45,
                        help="Seconds the smoke's process has to exit (default: 45)")
    args = parser.parse_args(argv)
    if (args.report is None) == (args.executable is None):
        parser.error("give either a report to check or --executable to run the smoke")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    path = args.report if args.report is not None else args.output
    if args.baseline and args.baseline.resolve() == path.resolve():
        parser.error("--baseline and the report must be different files")
    if args.executable is not None and not _run_smoke(args.executable, path, args.timeout):
        return 1
    report = _read_json(path, "Release lifecycle report")
    if report is None:
        return 1
    baseline = None
    if args.baseline:
        baseline = _read_json(args.baseline, "Baseline report")
        if baseline is None:
            return 1
    errors = failures(report, baseline)
    for error in errors:
        print(error, file=sys.stderr)
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
