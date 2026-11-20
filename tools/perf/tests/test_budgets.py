"""Tests of the performance tools: budgets and their verdicts, the report's platform labelling,
merging runs and reading benchmark reports. Run with `python -m pytest -q tools/perf/tests`."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import check_budgets  # noqa: E402
import perf_lib  # noqa: E402
import perf_report  # noqa: E402
import run_perf  # noqa: E402


def metric(name: str, samples: list[float], platform: str = "windows", unit: str = "ms",
           build: str = "RelWithDebInfo") -> dict:
    return {"schema": 1, "metric": name, "unit": unit, "samples": samples,
            **perf_lib.summarize(samples), "commit": "abc123", "build": build,
            "machine": {"platform": platform, "os": platform, "cpu": "test cpu",
                        "logical_cores": 4}, "test": "perf.x", "notes": {}}


def write(out: Path, record: dict) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{record['metric']}.json").write_text(json.dumps(record), encoding="utf-8")


def budget(name: str, compare: str, limit, statistic: str = "p50", owner: str = "M10.99") -> dict:
    return {"metric": name, "unit": "ms", "statistic": statistic, "budget": limit,
            "compare": compare, "source": "test", "owner": owner, "test": "perf.x"}


# --- the committed files ---------------------------------------------------------------------

def test_the_committed_budgets_are_valid_and_name_their_owners():
    budgets = perf_lib.load_budgets()
    assert budgets
    for entry in budgets:
        assert entry["owner"].startswith("M10."), entry["metric"]
        assert entry["test"], entry["metric"]


def test_the_spec_budgets_are_present_with_their_owners():
    by_name = {entry["metric"]: entry for entry in perf_lib.load_budgets()}
    expected = {
        "startup_to_window_s": ("max", 0.4, "M10.18"),
        "idle_rss_mb": ("max", 150, "M10.18"),
        "main_window_first_ms": ("ratio", 1.0, "M10.18"),
        "settings_first_ms": ("ratio", 1.0, "M10.20"),
        "history_refresh_100_ms": ("max", 1.9, "M10.10"),
        "stop_to_result_p95_ms": ("ratio", 1.0, "M10.19"),
        "key_to_text_p95_ms": ("max", 300, "M10.19"),
        "dictation_stop_overhead_ms": ("ratio", 1.0, "M10.7"),
        "remote_round_trip_ms": ("ratio", 1.0, "M10.14"),
        "api_upload_ms": ("ratio", 1.0, "M10.7"),
        "agent_ttft_overhead_p95_ms": ("max", 50, "M10.22"),
        "agent_pass_overhead_p95_ms": ("max", 150, "M10.22"),
    }
    for name, (compare, limit, owner) in expected.items():
        assert (by_name[name]["compare"], by_name[name]["budget"], by_name[name]["owner"]) == \
            (compare, limit, owner), name
    assert {name for name in by_name if name.startswith("meeting_state_")} == {
        "meeting_state_runtime_write_ms", "meeting_state_summary_patch_ms",
        "meeting_state_card_edit_ms"}
    assert all(by_name[name]["owner"] == "M10.16" for name in by_name
               if name.startswith("meeting_state_"))


def test_the_quit_metrics_are_reported_not_judged():
    # OpenWhisper's release-health check measures the exit after a busy Quit and sets no bound
    # ("There is no application-wide hard shutdown deadline"), so there is nothing to judge.
    by_name = {entry["metric"]: entry for entry in perf_lib.load_budgets()}
    quit_metrics = ["quit_transcribing_ms", "quit_transcribing_exit_ms", "quit_installing_ms",
                    "quit_installing_exit_ms", "shutdown_s"]
    for name in quit_metrics:
        entry = by_name[name]
        assert (entry["compare"], entry["budget"], entry["statistic"]) == ("info", None, "max")
        assert entry["test"] == "perf.quit while workers are busy"
        assert entry.get("cold") is True
    unavailable = perf_lib.read_json(perf_lib.BASELINE_FILE)["unavailable"]
    assert all(name in unavailable for name in quit_metrics)
    verdicts = perf_lib.evaluate([by_name["quit_installing_ms"]], {},
                                 {"quit_installing_ms": metric("quit_installing_ms", [99999.0])})
    assert [v.status for v in verdicts] == [perf_lib.INFO]


def test_every_baseline_number_has_a_source_and_a_budget():
    baseline = perf_lib.read_json(perf_lib.BASELINE_FILE)
    names = {entry["metric"] for entry in perf_lib.load_budgets()}
    for name, entry in baseline["metrics"].items():
        assert name in names
        assert isinstance(entry["value"], (int, float))
        assert "docs/" in entry["source"]
    assert baseline["metrics"]["main_window_first_ms"]["value"] == 117.2
    assert baseline["metrics"]["settings_first_ms"]["value"] == 221.09
    assert baseline["metrics"]["meeting_state_runtime_write_ms"]["value"] == 6.386


@pytest.mark.parametrize("broken, problem", [
    ([{"metric": "a", "unit": "ms", "statistic": "p50", "budget": 1, "compare": "less",
       "source": "s", "owner": "o"}], "compare"),
    ([{"metric": "a", "unit": "ms", "statistic": "p99", "budget": 1, "compare": "max",
       "source": "s", "owner": "o"}], "statistic"),
    ([{"metric": "a", "unit": "ms", "statistic": "p50", "budget": None, "compare": "max",
       "source": "s", "owner": "o"}], "positive number"),
    ([{"metric": "a", "unit": "ms", "statistic": "p50", "budget": 1, "compare": "max",
       "source": "s"}], "no owner"),
])
def test_invalid_budgets_are_refused(broken, problem):
    assert any(problem in line for line in perf_lib.validate_budgets(broken))


# --- verdicts --------------------------------------------------------------------------------

def test_a_miss_names_its_owner(tmp_path, capsys):
    write(tmp_path, metric("key_to_text_p95_ms", [100.0, 200.0, 900.0]))
    assert check_budgets.main(["--out", str(tmp_path)]) == 1
    printed = capsys.readouterr().out
    assert "key_to_text_p95_ms" in printed
    assert "owner M10.19" in printed


def test_a_ratio_budget_without_an_openwhisper_number_is_no_reference_and_does_not_fail(
        tmp_path, capsys):
    write(tmp_path, metric("remote_round_trip_ms", [5000.0]))
    assert check_budgets.main(["--out", str(tmp_path)]) == 0
    printed = capsys.readouterr().out
    line = next(line for line in printed.splitlines() if "remote_round_trip_ms" in line)
    assert line.startswith("NO REFERENCE")


def test_a_ratio_budget_is_judged_against_the_openwhisper_number():
    verdicts = perf_lib.evaluate(
        [budget("x_ms", "ratio", 1.0), budget("y_ms", "ratio", 2.0)],
        {"x_ms": {"value": 10.0}, "y_ms": {"value": 10.0}},
        {"x_ms": metric("x_ms", [12.0]), "y_ms": metric("y_ms", [12.0])})
    assert [(v.status, v.limit) for v in verdicts] == [(perf_lib.MISS, 10.0),
                                                       (perf_lib.PASS, 20.0)]


def test_unmeasured_and_info_metrics_never_fail(tmp_path):
    verdicts = perf_lib.evaluate([budget("a_ms", "max", 1.0), budget("b_ms", "info", None)],
                                 {}, {"b_ms": metric("b_ms", [99.0])})
    assert [v.status for v in verdicts] == [perf_lib.NOT_MEASURED, perf_lib.INFO]
    assert perf_lib.misses(verdicts) == []
    assert check_budgets.main(["--out", str(tmp_path / "empty")]) == 0


def test_the_statistic_a_budget_names_is_the_one_judged():
    samples = [10.0] * 19 + [1000.0]
    verdicts = perf_lib.evaluate(
        [budget("p50_ms", "max", 50, "p50"), budget("p95_ms", "max", 50, "p95")], {},
        {"p50_ms": metric("p50_ms", samples), "p95_ms": metric("p95_ms", samples)})
    assert [v.status for v in verdicts] == [perf_lib.PASS, perf_lib.MISS]


def test_a_measured_metric_without_a_budget_is_still_reported():
    verdicts = perf_lib.evaluate([], {}, {"extra_ms": metric("extra_ms", [1.0])})
    assert [(v.metric, v.status, v.owner) for v in verdicts] == [("extra_ms", perf_lib.INFO, "-")]


def test_numbers_measured_under_decision_5_replace_the_documents(tmp_path):
    (tmp_path / perf_lib.OW_MEASURED_NAME).write_text(json.dumps(
        {"metrics": {"remote_round_trip_ms": {"value": 30.0, "source": "measured: x"}}}),
        encoding="utf-8")
    references = perf_lib.load_baseline(perf_lib.BASELINE_FILE, tmp_path)
    assert references["remote_round_trip_ms"]["kind"] == "measured"
    assert references["main_window_first_ms"]["kind"] == "document"


def test_percentiles_interpolate_between_ranks():
    assert perf_lib.percentile([1.0, 2.0, 3.0, 4.0], 50) == 2.5
    assert perf_lib.percentile([1.0, 2.0, 3.0, 4.0, 5.0], 95) == pytest.approx(4.8)
    assert perf_lib.percentile([7.0], 95) == 7.0


# --- the report ------------------------------------------------------------------------------

def test_the_report_says_which_platform_the_numbers_were_measured_on(tmp_path):
    write(tmp_path, metric("history_refresh_100_ms", [1.0, 1.2], platform="linux", build="Debug"))
    output = perf_report.write_report(tmp_path, tmp_path / "report.md")
    text = output.read_text(encoding="utf-8")
    assert "Measured on Linux, not Windows" in text
    assert "not the product's numbers" in text
    assert "Debug build" in text
    row = next(line for line in text.splitlines() if "`history_refresh_100_ms`" in line)
    assert "Linux, Debug" in row
    assert "| pass |" in row
    assert "M10.10" in row


def test_a_windows_report_carries_no_other_platform_warning(tmp_path):
    write(tmp_path, metric("history_refresh_100_ms", [1.0]))
    text = perf_report.write_report(tmp_path, tmp_path / "r.md").read_text(encoding="utf-8")
    assert "not Windows" not in text
    assert "Windows, RelWithDebInfo" in text


def test_the_committed_report_is_not_written_from_linux_numbers(tmp_path):
    write(tmp_path, metric("history_refresh_100_ms", [1.0], platform="linux"))
    with pytest.raises(SystemExit, match="Windows run"):
        perf_report.write_report(tmp_path, perf_report.COMMITTED_REPORT)


def test_the_report_lists_every_budgeted_metric_with_its_openwhisper_number(tmp_path):
    text = perf_report.write_report(tmp_path, tmp_path / "r.md").read_text(encoding="utf-8")
    for entry in perf_lib.load_budgets():
        assert f"`{entry['metric']}`" in text
    row = next(line for line in text.splitlines() if "| `settings_first_ms`" in line)
    assert "221.1 (document)" in row
    row = next(line for line in text.splitlines() if "| `api_upload_ms`" in line)
    assert "| none |" in row
    assert "not measured" in row


def test_check_budgets_report_writes_the_report(tmp_path):
    write(tmp_path, metric("idle_rss_mb", [80.0], unit="MB"))
    assert check_budgets.main(["--out", str(tmp_path), "--report"]) == 0
    assert (tmp_path / "performance-report.md").is_file()


# --- run_perf ---------------------------------------------------------------------------------

def test_runs_merge_their_samples(tmp_path):
    first, second = tmp_path / "runs" / "1", tmp_path / "runs" / "2"
    write(first, metric("startup_to_window_s", [0.2], unit="s"))
    write(second, metric("startup_to_window_s", [0.4], unit="s"))
    write(first, metric("history_refresh_100_ms", [1.0, 2.0]))
    assert run_perf.merge_runs([first, second], tmp_path) == ["history_refresh_100_ms",
                                                              "startup_to_window_s"]
    merged = perf_lib.read_json(tmp_path / "startup_to_window_s.json")
    assert merged["samples"] == [0.2, 0.4]
    assert merged["runs"] == 2
    assert merged["p50"] == pytest.approx(0.3)


def test_ctest_names_are_escaped_for_cmake_regexes():
    regex = run_perf.tests_regex(["perf.dictation overhead: API upload",
                                  "perf.meeting state SQL writes on a 739,068-byte document"])
    assert regex == ("^(perf\\.dictation overhead: API upload|"
                     "perf\\.meeting state SQL writes on a 739,068-byte document)$")
    assert run_perf.tests_regex([]) == "^perf\\."


def test_every_measuring_test_named_by_a_budget_exists():
    sources = "".join(path.read_text(encoding="utf-8")
                      for path in (perf_lib.REPO / "tests" / "perf").glob("test_*.cpp"))
    for entry in perf_lib.load_budgets():
        if entry["test"].startswith("perf."):
            assert f'TEST_CASE("{entry["test"][len("perf."):]}"' in sources, entry["test"]


def test_benchmark_reports_become_metrics(tmp_path):
    (tmp_path / "bench-live-preview.json").write_text(json.dumps(
        {"schema": 1, "kind": "live-preview", "metrics": {"wer": 0.12, "label": "x"},
         "provenance": {"commit": "c0ffee", "machine": {"platform": "windows"}}}),
        encoding="utf-8")
    assert run_perf.ingest_bench_reports(tmp_path, "head") == ["live_preview_wer"]
    record = perf_lib.read_json(tmp_path / "live_preview_wer.json")
    assert record["p50"] == 0.12
    assert record["commit"] == "c0ffee"
    assert record["machine"]["platform"] == "windows"
    assert "live_preview_wer" in perf_lib.load_measured(tmp_path)


def test_openwhisper_runs_need_lead_decision_5(tmp_path, monkeypatch):
    monkeypatch.delenv("OPENWHISPER_CPP_OW_APPROVED", raising=False)
    with pytest.raises(SystemExit, match="lead decision 5"):
        run_perf.run_openwhisper(tmp_path)


def test_a_busy_machine_is_refused(monkeypatch):
    monkeypatch.setattr(run_perf, "cpu_busy_percent", lambda: 75.0)
    with pytest.raises(SystemExit, match="busy"):
        run_perf.check_load(ignore=False)
    run_perf.check_load(ignore=True)


def test_preset_paths_follow_cmake_presets():
    folder, configuration = run_perf.preset_paths("linux-release")
    assert folder == perf_lib.REPO / "out" / "build" / "linux"
    assert configuration == "RelWithDebInfo"
    folder, configuration = run_perf.preset_paths("windows-release")
    assert folder == perf_lib.REPO / "out" / "build" / "windows"


def test_measuring_tests_run_through_the_perf_test_preset():
    # The default test presets exclude the perf label, so the runner must not use them.
    assert run_perf.perf_test_preset("linux-release") == ("linux-perf", ["-C", "RelWithDebInfo"])
    assert run_perf.perf_test_preset("windows-release") == ("windows-perf", ["-C", "RelWithDebInfo"])
    assert run_perf.perf_test_preset("linux-debug") == ("linux-perf", ["-C", "Debug"])
    assert run_perf.perf_test_preset("no-such-preset") == ("no-such-preset", [])
