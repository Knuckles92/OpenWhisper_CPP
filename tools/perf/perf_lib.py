"""What run_perf.py, perf_report.py and check_budgets.py share: the budgets, OpenWhisper's
reference numbers, the measured metric files and the verdict for each metric.

Files:
  tools/perf/budgets.json               [{metric, unit, statistic, budget, compare, source,
                                          owner, test?, cold?, note?}]
  tools/perf/baseline-openwhisper.json  {metrics: {name: {value, unit, statistic, source}},
                                          unavailable: {name: why}}
  out/perf/<metric>.json                {metric, unit, samples, p50, p95, max, commit, machine,
                                          build, test, notes, runs?}
  out/perf/openwhisper-measured.json    numbers measured by run_perf.py --openwhisper (lead
                                          decision 5); they replace the baseline's when present

compare: "max" (the statistic must be at most `budget`), "ratio" (at most `budget` times
OpenWhisper's number; with no number the verdict is "no reference" and nothing fails), "info"
(recorded and reported, never judged).
"""
from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools" / "perf"
BUDGETS_FILE = TOOLS / "budgets.json"
BASELINE_FILE = TOOLS / "baseline-openwhisper.json"
DEFAULT_OUT = REPO / "out" / "perf"
OW_MEASURED_NAME = "openwhisper-measured.json"

COMPARES = ("max", "ratio", "info")
STATISTICS = ("p50", "p95", "max")

# Verdicts. Only MISS fails check_budgets.py.
PASS = "pass"
MISS = "miss"
NO_REFERENCE = "no reference"
NOT_MEASURED = "not measured"
INFO = "info"


def percentile(values: list[float], pct: float) -> float:
    """Linear interpolation between closest ranks (numpy's default), as the C++ tests compute
    it (tests/perf/support/perf_support.h)."""
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = pct / 100.0 * (len(ordered) - 1)
    low, high = math.floor(rank), math.ceil(rank)
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def summarize(samples: list[float]) -> dict:
    return {"p50": percentile(samples, 50), "p95": percentile(samples, 95),
            "max": max(samples) if samples else math.nan}


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def load_budgets(path: Path = BUDGETS_FILE) -> list[dict]:
    budgets = read_json(path)
    problems = validate_budgets(budgets)
    if problems:
        raise ValueError(f"{path}: " + "; ".join(problems))
    return budgets


def validate_budgets(budgets) -> list[str]:
    problems: list[str] = []
    if not isinstance(budgets, list):
        return ["the budgets file is a list of budgets"]
    seen: set[str] = set()
    for index, budget in enumerate(budgets):
        name = budget.get("metric") if isinstance(budget, dict) else None
        where = name or f"entry {index}"
        if not isinstance(budget, dict) or not isinstance(name, str) or not name:
            problems.append(f"{where}: no metric name")
            continue
        if name in seen:
            problems.append(f"{name}: listed twice")
        seen.add(name)
        for key in ("unit", "source", "owner"):
            if not isinstance(budget.get(key), str) or not budget[key]:
                problems.append(f"{name}: no {key}")
        if budget.get("compare") not in COMPARES:
            problems.append(f"{name}: compare must be one of {', '.join(COMPARES)}")
        if budget.get("statistic") not in STATISTICS:
            problems.append(f"{name}: statistic must be one of {', '.join(STATISTICS)}")
        limit = budget.get("budget")
        if budget.get("compare") in ("max", "ratio"):
            if not isinstance(limit, (int, float)) or isinstance(limit, bool) or limit <= 0:
                problems.append(f"{name}: a {budget.get('compare')} budget needs a positive number")
        elif limit is not None:
            problems.append(f"{name}: an info metric has no budget")
    return problems


def load_baseline(path: Path = BASELINE_FILE, out: Path | None = None) -> dict[str, dict]:
    """OpenWhisper's numbers by metric: the committed documents' (kind "document"), replaced by
    numbers measured under lead decision 5 (kind "measured") when out/perf has them."""
    references: dict[str, dict] = {}
    if path.is_file():
        for name, entry in read_json(path).get("metrics", {}).items():
            references[name] = {**entry, "kind": "document"}
    if out is not None and (out / OW_MEASURED_NAME).is_file():
        for name, entry in read_json(out / OW_MEASURED_NAME).get("metrics", {}).items():
            references[name] = {**entry, "kind": "measured"}
    return references


def load_measured(out: Path = DEFAULT_OUT) -> dict[str, dict]:
    measured: dict[str, dict] = {}
    if not out.is_dir():
        return measured
    for path in sorted(out.glob("*.json")):
        if path.name == OW_MEASURED_NAME or path.name.startswith("bench-"):
            continue
        try:
            data = read_json(path)
        except (OSError, ValueError):
            continue
        if isinstance(data, dict) and isinstance(data.get("metric"), str):
            measured[data["metric"]] = data
    return measured


def platform_of(record: dict) -> str:
    machine = record.get("machine") or {}
    return str(machine.get("platform") or "unknown")


@dataclass
class Verdict:
    metric: str
    unit: str
    statistic: str
    compare: str
    owner: str
    source: str
    status: str
    value: float | None = None
    limit: float | None = None
    reference: dict | None = None
    record: dict | None = None
    note: str = ""
    extra: dict = field(default_factory=dict)

    @property
    def platform(self) -> str:
        return platform_of(self.record) if self.record else "-"


def evaluate(budgets: list[dict], references: dict[str, dict],
             measured: dict[str, dict]) -> list[Verdict]:
    """One verdict per budgeted metric, then one ("info", owner "-") for each measured metric no
    budget names, so nothing measured is left out of the report."""
    verdicts: list[Verdict] = []
    for budget in budgets:
        name = budget["metric"]
        record = measured.get(name)
        reference = references.get(name)
        verdict = Verdict(metric=name, unit=budget["unit"], statistic=budget["statistic"],
                          compare=budget["compare"], owner=budget["owner"],
                          source=budget["source"], status=NOT_MEASURED, reference=reference,
                          record=record, note=budget.get("note", ""))
        if record is not None:
            value = record.get(budget["statistic"])
            verdict.value = value if isinstance(value, (int, float)) else None
        if budget["compare"] == "max":
            verdict.limit = float(budget["budget"])
        elif budget["compare"] == "ratio" and reference and isinstance(
                reference.get("value"), (int, float)):
            verdict.limit = float(budget["budget"]) * float(reference["value"])

        if verdict.value is None or (isinstance(verdict.value, float) and
                                     math.isnan(verdict.value)):
            verdict.status = NOT_MEASURED
        elif budget["compare"] == "info":
            verdict.status = INFO
        elif verdict.limit is None:
            verdict.status = NO_REFERENCE
        else:
            verdict.status = PASS if verdict.value <= verdict.limit else MISS
        verdicts.append(verdict)

    named = {budget["metric"] for budget in budgets}
    for name, record in sorted(measured.items()):
        if name in named:
            continue
        verdicts.append(Verdict(metric=name, unit=str(record.get("unit", "")), statistic="p50",
                                compare="info", owner="-", source="no budget",
                                status=INFO, value=record.get("p50"), record=record,
                                reference=references.get(name)))
    return verdicts


def misses(verdicts: list[Verdict]) -> list[Verdict]:
    return [verdict for verdict in verdicts if verdict.status == MISS]
