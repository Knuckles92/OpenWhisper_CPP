# Cloud worker brief: closing the port's test ledger from a session of your own

For a worker that runs in its own cloud session (its own container, clone and build tree) and
hands a branch back to the lead. Read `CLAUDE.md` first: its rules bind you. The port's design
is `docs/port/ARCHITECTURE.md`; each milestone's package specs are `docs/port/packages/M*.md`
(their "Tests to port" tables often say where an upstream test was meant to go and which
functions were meant to be dropped, with the reason; the code that landed does not always follow
them file for file, so use them for intent and trust the tree for what exists).

## Set up (once)

```bash
# OpenWhisper at the pinned reference commit (tools/parity/reference.json), read-only.
GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 https://github.com/Knuckles92/openwhisper /home/user/openwhisper-ref
git -C /home/user/openwhisper-ref fetch --depth 1 origin 768b559edd932d40806c96eed05ef5a20e57e415
git -C /home/user/openwhisper-ref checkout -q --detach FETCH_HEAD
export FIORIVOCE_OW_ROOT=/home/user/openwhisper-ref      # every parity tool reads it

# Configure: the first run builds the vcpkg dependencies (about half an hour).
cmake --preset linux -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
      -DCMAKE_CXX_FLAGS_DEBUG=-g1 -DCMAKE_C_FLAGS_DEBUG=-g1
```

Never run git or write anything inside the OpenWhisper checkout after that. If the session's
setup script failed (a start-up message says so), do not propose environment changes, which
nobody is there to approve: read the script with `get_environment`, run by hand the steps that
did not complete (Qt with the `qtsvg` archive, the vcpkg asset script, the test runtime, model
and WAV), and say so in your report.

## The job

Your package is a list of upstream test functions (pytest ids) that the ledger still shows as
`pending` or does not list at all:

```bash
python3 -I -B tools/parity/open_work.py test_x.py test_y.py     # what is open in those files
```

For each one:

1. Read the Python test and the Python code it exercises. The test says what behaviour matters.
2. Find the C++ that ports that behaviour (the area map `docs/port/map/aNN-*.md` and the package
   specs name the files; otherwise search).
3. Resolve it, in this order of preference:
   - **An existing C++ test already proves the same behaviour**: ledger entry `ported`, `cpp`
     naming that test file, `note` naming the test case. Only when it really asserts the same
     thing; read the C++ test, do not go by its name.
   - **The behaviour is there, no test proves it**: write a Catch2 test in the right
     `tests/<dir>/` (a new `test_*.cpp` or an existing one), then `ported`.
   - **The behaviour is missing or differs**: port it from the Python (a pending test often
     marks code nobody wired up), then test it, then `ported`. Say in your report what was
     missing.
   - **It does not apply to FioriVoce**: `dropped` or `other-platform` with a `reason` a
     reviewer can check: another speech engine than Parakeet, Python or PyInstaller plumbing, the
     TypeScript sidecars the native agent loop replaced, macOS- or Linux-only behaviour of the
     Windows product, or a deliberate difference already recorded in `docs/port/differences.md`
     or a package spec. Never because it is hard. If only part applies, port that part.
   - **You cannot finish it**: leave or write `pending` with a true `owner` and `reason`, and
     list it in your report with what blocks it.

A `ported` entry is a claim that a passing C++ test asserts that behaviour. Do not make the claim
unless you ran the test and it passed. Do not weaken, skip, tag away or delete an existing test
to get green. If you find a real bug, fix it and say so.

User-visible strings are copied verbatim from the Python; "OpenWhisper" in copy becomes
`identity().product` (`lint.brand` forbids the literal outside the identity and import code and
tests).

## The ledger

`tests/<dir>/PARITY.json` maps `test_name` or `Class::test_name`, exactly as in the Python file,
to `{"status": "ported", "cpp": ..., "note": ...}`, `{"status": "dropped" | "other-platform",
"reason": ...}` or `{"status": "pending", "owner": ..., "reason": ...}`. The same function may be
listed in several files; the report keeps the most finished status, so add your entry to the
PARITY.json of the directory your C++ test lives in. Write entries with the helper, which
touches only the lines it adds and refuses an invalid result:

