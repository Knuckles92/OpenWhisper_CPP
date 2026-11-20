# tools/perf: performance budgets (M10.3)

OpenWhisper_CPP's performance metrics, the budgets they are held to, OpenWhisper's numbers where it
has them, and the report. The measuring tests are C++ (`tests/perf`, CTest label `perf`, names
`perf.<case>`); these tools run them, judge them and write the report.

| File | What it is |
|---|---|
| `budgets.json` | `[{metric, unit, statistic, budget, compare, source, owner, test, cold?, note?}]`. `compare`: `max` (the statistic at most `budget`), `ratio` (at most `budget` x OpenWhisper's number), `info` (reported, never judged). `owner` is the sweep package that fixes a miss. |
| `baseline-openwhisper.json` | OpenWhisper's numbers from its committed documents, each with its source, and the metrics it has none for (`unavailable`). No OpenWhisper code was run to make it. |
| `run_perf.py` | Builds, runs the measuring tests (serially, the cold-start ones in five fresh processes), the workflow smoke, reads M10.4's `out/perf/bench-*.json`, writes `out/perf/<metric>.json` and the report. Refuses to run on a busy machine. |
| `check_budgets.py` | Judges every metric; exit 1 on a miss, naming the owner. A ratio budget with no OpenWhisper number is "no reference" and does not fail. `--report` also writes the report. |
| `perf_report.py` | Writes the report (default `out/perf/performance-report.md`). Says which platform and build each number was measured on, and refuses to write `docs/port/performance-report.md` from non-Windows numbers. |
| `perf_lib.py` | What the three share: budgets, references, measured files, verdicts, percentiles. |
| `ow_probes/run_probes.py` | OpenWhisper's own `--workflow-smoke` and `bench_dictation_overhead.py`, under `tools/parity/ow_isolation.py` and `check_isolation.py`. **Not run yet**: it needs lead decision 5. |
| `tests/` | `python -m pytest -q tools/perf/tests` |

Each `out/perf/<metric>.json` is `{metric, unit, samples, p50, p95, max, commit, machine, build,
test, notes}`; `machine` names the platform, OS, CPU and core count (never a host or user name).
Percentiles interpolate between ranks, the same in C++ (`tests/perf/support/perf_support.h`) and
Python. The measuring tests never fail on a budget: each asserts only that its number stays under
ten times its budget (no ceiling for a ratio budget without OpenWhisper's number; in a Debug build the ceiling is ten times wider again, since budgets are for optimised builds).

## The metrics and their tests

| Test (`perf.` ...) | Metrics |
|---|---|
| `startup to the main window and idle footprint` | `startup_to_window_s` (Bootstrap's own metric: GUI bootstrap entry to the main window shown, real Bootstrap offscreen over fakes), `idle_rss_mb` (resident set after 2 s idle, no model) |
| `main window first and warm construction` | `main_window_first_ms`, `main_window_warm_ms`, `main_window_widgets`, `main_window_show_events_ms` |
| `settings window first and warm construction` | `settings_first_ms`, `settings_warm_ms`, `settings_widgets`, `settings_show_events_ms` |
| `history refresh of 100 unchanged cards` | `history_refresh_100_ms`, `history_first_100_cards_ms` |
| `meeting state SQL writes on a 739,068-byte document` | `meeting_state_runtime_write_ms`, `meeting_state_summary_patch_ms`, `meeting_state_card_edit_ms` |
| `key to text with the fake engine` | `key_to_text_p95_ms`, `key_to_text_p50_ms` |
| `key to text with Parakeet` (`[decode]`, needs `OPENWHISPER_CPP_TEST_*`) | `key_to_text_decode_p95_ms` |
| `dictation overhead: stop to 16 kHz audio` | `dictation_stop_overhead_ms` and its parts |
| `dictation overhead: remote round trip` | `remote_round_trip_ms`, `remote_request_ms` |
| `dictation overhead: API upload` | `api_upload_ms`, `api_upload_cold_ms` |
| `agent loop time to first token and pass overhead` | `agent_ttft_overhead_p95_ms`, `agent_pass_overhead_p95_ms` |
| `quit while workers are busy` | OpenWhisper's release-health check "Quit while workers are busy: measure process exit time". Real Bootstrap offscreen over fakes; Quit confirmed through the busy question during the stop's decode of a 120 s dictation (`quit_transcribing_ms`, `quit_transcribing_exit_ms`; a modelled 1.5 s window that cannot be interrupted, as Parakeet's cannot) and during the CUDA runtime's download from 127.0.0.1 (`quit_installing_ms`, `quit_installing_exit_ms`); `_ms` to the end of the shutdown plan, `_exit_ms` to `Bootstrap::run` returning; `shutdown_s` (the plan's own metric). All `info`: OpenWhisper sets no shutdown bound |
| (`openwhisper-cpp-cli --workflow-smoke`, 5 runs) | `stop_to_result_p95_ms`, `stop_to_result_p50_ms`, `cancel_ms`, `restart_ms`, `peak_rss_mb` |

No measuring test touches the desktop: the keyboard and mouse hooks, synthetic keys, tray, audio
devices and clipboard are fakes (the offscreen platform's clipboard is private to the process),
every case has its own temporary data folder, and the only network is 127.0.0.1.

## The owner's Windows run (writes the committed report)

On the build machine, with nothing else running (no build, no indexing), from a Developer
PowerShell at the checkout root:

```powershell
. .\scripts\vsenv.ps1
cmake --preset windows                      # once
$env:OPENWHISPER_CPP_TEST_RUNTIME_DIR = "<NeMo-Speech.cpp GPU release folder>"   # optional: [decode]
$env:OPENWHISPER_CPP_TEST_MODEL = "<parakeet .gguf>"
$env:OPENWHISPER_CPP_TEST_WAV = "<a 16 kHz speech WAV>"
python tools/perf/run_perf.py --preset windows-release --report-to docs/port/performance-report.md
python tools/perf/check_budgets.py
```

`run_perf.py` builds `fv_perf_tests` and `openwhisper-cpp-cli` with `windows-release`, runs
`ctest --preset windows-release -R "^perf\." -L perf -j1` (plus four more processes for each
cold-start test), the workflow smoke five times, and writes the report. `check_budgets.py` exits 1
on a miss and prints the owner package. Once lead decision 5 is approved, add OpenWhisper's
measured numbers to the same run:

```powershell
$env:OPENWHISPER_CPP_OW_APPROVED = "1"
$env:OPENWHISPER_CPP_OW_ROOT = "<read-only OpenWhisper checkout at tools/parity/reference.json's commit>"
$env:OPENWHISPER_CPP_OW_PYTHON = "<OpenWhisper's Python 3.12>"
python tools/perf/run_perf.py --preset windows-release --openwhisper --report-to docs/port/performance-report.md
```

One metric: `python tools/perf/run_perf.py --metric settings_first_ms` (it reruns only that
metric's test and keeps the other files in `out/perf`).

## Elsewhere (a smoke run, not the product's numbers)

On Linux the same commands run with `--preset linux-release` (the default there) or
`linux-debug`. The report says plainly that its numbers were not measured on Windows, and
`perf_report.py` refuses to overwrite `docs/port/performance-report.md` with them:

```bash
python3 tools/perf/run_perf.py --preset linux-debug --ignore-load   # --ignore-load: smoke only
python3 tools/perf/check_budgets.py --report                         # out/perf/performance-report.md
```

## Presets (lead decision 3)

The default test presets exclude the `perf` label, so a plain `ctest --preset windows-debug`
measures nothing. `windows-perf` (and `linux-perf`, whose numbers are not the product's) run
the measuring tests one at a time; `run_perf.py` goes through them with the configuration of
the build preset it was given (`ctest --preset windows-perf -C RelWithDebInfo -R ... -L perf`).

