# Parity harness

Proves that OpenWhisper_CPP behaves and looks like OpenWhisper (architecture section 9; the design and
the trial that validated it are in `docs/port/map/a33-tests-and-parity-harness.md`). Two
mechanisms, both replayed by C++ tests under `tests/`:

- **Golden vectors**: OpenWhisper's pure functions run on curated and traced inputs; the
  results go to `tests/golden/<python.module>.json` and `fv_golden` replays them.
- **Reference screenshots**: OpenWhisper widgets rendered offscreen with pinned fonts, DPI,
  style and theme, plus a dump of their widget tree, under `tests/reference/<widget>/`;
  `fv_screenshot` compares a C++ widget's tree, then its pixels.

The pinned reference is in `reference.json`: OpenWhisper commit, Python and PyQt6 versions, and
the Qt minors whose renders were measured equal to the PyQt6 references (`qt_compatible`).

## Environment

```powershell
$env:OPENWHISPER_CPP_OW_ROOT = 'D:\coding\whisper_local'                         # the checkout, read-only
$env:OPENWHISPER_CPP_OW_PYTHON = 'D:\coding\whisper_local\.venv\Scripts\python.exe' # Python 3.12.10, PyQt6 6.11.0
```

Every script runs with `-I -B` from the repository root. The scripts refuse to run when the
checkout is not at the pinned commit (`--allow-other-commit` overrides, for trying out a move of
the pin), or when Python or PyQt6 differ from `reference.json`.

## Isolation: nothing may write into OpenWhisper's data

A plain `import config` from a source checkout points at the owner's live settings, database
and recordings. `ow_isolation.py` repeats OpenWhisper's `tests/conftest.py` for scripts:
`OPENWHISPER_DATA_DIR`, `LOCALAPPDATA`, `APPDATA` and `XDG_DATA_HOME` move into
`out/parity/ow-data` and `out/parity/ow-appdata` before anything imports `config` (it refuses
otherwise), bytecode is off, proxies are dropped, `HF_HUB_OFFLINE=1`; after the import it checks
every resolved path and installs the in-memory credential store, a scratch `.env` path and the
null focus service. pytest runs (tracing) also get `PYTHONPYCACHEPREFIX`, `-p no:cacheprovider`
and `--basetemp` under `out/parity/`, because OpenWhisper's conftest turns bytecode writing back
on. Do not run `git` commands that may refresh the index (`git status`) inside the checkout:
the scripts read its HEAD from `.git` directly.

Check a run afterwards, or wrap it:

```powershell
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
    $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/golden_vectors.py --all
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/check_isolation.py --since 2026-11-14T12:00:00
```

It lists every file and folder under the checkout and `%LOCALAPPDATA%\OpenWhisper` whose write
or creation time is newer than the start (names and times only, never contents) and fails when
there is one. Run it from a shell whose `LOCALAPPDATA` is the real one.

## The test ledger

`parity_report.py` aggregates every `tests/<dir>/PARITY.json` into `docs/port/parity-report.md`
(`--check` validates only, `--strict` also fails on anything pending, unlisted or unaudited). Two
helpers work on the ledgers themselves; both read the OpenWhisper checkout named by
`OPENWHISPER_CPP_OW_ROOT` and write nothing there:

```bash
python -I -B tools/parity/open_work.py                    # files with pending or unlisted functions
python -I -B tools/parity/open_work.py test_x.py          # the open functions of a file, by name
python -I -B tools/parity/ledger.py tests/<dir>/PARITY.json tests/test_x.py ported \
    --cpp tests/<dir>/test_x.cpp --note "case: ..." test_a Class::test_b
```

`ledger.py` adds or replaces entries textually, so only the touched lines change, and refuses a
result that is not valid ledger JSON. `docs/port/workflows/cloud-worker.md` is the brief for a
worker closing ledger entries from a cloud session.

## Golden vectors

```powershell
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/golden_vectors.py --module services.format_utils
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/golden_vectors.py --all
```

Inputs come from `cases/<python.module>.py`, one file per module so packages never share one:

```python
CASES = {"function": [[arg, ...], ...]}           # curated argument lists
WRAPPERS = {"method": lambda module, *args: ...}  # optional: methods that need a stub self
TRACE = {"tests": ["tests/test_x.py"], "functions": ["function"]}   # optional
PINS = {"note": "..."}                            # optional, added to the file's pins
FLOAT_TOLERANCE = 1e-6                            # optional absolute tolerance for floats
```