```bash
python3 -I -B tools/parity/ledger.py tests/<dir>/PARITY.json tests/test_x.py ported \
    --cpp tests/<dir>/test_x.cpp --note "case: <the Catch2 test case name>" test_a Class::test_b
python3 -I -B tools/parity/ledger.py tests/<dir>/PARITY.json tests/test_x.py dropped --reason "..." test_c
python3 -I -B tools/parity/parity_report.py --check      # schema; must print 0 problems
```

Do not regenerate `docs/port/parity-report.md` or `docs/port/differences.md`, and do not edit
`docs/port/open-items.md` or anything under `docs/port/audit` unless your package says so: the
lead does that from your report.

## Building and testing

```bash
cmake --build --preset linux-debug --target fv_<dir>_tests
ctest --preset linux-debug -L '^<dir>$'            # a directory's tests (CTest names: "<dir>.<case>")
ctest --preset linux-debug -L lint                 # when you touched src/
```

Run tests through `ctest`, never by starting a test executable. Build and run every few
functions, not once at the end, and run each directory you touched in full before you finish.
The decode tests have their runtime, model and WAV (`FIORIVOCE_TEST_*` are set). Windows-only
code under `win/` does not compile here: keep new behaviour platform-neutral where the design
allows; when a change must go in `win/`, write it carefully, keep the `posix/` stub in step, and
list it in your report as not compiled. `[interactive]` tests are never run. Format what you
touch with `clang-format -i`.

### What already fails on Linux and is not a port defect

FioriVoce is a Windows product and its Qt modules were first built with GCC on 2026-11-19. Do
not "fix" these by changing what a test asserts, and do not count them against your work:

- Reference screenshot tests: the pixel half skips; the widget-tree half fails where the
  reference records Windows font metrics (Segoe UI widths) or the windows11 style. That is
  dozens of cases in `ui_settings`, `ui_meeting`, `ui_widgets`, `ui_record`, `ui_hotkeys`,
  `ui_overlay`. Golden cases that replay label widths fail the same way
  (`ui_overlay.ui_qt.overlays.waveform_overlay controls`), as does `ui_widgets.fitDialog grows
  the copy to its height...`.
- Windows wording or capability: two `ui_hotkeys` side-button tests,
  `ui_settings.test_home_privacy_strip` and the "where strip" golden (text reading is
  Windows-only).
- Not yet explained: eight `ui_controller` failures (two About box tests, "M3 end to end: the
  launch check finds a release...", five code-change-restart tests that crash), four `documents`
  golden layout tests, `cli.test_windowed_interpreter_speaks_over_inherited_pipes`. If your
  package touches one of these directories, say what you learn.
- Timing-sensitive under load: `focus.a fast read resets the miss count`.

Run a directory's tests before you change anything in it, so you know its baseline.

## Frozen files and scope

Stay inside your package's directories. A small, additive change elsewhere is allowed when the
behaviour needs it (say so in your report). The lead's files: root `CMakeLists.txt`,
`CMakePresets.json`, `vcpkg.json`, `cmake/*`, `src/settings/keys.inc`, `src/appkit/order.h`,
`src/appkit/destinations.h`, `src/ui_theme/style_props.h`, `src/ui_theme/resources/theme.qss`,
`CLAUDE.md`, `AGENTS.md`, `BUILDING.md`: put the exact change you need in your report and carry
on with the rest. Do not download anything beyond what setup needs, and do not contact any host
but 127.0.0.1 from code or tests.

## Handing back

Commit on your branch as you go, in small commits whose messages say what changed and why, and
push after each. When the package is done (or you are running out of room: stop at a clean,
compiling point with the ledger matching the code), push, then send the lead session your
report with `send_message`. The report, in plain text:

1. Per upstream test file: how many functions you resolved as ported / dropped /
   other-platform, and which are still pending and why.
2. Behaviour that was missing or wrong and that you ported or fixed (files, one line each).
3. The last test run of each directory you touched: passed / failed counts, and any failure
   with its cause.
4. What the lead must do: frozen-file changes, `docs/port/open-items.md` rows your work closes
   (quote the first words), deliberate differences you relied on that are not yet in
   `docs/port/differences.md`, anything not compiled or not run.
5. Your branch name and its last commit.
