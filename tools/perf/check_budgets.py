"""Check the measured metrics against their budgets.

    python tools/perf/check_budgets.py [--out out/perf] [--report] [--output <file.md>]

Prints one line per budgeted metric and exits 1 when any metric misses its budget, naming the
package that owns the fix. A ratio budget whose OpenWhisper number is missing reports "no
reference" and does not fail; neither does a metric that was not measured, nor an "info" one.
--report also writes the report (perf_report.py; out/perf/performance-report.md by default).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_lib  # noqa: E402
import perf_report  # noqa: E402


def check(out: Path) -> tuple[list[perf_lib.Verdict], list[str]]:
    budgets = perf_lib.load_budgets()
    references = perf_lib.load_baseline(perf_lib.BASELINE_FILE, out)
    verdicts = perf_lib.evaluate(budgets, references, perf_lib.load_measured(out))
    lines: list[str] = []
    for verdict in verdicts:
        value = perf_report.fmt(verdict.value, verdict.unit)
        limit = perf_report.fmt(verdict.limit, verdict.unit)
        where = f" [{verdict.platform}]" if verdict.record else ""
        line = (f"{verdict.status.upper():<13} {verdict.metric:<34} {verdict.statistic} "
                f"{value:>9} {verdict.unit:<7} limit {limit:>9}{where}")
        if verdict.status == perf_lib.MISS:
            line += f"  -> owner {verdict.owner}"
        lines.append(line)
    return verdicts, lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=perf_lib.DEFAULT_OUT)
    parser.add_argument("--report", action="store_true", help="also write the report")
    parser.add_argument("--output", type=Path, help="where --report writes it")
    parser.add_argument("--allow-non-windows", action="store_true")
    args = parser.parse_args(argv)

    verdicts, lines = check(args.out)
    for line in lines:
        print(line)
    missed = perf_lib.misses(verdicts)
    if args.report:
        output = args.output or args.out / "performance-report.md"
        print(f"check_budgets: wrote "
              f"{perf_report.write_report(args.out, output, args.allow_non_windows)}")
    if missed:
        print(f"check_budgets: {len(missed)} budget(s) missed:")
        for verdict in missed:
            print(f"  {verdict.metric}: {perf_report.fmt(verdict.value, verdict.unit)} "
                  f"{verdict.unit} > {perf_report.fmt(verdict.limit, verdict.unit)} "
                  f"{verdict.unit} on {verdict.platform}; owner {verdict.owner}")
        return 1
    print("check_budgets: no budget missed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