With `TRACE`, the named OpenWhisper test files run under pytest with `trace_plugin.py`, which
records every distinct call the tests make to the functions; their arguments join the curated
ones (marked `"traced": true`) and every result is computed again in the isolated generator
process, so no test's monkeypatching reaches a vector. The plugin wraps functions at session
start, so it misses calls made through references bound earlier (decorators, default arguments,
names a module imported by conftest bound at import); that is a limit of the method, and no
module gets a workaround for it. `--no-trace` writes curated cases only.

Output format (`golden_format.py`): `{"module", "source_commit", "pins", "traced_from",
"float_tolerance"?, "functions": {name: [{"args", "kwargs"?, "result" | "raises", "traced"?}]}}`,
floats as `repr()`, NaN and infinities as `{"$float": "nan" | "inf" | "-inf"}`, anything else as
`{"$repr": "..."}`. A rerun at the same commit gives the same bytes; a changed vector is a
behaviour change to review.

The C++ side (`tests/support/golden/golden.h`, target `fv_golden`):

```cpp
golden::for_each("services.format_utils", "format_audio_duration", [](const golden::Case& c) {
    CHECK(fv::format_audio_duration(c.arg<double>(0)) == c.result<std::string>());
});
```

`Case::arg<T>(i)` and `result<T>()` decode to `bool`, integers, floating types (tags included),
`std::string`, `std::optional<U>` (null), `std::vector<U>` or `Json`; `raises()` and
`raises_type()` give the Python exception. Comparators: `equal` (Python `==` on JSON data),
`near` and `matches` (absolute float tolerance), `equal_rebranded` with `rebrand()`, which maps
OpenWhisper's product, file and variable names to OpenWhisper_CPP's.

## Reference screenshots

```powershell
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/render_reference.py --builder a02_harness `
    --widget empty_state_harness [--theme dark|light] [--style windows11] [--dpr 1] [--scale 100]
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/render_reference.py --theme light --scale 130 --dump-stylesheet
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/render_reference.py --builder a02_harness --check
```

Builders are `builders/aNN.py`, one per area, owned by the package that ports the area's widgets:
`BUILDERS = {name: fn(variant) -> (widget, (width, height), focus widget or None)}` with fixed fake
data, pinned clock, time zone and seeds, and animations driven to their end values. Each render
writes `tests/reference/<widget>/<theme>-<style>-dpr<n>-s<scale>.png` and `.json` (every pin plus
the widget tree). `--dump-stylesheet` writes `tests/reference/app_stylesheet/<theme>-s<scale>.qss`
(the resolved application stylesheet with asset URLs rewritten to `:/ui_theme/assets/`), its
`.sha1`, and `palette-<theme>.json` (`{"Active"|"Inactive"|"Disabled": {role: "#aarrggbb"}}`:
OpenWhisper's palette gives disabled text its own colour). The sidecar also pins `color_scheme`,
the scheme in effect (the offscreen platform keeps `Unknown` whatever the theme asks for).
`--check` renders into `out/parity/render-check/` and fails when a pixel or stylesheet SHA-1 or a
palette differs from the committed files.

Pins on both sides: offscreen platform, `QT_QPA_FONTDIR=%WINDIR%\Fonts`, `QT_FONT_DPI=96`,
`QT_SCALE_FACTOR` (1, or 2 for the HiDPI set), `QT_ENABLE_HIGHDPI_SCALING=1`, no
`QT_SCREEN_SCALE_FACTORS` or `QT_STYLE_OVERRIDE`, the `windows11` base style set after
`QApplication` exists, pinned focus, one `processEvents()`, and the mouse never moved.

The C++ side (`tests/support/screenshot/screenshot.h`, target `fv_screenshot`, with the
`fv_testkit_qt` main):

```cpp
fv::screenshot::expect_matches(widget, "empty_state", "dark-windows11-dpr1-s100",
                               {.focus = addButton});
```

It sizes and shows the widget, compares its tree (`dump_tree`, the same schema as the Python dump;
fonts compare across Qt minors' `QFont::toString` forms), checks that the installed application
stylesheet has the sidecar's `stylesheet_sha1`, and then compares the pixels as ARGB32. On a
difference, the grab and a magenta diff image go to `<build>/test-artifacts/screenshots/<widget>/`.
Pixels are skipped, with a message, when the build has `OPENWHISPER_CPP_SCREENSHOT_TESTS` off (on in
`windows-ci`), off Windows (the references use Windows' fonts), or when the Qt kit's minor is not
in `reference.json`'s `qt_compatible`; the tree is compared either way. The skip ends the test
case, so give each variant its own `TEST_CASE` or `SECTION`. Until `fv_ui_theme` exists,
`ScopedReferenceTheme` installs the dumped stylesheet, palette and font. The default tolerance is 0 differing pixels; an accepted difference is a
`<variant>.tolerance.json` beside the reference, `{"max_differing_pixels": n,
"max_channel_delta": d, "reason": "..."}`, listed in the package report for the lead to approve.

A widget test executable uses `fv_test(NO_MAIN DEPS fv_testkit_qt fv_screenshot ...)`.

## Test classification and parity tracking

```powershell
& $env:OPENWHISPER_CPP_OW_PYTHON -I -B tools/parity/classify_tests.py      # -> classification.json, .md
python -I -B tools/parity/coverage_table.py                          # density per area
python -I -B tools/parity/parity_report.py                           # -> docs/port/parity-report.md
python -I -B tools/parity/parity_report.py --check                   # validate PARITY.json files
```

Every package keeps `tests/<dir>/PARITY.json`:

```json
{"package": "M0.6",
 "files": {"tests/test_settings.py": {
   "test_failed_atomic_replace_preserves_previous_settings":
     {"status": "ported", "cpp": "tests/settings/test_store.cpp"},
   "test_hf_access_policy_defaults_to_ask": {"status": "pending", "owner": "M1.11", "reason": "..."},
   "test_an_existing_linux_install_keeps_local_whisper": {"status": "dropped", "reason": "..."},
   "test_typed_kinds": {"status": "golden", "vectors": "tests/golden/services.setting_types.json"}}}}
```

Statuses: `ported` (with `cpp`), `golden` (with `vectors`), `dropped` (with `reason`), `pending`
(with `owner` and `reason`). Class methods are keyed `Class::test_name`; a parametrized test is
one key. The report lists all 548 OpenWhisper test files; one no PARITY.json names is
`unassigned`. When several PARITY.json files list a function, the most finished status wins
(ported or golden over dropped over pending). CTest runs `parity_report.py --check`
(`support.parity_json`) and the scripts' own tests (`support.parity_tools`).

## Files

| File | Purpose |
|---|---|
| `ow_isolation.py` | Import OpenWhisper without touching its data |
| `check_isolation.py` | Verify a run wrote nothing into the checkout or `%LOCALAPPDATA%\OpenWhisper` |
| `golden_vectors.py`, `golden_format.py`, `cases/` | Golden vectors |
| `trace_plugin.py` | pytest plugin harvesting inputs from OpenWhisper's tests |
| `render_reference.py`, `builders/` | Reference screenshots, tree, stylesheet and palette dumps |
| `classify_tests.py`, `classification.json`, `classification.md`, `coverage_table.py` | OpenWhisper's tests by area and kind |
| `parity_report.py` | Aggregates `tests/*/PARITY.json` |
| `reference.json` | The pinned OpenWhisper commit and versions |
| `tests/` | Self-tests of the scripts (no OpenWhisper needed) |

## Writing audit evidence from several workers at once

`auditput.py` writes one entry or one difference record into a `docs/port/audit/*.json` file in
a single locked load, change and write, checks that the paths an entry names exist, and refuses
an id another audit file already holds. `diffref.py` attaches inventory ids to a difference
record that already exists, wherever it lives. Workers sharing a checkout use these instead of
editing the files by hand, so that two of them cannot lose each other's entries.

```bash
python3 -I -B tools/parity/auditput.py a05-recording-audio.json features A05-01 \
    '{"status": "verified", "cpp": ["src/recorder/recorder.cpp"], "tests": ["tests/recorder/test_recorder.cpp"]}'
python3 -I -B tools/parity/diffref.py a17-retention RE-08
```
