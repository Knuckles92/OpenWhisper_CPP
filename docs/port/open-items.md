# Open items

What the reviews left open, kept by the lead so that nothing is lost between waves. An item
leaves this list when a package closes it. Items a later package must act on are also in its
spec or in the architecture's lead amendments; the rest are swept in M10.

## Cloud completion branch, November 19, 2026

Branch `codex/cloud-development`, continued from the owner's commit 4029062. All the work of
November 19 was done in Linux cloud containers. The whole tree, the Qt modules included, was
built for the first time with GCC 13 and Qt 6.10.3 (preset `linux`, Debug). That took four small
source fixes for GCC and one link option, `--exclude-libs,ALL` in the root `CMakeLists.txt`, so
that vcpkg's static zlib does not interpose the system zlib that Qt's PNG reader uses. Nothing on
this branch has been compiled or run on Windows since 4029062.

`tools/parity/parity_report.py --check --strict` reports 0 problems. Of OpenWhisper's 7,766
test functions in 548 files at the reference commit, 6,741 are ported and 15 replayed as golden
vectors, 858 are dropped with a reason and 152 belong to another platform; none is pending or
unlisted, where 971 were open when the day began. The 271 stale pending rows that other ledgers
already resolved were removed. The behaviour audit gives every one of the 2,405 inventory ids of
the 34 areas evidence in `docs/port/audit`: 1,697 verified, 413 fixed during the audit (a
missing test written or a defect corrected), 159 covered by 86 recorded deliberate differences
(`docs/port/differences.md`), 61 dropped and 75 other-platform. No handoff is left. The
user-facing checklist built from OpenWhisper's documentation (area a32, 351 lines) is part of
that count.

The audit found real defects as well as missing tests. The meeting agent's cores never received
their charter (an empty system prompt), and the insight review's regular expressions had lost
their word boundaries in the port. The remote engine's reconnect ran its timeout attempts at
once, a running host download read as failed, and History never told the record sync what it
saved, edited or deleted, so nothing reached a host that keeps a paired computer's records. The
walkthrough's recovery never ran, and an open Saved Walkthroughs window did not refresh when the
Walkthrough tab saved or deleted one. The quit sequence had two use-after-free faults, one when
a stop was still finalizing its live preview and one in the timer pool (`base/executor.cpp`);
Quit did not ask during a component install, and waits that held Quit are now cancelled at their
source. The updater could stay silent about a failure it had not reported, and left downloaded
archives until a later start.

The sweep that followed reconciled the review findings this file held with the tree and sorted
them (`docs/port/workflows/open-items-triage.md`); the parity gaps and defects among them were
worked through. Among the changes: the AI cleanup card hosts the shared agent picker, the
database opens after the first paint and a database that will not open leaves the app running
with failing saves, as OpenWhisper's does, and an OpenWhisper import no longer copies the
database to count its history. The sections below then held 40 rows waiting on the owner, 4 for
the lead, 13 for named later packages and 130 for the M10 sweep (129 after the Windows build
below).

The whole tree builds. The default test set (`ctest --preset linux-debug`, which excludes
`interactive` and `perf`) ran 12,255 tests: 11,831 passed and 424 failed, all for known reasons
that are not port defects: 408 reference-screenshot and "matches OpenWhisper" comparisons
recorded on Windows (font metrics and the windows11 style), four document goldens that expect
Windows font names, one overlay golden of label widths, the GUI self-test (no windows11 style
plugin on Linux) and ten multi-screen snap tests (the offscreen platform has one screen). The
decode tests ran against the CPU runtime with the Parakeet model. The performance suite
(`tools/perf`) ran only as smoke runs on a shared 4-core container, once in a Debug build and
once optimised (`linux-release`): they show that the measuring tests execute, and by the
harness's own rule their numbers are not the product's. In the optimised run the budgets with a
fixed limit passed (key to text at 15 ms against 300 ms, the agent loop's overheads), and five
budgets set relative to OpenWhisper's documented numbers, which were measured on another
machine, were over: `main_window_warm_ms` and `history_first_100_cards_ms` by 1 to 2 %, and the
three meeting state SQL writes (`meeting_state_runtime_write_ms`, `_summary_patch_ms`,
`_card_edit_ms`) by 22 to 46 %. The owner's Windows run decides whether those five are real. The
`[interactive]` tests were never run.

The Windows build that followed is the next section. What is left for the owner is the
`[interactive]` suite, the Windows performance run that judges the budgets, and the decisions
taken on the owner's behalf and recorded as differences for confirmation. What remains in the
technical sections is mostly missing tests and later improvements, with the open questions the
sweep raised.

## Windows build of the cloud branch, November 19, 2026

The branch was built and tested on the owner's Windows machine (MSVC 19.51, Qt 6.10.3, vcpkg at
the manifest baseline), with the build trees outside the checkout and the machine in use, so
builds ran at below-normal priority and the `[interactive]` and `perf` labels were left out.

`windows-ci` (warnings as errors, lints, screenshots) builds. MSVC refused seven files the GCC
build had accepted: `DOMAIN`, a macro of the C runtime's `math.h`, as a name in
`remote_host/reachability.h` (the categories are now `CATEGORY_PUBLIC`, `_PRIVATE`,
`_DOMAIN`); four locals or parameters that hid a member (three `QWidget::data`); comparisons of a
`Json` with a `std::string_view`, which are ambiguous under MSVC; code after `FAIL()`
(unreachable, C4702); and `fopen` (C4996). Its default test set, with the `[decode]` cases run on
the owner's CUDA runtime and Parakeet model (a synthesized recording of jfk.wav's sentence),
first ran 12,348 tests with 44 failures and now passes all of them (no screenshot is skipped:
`tools/parity/reference.json` lists Qt 6.10 as rendering like the references). The failures
were:

- 36 reference screenshots. The builders take out of OpenWhisper's trees the widgets the port
  did not build yet; on 2026-11-19 the port gained the Remote engine fields of the record tabs,
  the stats strip's detail line and the AI cleanup page's installed-agent runner, all hidden in
  the rendered states. The C++ tests now take them out as the builders do, which is enough: the
  pixels already matched. Home's where strip now lists Learned rules, as OpenWhisper's own code
  does, so `builders/a11.py`'s fixed list was corrected and the four `settings_home` references
  were rendered again at the pinned commit (a render `--check` first reproduced the old ones;
  `check_isolation.py` found nothing written to OpenWhisper's folders). On Linux every
  screenshot fails on fonts, which is how these went unseen there (M10 sweep).
- A real defect: `bench/live_preview.cpp`'s report pooling kept `std::initializer_list`s in a
  `std::pair` array, whose backing arrays had died; MSVC's optimised build read garbage and
  pooled runs with a different runtime or model.
- Tests that assumed Linux: `%TEMP%` spelled with 8.3 short names against stored long names
  (`TempDataRoot` now takes the long form), a temporary folder ending in a separator, a test
  name with a non-ASCII arrow that Catch2 cannot be given on Windows, a 420-character path
  without the `\\?\` form, the installed check reaching the Visual C++ runtime in System32, and
  `[decode]` WER scored against jfk.wav's text.

The Windows-only items that could not be written in the cloud are done: the WASAPI
`default_loopback_id` override (one `GetDefaultAudioEndpoint`), `FileLock` removing
`<key>.lock` as filelock does, the installed check's list of Windows libraries, and UTF-16
arguments for the lease and detached test helpers (their tests pass with a temporary folder
named outside the ANSI code page). The mouse hook's destructor now waits only for the callback
that is running and drops what is queued behind it; a time limit would let a thread outlive the
owner it calls. The three `process` items needed no change (M10 sweep, `process`). With the new
list, `fiorivoce-cli --self-test --installed` passes on a stage that `packaging/windows/stage.ps1`
made from the `windows-ci` tree (windeployqt included).

`windows-debug` builds with no error, and its default set passes: 12,349 tests (the
`RealtimeGuard` allocation test runs only without `NDEBUG`). Its build-folder import check had
to accept the Visual C++ debug runtime (`MSVCP140D.dll`, `ucrtbased.dll`, ...), which Visual
Studio puts in System32 and which the installed check rightly does not count as the system's.

## SSD integration branch, November 18, 2026

This section describes that earlier checkpoint; the section above supersedes its counts and
estimates.

Resume on `codex/resume-port-n4` in `C:\coding\fiorivoce_cpp`. The D: checkout was 154
commits behind the SSD checkpoint. Six saved implementations were recovered from the SSD
worktrees (M7.18, M7.19, M8.4, M8.5, M9.4 and M9.7), including review fixes.

The desktop wires the recorder, jobs, paste, history and model/download pages to native modules.
Walkthrough capture and its library, meeting capture and recovery, optional speaker review,
and local or paired remote speech are connected. Both peer directions paired over TLS and
transcribed synthetic audio against the pinned Python implementation. Explicit sharing of a
finished meeting can include audio. Host and local copies have independent identities and
credentials. The live meeting scheduler supports notes, highlights, topic changes, question
tracking, citations and knowledge sources.

The Windows release includes a native updater, per-user registration, guarded install-directory
swaps, consistent SQLite snapshots, a health acknowledgement and recovery after an interrupted
upgrade. The installer and portable update ZIP are generated locally. The reference dashboard
rebuilt from its TypeScript sources, and all 173 browser tests passed with cached dependencies.

Verification includes CPU and NVIDIA Parakeet decoding, PDF extraction, speaker embeddings,
synthetic microphone and meeting workflows, loopback peer interoperability, and updater tests
using a private registry hive and disposable installations. The RTX 2060 decoded an 18.4-second
local recording in 0.36 seconds. The release desktop still needs a session with real input
devices, desktop hooks, tray, clipboard and a second physical peer. The GCC 13.3 Linux core build runs under WSL with an isolated toolchain, including the native
channel process and reconnect tests. The owner-run interactive suite and a signed, published upgrade on a disposable machine remain outstanding.

The local MCP session serves saved content. Optional title and settings changes need explicit
permissions. Its tool list excludes live dictation text, microphone control and action requests.
Host MCP has its own time-limited, read-only scope. Manual transcript cleanup in History,
explicit hook and action tests, and approved per-meeting external destinations are available.
Ordinary live dictation started on this computer, including cleanup profiles, can use Claude
Code, Codex, or OpenCode after the user approves the named runner in AI cleanup settings and
enables cleanup. The confirmation explains that transcripts and cleanup instructions go through
the CLI to its signed-in AI provider. Switching to an endpoint clears the approval. Persisted
startup restoration of external hooks and unattended sharing remain differences.

The recovered SSD checkpoint mapped 5,659 upstream tests. This branch maps 6,063 of the
7,034 kept upstream tests, with 275 explicitly pending and 696 still without a mapping.
The [parity report](parity-report.md) lists them by area. The separate feature inventory
also needs source and test evidence before a reviewer can certify complete parity. The
older plan's standalone benchmark CLI and comparative performance suite are outstanding.
The existing release health test covers a synthetic app lifecycle, and the real decode
measurements cover the installed Parakeet model. The older package counts and open-item
entries below describe their original integration checkpoints.

For the Windows Parakeet port, the current planning range is one to three working weeks to
reconcile the remaining behavior and qualify the release, plus scheduling for hands-on checks.
The range assumes two workers and uses the earlier implementation and review durations below.
Remote speech and meetings have the most unresolved test mappings. Spot checks there found
both working behavior awaiting verification and real desktop integration gaps. Full Linux
and macOS desktop capture would need separate estimates.

## Earlier November 18 baseline

This section describes that earlier checkpoint; the first section of this file supersedes its
counts and estimates.

The integration branch has landed 147 of the 204 numbered packages. Of the 57 remaining,
29 are in M5–M9 and 28 are in M10. The generated [parity report](parity-report.md) counts
upstream test functions separately.

Resume walkthroughs with M6.13, then wire M6.16. The saved M6.13 draft contains the runtime
header and an empty implementation file. Four other packages already have implementation
branches awaiting integration or further review (M7.18, M7.19, M8.4 and M8.5). Check those
branches and interrupted worktrees before assigning fresh implementations.

The last three inspected Claude run configurations allowed six workers with six build jobs each
(36 simultaneous jobs). The revised defaults allow eight build or test jobs across package
workers. For the two-worker estimate, use `concurrency: 2`, `jobs: 4`, `maxJobs: 8`.
Integrated packages use module checks and share one full gate per integration stage.
The previous journals include 102 launches just for cleanup. The reviewer now frees its own
finished cache after saving its report.

The available Claude workflow journals record 64 completed M5–M9 implementations with a median
of 75 minutes and 62 reviews with a median of 56 minutes, measured from first to last transcript
timestamp. Applying those medians to 57 packages gives about 124 worker-hours. Two continuously
occupied workers would take 62 elapsed hours at that rate. Allow **80–160 elapsed working hours**
for dependencies, integration, rework and M10 qualification, plus calendar waits for owner tests.
For Codex workers, use Sol 6.2 for bounded C++ ports and their tests, and Terra 6.1 for
reference-image comparisons and narrowly specified verification. Keep cross-module contracts,
concurrency changes and unresolved integration failures with the lead. The JavaScript wave
runner uses Claude model names. Measure the first Codex packages before revising the estimate.

## Waiting on the owner

- **Linux gate.** Runs on the owner's Arch machine (`ssh jed@jed`, GCC 16), at low priority
  inside `~/fiorivoce-port`: push `port/parity` there, then `run.sh configure|build|test`
  (the `linux-core` preset). Green on 2026-11-14: 1,399 tests, 10 skipped (Windows-only or
  needing the Parakeet runtime). The lead reruns it once per milestone. WSL on the Windows
  machine still lacks `nasm` and the ALSA headers. On 2026-11-19 the whole tree, the Qt modules
  included, was built with GCC 13 and Qt 6.10.3 (offscreen platform) in a Linux cloud container
  on `codex/cloud-development`, and its tests run there; jed's GCC 16 has not built it since
  2026-11-14. The tests that fail on that Linux tree for reasons that are not port defects are
  listed under "For the M10 sweep" ("On Linux, ..."). jed's run is the check with a second
  compiler; the `linux-core` preset there has not yet built the modules added since 2026-11-14.
- **The cloud branch on a real Windows session.** `windows-ci` and `windows-debug` build and
  pass on the owner's machine ("Windows build of the cloud branch" above), with every `win/`
  file of 2026-11-19 compiled and its unattended tests run, among them
  `tests/process/test_console_window.cpp`, the Auto Mode case of
  `tests/ui_controller/test_walkthrough_live_and_write_up.cpp`, `tests/update_native` over a
  private registry hive and disposable installs, and the 438 KB Word style embeds. What tests
  cannot show is left: the `src/update_native` restructure (installed transaction, health child,
  parent wait, the desktop's gate) in a real install and update, which belongs to the M3 demos
  below, and the `[interactive]` suite.
- **Interactive tests.** Every `[interactive]` test is written and has never run: the keyboard
  hook, injected keys, foreground-window reads, a real microphone recording, the real clipboard
  round trip, the tray icon, `--version` to a real parent console, the real Mouse 4 hook, the
  hotkey capture hook, the overlay's language chip clicked on the real window and the Upload
  tab's microphone check on a real device, and the update launch gate over the kernel's mutexes
  (`tests/update/test_gates.cpp`, "the kernel's mutexes gate a launch"), the rule dictated on
  the real microphone (`tests/ui_personalize/test_rules_page.cpp`, M4.12) and the transform
  that rewrites a real Notepad selection (`tests/dictation/test_command_interactive.cpp`, M4.13;
  on Windows 11 notepad.exe may open as a tab of a Notepad already open, and terminating the
  launcher may not close it), the two links opened through the real Explorer
  (`tests/process/test_explorer_open.cpp`, M5.1: they register a URL scheme under
  `HKCU\Software\Classes` and start the console helper `fv_process_child`, so a console window
  may flash), the three WASAPI loopback tests (`tests/audio/test_loopback.cpp`, M7.2: a tone
  through loopback, a quiet endpoint while the owner changes or unplugs the default output
  within 20 s, PortAudio's ADC times on microphone blocks; no unattended test reaches the COM
  paths of `win/wasapi_loopback.cpp`, which have never been seen capturing), and serving on
  0.0.0.0 (`tests/net`, M7.1 and M9.1's TLS server answering `ws_connect` on the LAN address),
  Snap's capture exclusion, screen grab, window edges and foreground forcing on Windows
  (`tests/ui_snap/test_interactive.cpp` through `fv_ui_snap_platform_helper`, M6.3), a real snap
  covering every screen (`tests/ui_snap/test_session_interactive.cpp` through
  `fv_ui_snap_session_helper`, M6.8), and the real `WH_MOUSE_LL` hook observing a click in
  another window (`tests/hotkeys/test_click_observer.cpp`, M6.4), and a 5 s meeting capture of
  the default microphone and loopback endpoint (`tests/meeting_capture/test_interactive.cpp`,
  M7.8), and the dashboard server answering on the machine's LAN address
  (`tests/meeting_web/test_server.cpp`, M7.13), and the storyboard copying a screenshot to the
  real clipboard when the host gives no `copyFiles` (`tests/ui_walkthrough/test_review_states.cpp`,
  M6.12), the meeting audio check hearing the real microphone and system audio
  (`tests/meeting/test_setup_check.cpp`, M7.14) and a 20-second meeting over the real microphone
  and loopback (`tests/meeting/test_engine_scripted.cpp`, M7.17), and the speech host shared on
  0.0.0.0 pairing and answering on loopback (`tests/remote_host/test_speech_host.cpp`, M9.6).
  `ctest --preset windows-interactive` runs them, on the owner's say-so.
- **M1 dictation demo.** Not run, since no worker starts the app: kp * in Notepad through the
  real hook, a real microphone, the real Ctrl+V and the clipboard back after 250 ms, adoption
  from the real `%LOCALAPPDATA%\OpenWhisper`, the real runtime and model loading at startup, the
  RuntimeSetup dialog and its real file pickers (`ui_setup/runtime_setup_dialog.cpp`; the tests
  replace them), Ctrl+H and Play, kp - playing Canceled.
- **Groq test.** The `[platform]` cloud speech test needs `FIORIVOCE_TEST_GROQ_KEY` and spends
  the owner's credit; it has never run.
- **Real HTTPS download.** Downloads are tested against a loopback server only; one fetch from
  the internet through Schannel and the system proxy needs the owner's permission. The same
  holds for the speaker model from huggingface.co (M7.5, `models/speaker_model.cpp`).
- **Dashboard notices (M7.6).** `tools/release/notices/04-dashboard.md` names React, React DOM
  and Scheduler with their versions and the MIT text but no copyright line: the line is the one
  in each package's `LICENSE`, which needs `npm ci` in `webui` (a download) to read.
- **Dashboard in a browser (M7.16).** The spec's manual check of the meeting dashboard in the
  owner's browser has not run.
- **Qt 6.11 kit.** References are rendered with Qt 6.11, the kit is 6.10.3 (amendment 17).
- **Symlink tests** skip without Developer Mode.

- **OpenAI speaker pass (M7).** OpenWhisper can label speakers after a meeting through an
  OpenAI model that retires on 2027-02-26. Kept by default (amendment 19); the owner may say
  to drop it.
- **Fetching two things from the internet.** Each needs the owner's yes, since workers and
  the lead download nothing unasked: the URL, size and SHA-256 of the `onnxruntime-win-x64`
  archive for the speaker-labelling component (M7; `models/data/onnxruntime_catalog.json`); the
  same three facts of a `pdfium-binaries` Windows x64 release for the knowledge folder's PDFs
  (M8). Until then both components ship unpublished and are found through
  `FIORIVOCE_ONNXRUNTIME` and `FIORIVOCE_PDFIUM`. (ONNX Runtime 1.29.0's C API headers and
  licence were vendored on 2026-11-15 with the owner's yes.)
- **OpenWhisper's own numbers (M10).** The performance table compares FioriVoce with
  OpenWhisper. Measuring OpenWhisper means running its `--workflow-smoke` and benchmark scripts
  under the isolation harness (`tools/perf/ow_probes/run_probes.py`, never run: lead decision
  5); without the owner's yes the column comes from its documents
  (`tools/perf/baseline-openwhisper.json`). `src/bench`'s reports (M10.4) have no OpenWhisper
  column, and the golden vectors of `benchmarks.meeting_mode.metrics` are not written, for the
  same reason.
- **Performance run on Windows (M10.3).** No FioriVoce number has been measured on Windows and
  `docs/port/performance-report.md` does not exist: `perf_report.py` refuses to write it from
  non-Windows numbers. The owner's run is the section "The owner's Windows run" of
  `tools/perf/README.md` (`run_perf.py --preset windows-release --report-to
  docs/port/performance-report.md`, then `check_budgets.py`), on a quiet machine, with the
  `FIORIVOCE_TEST_*` variables set for the `[decode]` case. The checklist audit left two lines
  on it (`docs/port/audit/a32-checklist-platform.json`): QA-04, startup and footprint
  (`startup_to_window_s` 0.4 s, `idle_rss_mb` 150 MB, `key_to_text_p95_ms` 300 ms in
  `tools/perf/budgets.json`, never measured on Windows), and QA-05, the time `fiorivoce.exe`
  takes to exit after Quit. On the Linux container, an optimised smoke run of 2026-11-19 was
  over five budgets that are set relative to OpenWhisper's documented numbers:
  `main_window_warm_ms` (43.6 ms against 42.7) and `history_first_100_cards_ms` (93.0 against
  91.8) by 1 to 2 %, and `meeting_state_runtime_write_ms` (7.8 against 6.4),
  `meeting_state_summary_patch_ms` (7.1 against 5.1) and `meeting_state_card_edit_ms` (8.2
  against 5.6) by 22 to 46 %; a Debug run was also over `history_refresh_100_ms`. OpenWhisper's
  numbers come from another machine, so these matter only if the Windows run repeats them; the
  three meeting state writes are the ones to look at first. For QA-05,
  `perf.quit while workers are busy`
  (`tests/perf/test_shutdown.cpp`) records `quit_transcribing_ms`, `quit_installing_ms`, their
  `_exit_ms` and `shutdown_s` as information only (OpenWhisper documents no shutdown bound);
  the busy dictation's decode is modelled as 1.5 s windows that cannot be interrupted, so the
  Windows run should compare `quit_transcribing_ms` with one real Parakeet window on that
  machine (the `[decode]` case's timing). Two parts of QA-05 no test measures and the owner
  checks by hand: the time from `main()` returning to the process ending (C runtime and Qt
  teardown), and a Quit while a history write is still pending.
- **Wake names (M7, M8).** How the product name is spoken in a meeting is the owner's to
  choose. So that M7 could go on, the lead set `fiorivoce` and `fiori voce`
  (`AppIdentity::wake_name`, `wake_name_spaced`) on 2026-11-16; other spellings are one edit in
  `base/identity.cpp`.
- **GitHub push.** GitHub refused `port/parity` on 2026-11-16: the branch now holds workflow
  files and the stored token lacks the `workflow` scope. The owner runs
  `gh auth refresh -h github.com -s workflow`; until then `port/parity` on GitHub is at f65f48b
  and jed has everything. The cloud sessions of 2026-11-19 pushed their own branches
  (`codex/cloud-development` and the `codex/cloud-*` branches merged into it) to GitHub through
  their own credentials; that does not change the owner's token.
- **Folder on jed.** At a5337d6 a test (`tests/models/test_speaker_adoption.cpp`) created the
  empty folder `~/.local/share/FioriVoce` on jed (2026-11-16 03:08), outside
  `~/fiorivoce-port`, and another read the real `~/.local/share/OpenWhisper` there (sizes, and
  a temporary copy of its database to count rows). Both tests are fixed and every test
  executable is now isolated (amendment 25). The lead removes nothing outside
  `~/fiorivoce-port`: the owner may `rmdir ~/.local/share/FioriVoce`.
- **M3.6's commit message.** Commit 5b23082 has no blank line after its first line, so its whole
  description is its subject. Rewording it rewrites history and needs a forced update of jed
  (and of GitHub once it has the commit): only on the owner's word, and only between runs.
- **A test that reads the live OpenWhisper folder by choice.**
  `tests/models/test_openwhisper_discovery.cpp` (`[platform]`) reads the folder named by
  `FIORIVOCE_TEST_OW_DATA` when that variable is set. Nobody sets it; it is the owner's to use.
- **Default ports (M5).** FioriVoce keeps OpenWhisper's MCP and History API ports (8767 and
  8766), so both apps cannot serve on the defaults at once.
- **Field help card colour (M2.1).** The help card's description is drawn in the secondary text
  colour. OpenWhisper draws it in the ordinary label colour, through a fault that leaves the
  card's colour tokens unresolved; the owner may ask for OpenWhisper's look.
- **Website (M1.16).** No FioriVoce website exists, so the About box carries no link.
- **Upload demo (M2.21).** Not run: three dropped files, then Groq with a 30 MB file, need the
  owner's files and Groq key. Fakes cover the queue badges, "About N chunks" and the Splitting
  stage.
- **M2 Settings, History and Scratchpad demos.** Not run, since no worker starts the app: the
  footer Stats button and Export history… writing `fiorivoce_history.md` (M2.27); M2.20's demo
  again over the real Downloads page (switch to API and Groq, open Downloads filtered to
  Parakeet); the Scratchpad's real focus and activation (present, the reveal that must not take
  focus, the on-top re-show), tested only through an activity probe, with no `[interactive]` test.
- **M2 demo (M2.28).** Not run, since no worker starts the app: live preview in Notepad; a
  3-minute dictation pasted about a second after stop; three files transcribed locally, then
  through Groq (a real key, splitting over 23 MB); the History calendar, an export and Stats;
  the CUDA runtime and Orukeet downloaded from Downloads, against the real catalog, pins, sizes
  and GPU probe, and the GPU runtime loading after a restart; first-run guided setup on an
  empty data folder; the consent, required-runtime and GPU dialogs clicked by hand; the modal
  export dialog; a theme change on a real palette; tray-menu placement.
- **Real signing (M3.3).** `tools/release/windows_signing.py` has never signed a file (no
  certificate exists, `signtool` is not on PATH) nor verified a really signed one (chain
  validation may go online). Injected inspections cover both.
- **M3 demos (M3.7, M3.10, M3.11).** Not run, since no worker starts the app or a setup: the
  OpenWhisper import against the real `%LOCALAPPDATA%\OpenWhisper` and the real Credential
  Manager names (the WinCred test uses test service names only; reading the real credentials is
  a manual check); the real restart after exit 73, the real `std::_Exit` watchdog, the named
  mutexes against a real Inno setup and an update through a loopback feed with an installed
  0.1.0; `packaging/windows/smoke-setup.ps1` (its Acceptance step against a `-Version 0.1.1`
  build, before FioriVoce is installed: it plants a file in the real `%LOCALAPPDATA%\FioriVoce`
  and relies on the silent uninstall prompt defaulting to No); `build-installer.ps1` with its
  default `windows` preset, `-Clean` and `-Version 0.1.1` (only `-Preset windows-ci` ran); the
  milestone demo written in `packaging/windows/README.md`.
- **CI workflows (M3.11).** `.github/workflows/ci.yml`, `release-build.yml` and
  `qualify-release.yml` (added 2026-11-19 for A34-15; both its jobs run only when the repository
  variable `FIORIVOCE_RELEASE_REPO` is set) start by hand only (`workflow_dispatch`, plus
  `workflow_call` on `ci.yml`) and have never run; they reached GitHub only on the `codex/*`
  branches.
  Unproven: aqtinstall Qt 6.10.3, the vcpkg clone at the baseline, choco Inno Setup 6.4.0, the
  signing secrets. Risks: `linux-core` on ubuntu-24.04's GCC 13; windows-2022's MSVC 14.4x
  against this machine's 14.51; the 0-pixel screenshot tests on Server 2022's fonts;
  `actions/cache@v4` by tag, not SHA. The 9 `src/engine` files the lint job's clang-format
  step failed on were formatted on 2026-11-16. Push and pull_request triggers mean changing `tools/release/tests/test_workflows.py`, which
  pins manual triggers. actionlint is not installed, so `test_actionlint_accepts_the_workflows`
  skips.
- **Real agent passes (M6.2).** No pass has run through the owner's real Claude Code, Codex or
  OpenCode: it spends the owner's credit, and the test isolation makes a real agent read as
  signed out. It belongs to the M6 demo, which the owner runs.
- **Live drawing on the real desktop (M6.10).** No test draws over real windows: capture
  exclusion, the foreground handed over and back and the real window list under live canvases
  rest on M6.3's seams and their `[interactive]` tests. It belongs to the M6 demo, which the
  owner runs.
- **Word files in Word (M6.14).** No worker can open Word: open the fixture `walkthrough.docx`
  and `write-up.docx` in Word and in LibreOffice and confirm neither asks to repair the file.
- **Fast judgments demo (M5.19).** Not run: Settings > Meeting Mode > Fast judgments, Test
  showing "{where} answered in {t} s." against the owner's server, with a real key.
- **M4 demo (M4.14).** Not run, since no worker starts the app (steps 1-8): a real OpenRouter
  key or Ollama and the model's actual cleanup output; the real keyboard hook firing profile,
  transform and Command Mode shortcuts; real selection reads (UI Automation and Ctrl+C) and
  pastes, rich text into WordPad; Slack and Outlook detected as the focused app; the 60 s
  rule-dictation cap and a rule dictated on a real microphone; learning from a real field
  re-read after a manual edit; History's elapsed cleanup time.
- **Leftover folder.** Gate runs before the `WindowRig` fix left an empty
  `%LOCALAPPDATA%\FioriVoce\components\asr-nvidia-cpu` in the owner's real per-user folder;
  nothing deletes it unasked, so the owner may remove it. The same folder holds
  `fiorivoce_settings.json` and `.recorded_audio-recovery` written on 2026-11-15 at 23:29 and
  23:30, while workers were testing: the owner's own run of the app, or a test started before
  every test executable was isolated (amendment 25). Not opened, not changed.
- **Installer choices (M3.11).** `packaging/windows/FioriVoce.iss` differs from OpenWhisper's:
  the uninstall prompt's corrected speech-model line; no LicenseFile and no SetupIconFile (the
  repository has no licence file or `.ico`, so the setup has Inno's default icon). The local
  Qt kit has no licence files, so
  `generate_notices.py` copies no Qt licence text: a build that ships needs
  `-RequireQtLicenses` with a kit that has them.
- **Channel and --api in a real terminal (M5.20).** Not run, since a test child has no console:
  `fiorivoce.exe --api` in a real terminal (it attaches to the parent console and passes
  `CONIN$`/`CONOUT$` to `fiorivoce-cli`), Ctrl+C or Ctrl+Break stopping a running `--api`, and
  `cli::update_in_progress` answering true (it needs the installed copy and a live setup or update
  mutex; `src/app/main.cpp` `forwardToCli`, `src/cli/win/console_interrupt.cpp`).
- **Decisions taken for the owner on 2026-11-19, to confirm.** Each is recorded as a deliberate
  difference (`docs/port/differences.md`, from `docs/port/audit/*.json`) or in its audit entry,
  and each can be turned back to OpenWhisper's behaviour:
  `a16-failure-reported-once` (A16-28): `fiorivoce-updater` stays silent when a rolled-back
  commit started the previous version again and wrote `apply_error.txt` for it to show, where
  OpenWhisper's helper always shows its box; a previous version that starts and dies before its
  window opens then shows nothing. `a28-delete-keeps-host-copy` (A28-40): deleting a meeting
  leaves the copy shared to the paired host (`RecordSync::record_deleted` is empty, and Past
  Meetings' `hostClearNote` stays empty), where OpenWhisper deletes it and M9.20's spec said to
  wire `on_deleted` to it. `a29-revoked-agent-reads` (A29-33): an agent whose access was revoked
  or whose pass was cancelled can no longer search past meetings or the knowledge folder.
  `a04-refused-stop-ends-preview` (A04-10): a stop refused for a busy job slot also ends the
  live preview. A24-42: a re-run that carries a fresh desktop approval runs whatever the
  meeting's stored `cloud_enabled` says. The consent-first automation records
  (`a19-confirmed-actions`, `a19-explicit-routing`, `a19-hooks-not-started`,
  `a19-live-api-not-served`, `a20-local-mcp-saved-content`, `a20-peer-history-session`,
  `a20-host-mcp-read-only`, `a21-live-cards-hidden`) were written as deliberate from the
  owner's commit 8cf2b8e; the owner has not confirmed each. The app's events now go to the
  process-wide `automation::bus()` when the context names no event sink
  (`ui_controller/controller_feature.cpp`; before, dictation events reached a null sink), as
  OpenWhisper's do. The client half of
  recording reservations is not ported (A17-25, A04-46), so a host holds an update while this
  computer is connected, as for any untracked client. README's "AI cleanup for dictation"
  paragraph now names the endpoint choice "API / local model" (OpenWhisper's tile label) and no
  longer mentions a typed agent model ID: the AI cleanup card hosts the shared agent picker
  (installed-agent discovery and its model catalog), with FioriVoce's staged Apply and named
  consent kept (`a13-cleanup-agent-apply-and-consent`).
- **What an OpenWhisper import tells the user.** `backup/import.cpp` collects `copied_keys` and
  warnings (keys not copied, settings that could not be read and were imported as defaults, a
  legacy history left out), but they reach only the log (`backup/coordinator.cpp`, "Import:
  ..."), because the app restarts as soon as the import is staged. Two ways to show them: delay
  the restart and show a summary first, or record the result in the staged settings and show it
  after the restart. OpenWhisper has no import to follow.
- **A meetings folder that cannot be made.** Past Meetings' "open meetings folder"
  (`ui_meeting/past_meetings_panel.cpp`) logs "Could not create the meetings folder <path>:
  <reason>" and opens nothing (difference `a31-meetings-folder-failure-logged`), where
  OpenWhisper's `os.makedirs` raises an unhandled exception in a Qt slot. Should the user see a
  message?
- **Update source.** The release identity names no update source (`{"kind": "none"}` in
  `src/update/data/release_identity.json`, difference `a32-no-update-source`): automatic
  checks never run and a manual check says the build has none. The owner chooses the FioriVoce
  repository or feed the updater asks.

## Lead work before a wave

| When | What |
|---|---|
| M8 wave 3 | List `sidecar/src/pi-integration.test.ts` and `sidecar-opencode/src/adapter.test.ts` in `tools/parity/classification.json`, or choose another ledger: M8.1's ported TypeScript tests have no PARITY.json entries until then |
| M10 | `tools/lint/qss_unused.txt` still lists 14 names that C++ now sets, which `lint.qss_names` does not flag: `#hostModelCombo`, `#meetingBindWarning`, `#meetingConsentDialog`, `#meetingLanguageCombo`, `#meetingSpeakerIdCombo`, `#pairRequestCode`, `#pairRequestDialog`, `#remoteHostSummary`, `#remoteModelInspector`, `#remoteModelList`, `#remoteModelNotice`, `#remoteModelTitle`, `#remoteServingLabel`, `#remoteServingRow`. Take them out (the M8 wave 2 and M9 waves 2, 3 rows of this table). `#meetingAudioConsentDialog` stays listed: no dialog of that name was ported (`test_meeting_audio_consent_dialog.py`'s rows in `tests/ui_meeting/PARITY.json` say where each case went) |
| Before M10.24 | Add `fv_dictionary_speech_contexts` to the settings keys (`settings/keys.inc`, frozen). M10.24 (dictionary speech contexts, a gain) has not been done: `engine::RequestOptions` has no `speech_contexts`, Parakeet's dictionary support stays `after` (`speech/parakeet_backend.cpp`) and no `docs/port/audit/gain-speech-contexts.json` exists |
| M10 wave 5 | Count the sweep handoffs in `docs/port/audit/*.json`; decide whether M10.25 gets a second package |

## For a named later package

| Item | Where | Who |
|---|---|---|
| `automation::signing::verify` throws `py::TypeError` for a v1 signature with non-ASCII characters (as `hmac.compare_digest` does): code checking received headers must catch it or read it as false (M5.12's MCP Events only signs, it verifies nothing) | `automation/signing.cpp` | M5.22 |
| `JudgeKeys::over(AppContext&)` and `over(JudgeEnvironment&)` have no test of their own. (The composition now provides one `llm::JudgeEnvironment` and `JudgeProvider` per process, `ui_controller/process_judge.cpp`, and `ui_controller/action_wiring.cpp` sets the automation port and the action hotkeys before `setup_hotkeys`.) | `ui_settings/typesafe_credential.cpp` | M10 |
| `remote_pairing_name` falls back to the host for a `host_name` that is not a string, where Python shows `str(host_name)`. (The record tabs' remote fields are ported.) | `ui_downloads/model_assignments.cpp` | M10 |
| `FakeProcess` hands over all its scripted output at once when a sink is installed; streaming tests that need output arriving over time must grow it | `process/testing/fake_launcher.h` | M5.11 |
| The `onnxruntime-cpu` entry lives in its own embedded catalog, `onnxruntime_catalog()`, not in `builtin_catalog()` (generator output, and `test_runtime_resolver` asserts it holds runtimes only): give `ComponentCoordinator::Options::catalog` a catalog with it, or move it into the generated one. `locate_onnxruntime(const EnvReader&, const ComponentStore&)` has no `Paths` parameter; `MeetingEngineOptions::onnxruntime_path` (M7.17) comes from it | `models/onnx_runtime.h`, `models/data/onnxruntime_catalog.json` | M7.19 |
| `SpeechServiceEngine::accepts_phrases()` is always false and `transcribe` drops phrases (right while Parakeet is the only local backend); a backend that boosts phrases needs a `SpeechService` `recognition_support(Slot)` accessor | `remote_host/host_engine.cpp` | M9.12 |
| A `SnapSession` with no parent calls `deleteLater` on itself once its outcome is emitted: pass a parent (Python's `ui_controller` passes `parent=self`) or hold it in a `QPointer` before reading `isActive()` or `captureRect()` later. `SnapToolbar::setCaptureDelay` called again with another chip leaves the earlier chip a child of the toolbar: set the chip once. M6.10's `QtSnapHandle` and `LiveAnnotations` already do both | `ui_snap/session.h`, `ui_snap/toolbar.cpp` | M6.13, M6.16 |
| `QtLiveDrawingPort` emits its port signals inline, as Python's direct Qt connections do: `arming` from `arm()`, `enabled_changed` from `set_enabled()` and `close()`, `interaction_changed` from `disarm()`, against `runtime_ports.h`'s note that ports do not emit inline unless a method says so. `fv::Signal` delivers through each subscriber's executor, so receivers are not re-entered | `ui_snap/live_drawing_port.cpp`, `walkthrough/runtime_ports.h` | M6.13 |
| The Meeting Mode half of `test_opening_a_studio_tab_resizes_the_window_in_one_move` (M6.15 ported the Walkthrough half only; it needs `MeetingModeTab` in the test window) | `tests/ui_walkthrough/test_tab_stage.cpp` | M7.20 |
| `OpenCodeDriver` and `HeadlessDriver` keep a raw `LoopbackMcpServer*`: the server must outlive every `run_pass` and the driver's first `close()`, as `agent_tasks.cpp`'s `Running` does. `Driver::start`'s tools default to an empty array (tool-free); Python's `None` meant the meeting tools, so pass them explicitly | `agents/detail/driver_impl.h`, `agents/drivers.h` | M8.4 |
| Meeting Mode tab, left for the main window: the title-bar close-button half of `test_launch_controls_are_named_and_keyboard_reachable` and `test_stage_changes_never_resize_the_window` inside `MainWindow` with `PageFit` (both ported against the tab in a plain widget). `setRecentMeetingsReader` keeps a raw pointer to its `Executor`, which the header does not say. (The presenter registers the tab, answers `aiDestinationWanted` and sets `setAiInsightsAvailable`.) | `ui_meeting/meeting_mode_tab.h`, `tests/ui_meeting/test_meeting_mode_tab.cpp` | M10 |
| M9.3's transcripts group frames by connection, so the order across connections is not recorded: steps where two connections overlap (pair_approval busy, hello_ready `busy_connections` and `busy_handshakes`, requests busy) are driven by hand, for example two `play_client` threads with separate bindings. `play_client` checks a refused upgrade by HTTP status only (`ws_connect` reports no body): check the 404 body with an HTTP client. M9.6's host side does both (`tests/remote_host/test_transcripts.cpp`) | `tests/support/remote_wire/scripted_peer.cpp` | M9.7 |
| `setup_deps()`, the production wiring of the meeting setup seams (`SpeechModelCache`, the coordinators, `speaker_model`, `load_priority`), compiles but no test drives it (the `test_meeting_setup.py` functions are ported against fakes) | `meeting/setup_check.cpp` | M10 |

## For the M10 sweep

- `net.a send that races the close leaves the client's queued_bytes at 0`
  (`tests/net/test_ws_client.cpp`) assumes the server cannot drain the client's 64 MB before
  `stop` posts its forced close. If it ever fails under load, replace its `HttpServer` peer
  with a raw socket that answers the upgrade and never reads.
- Tests that write the real Windows Credential Manager under test service names and delete
  again: `tests/credentials/test_wincred.cpp`, `tests/backup/test_import_credentials.cpp`. The
  registry reads of `update/win/platform.cpp` and `http/win/system.cpp` are sandboxed or read
  only. Decide whether the WinCred tests move behind `[interactive]`.
- Never reviewed or run, because they need `FIORIVOCE_OW_PYTHON` at configure time, which the
  gate does not set (amendment 25): the Python keyring child of `test_wincred.cpp`, the
  `remote_host` interop tests, `settings.keys_are_current`, `models.speech_catalog_is_current`,
  `hotkeys.keyboard_names_are_current`. The isolated scripts share `out/parity/ow-data`, so
  they cannot run in parallel as they are.
- Branch `keep/M4.10-review-hotkeys-tests` (dd2499f) holds five dialog-level tests of the
  Hotkeys page's profile and transform sources that never ran; M4.14 wired the sources in
  `ui_controller/personalize_wiring.cpp` with tests of its own. Port them or drop the branch.
- `ui_shell/shell_feature.cpp`: the Upload File placeholder is still registered when `record`
  is missing (never in the app, where `record` installs before `shell`). Removing it breaks
  `tests/ui_shell/test_main_window.cpp` "the upload tab builds its screen on first show", which
  tests that placeholder.
- `ui_widgets/speech_server_controls.h`: any new construction of `SpeechServerControls` must
  build it with the shared `http::Client` and the io strand, call `setKeyProvider` right after
  construction, fill the Backend combo with `populateBackendCombo(model_values())`, call
  `LocalEngineControls::setDictation` and connect `DictationLanguagesField::changed` to
  `syncLanguageField`, as `DownloadsFeature` and `RecordWidgets` do.
- `tests/ui_controller/test_m2_end_to_end.cpp` proves part of M2's acceptance through seams:
  case 3 calls `UploadPresenter::requestUploads`, not the drop zone; case 4 builds the Groq
  factory inside the test (the app's own entry is covered by
  `tests/ui_shell/test_bootstrap_api_engine.cpp`); case 6 sets `guided_setup_state` to
  `pending` instead of running Bootstrap's initialize on an empty data root. No
  `[interactive]` test covers the composed side-button hook (`make_mouse_hook_source`): every
  controller test passes a `FakeMouseHook`.
- `DownloadsPresenter`'s guided-setup signals (`modelDownloadStarted`/`Progress`/`Finished`,
  `componentProgress`, `componentInstallFinished`) stay unconnected on purpose:
  `GuidedSetup::install` connects `ModelRuntime` and `ComponentRuntime` itself, so a second
  connection would deliver every event twice. Drop the signals or document them.
- `tests/CMakeLists.txt` still holds a dead block that registers `fv_app_tests` from
  `tests/app`, guarded by a target that no longer exists.
- `ui_overlay/visual.cpp` `toQColor`/`hueFamilies`: to match OpenWhisper's pixels (Qt paints a
  `fromHsv` colour with 16-bit channels) it finds a uimath colour's HSV by reverse lookup in a
  copy of `hue_color`'s three (saturation, value) families from `uimath/overlay_look.cpp`, close
  to a second colour function. The clean fix is in uimath: `Rgba` or `GradientStop` carries its
  source `Hsv`. Ribbon still paints from `VoiceRibbon::shape()`.
- `tests/uimath/PARITY.json` counts 6 `test_recording_look.py` and 3
  `test_window_fit_and_title_bar.py` functions as ported through sim-level tests; the widget
  tests in `tests/ui_overlay` and `tests/ui_settings/test_fit.cpp` now port them too. Drop them or
  point them at the widget tests.
- `models/model_runtime.cpp`: while one runtime question is open, a download needing another
  runtime queues behind it and is asked only after it finishes (Python nested a dialog). A
  failed or canceled update of the loaded runtime leaves its released engines closed until the
  next reload, as in Python.
- `ui_shell/application.cpp`: Linux `setDesktopFileName` is not ported.
- `tests/credentials/test_wincred.cpp`: the WinCred tests ("reads UTF-8 blobs and moves an
  entry without a user name", "writes, reads, overwrites and deletes as keyring does") fail now
  and then in parallel runs and pass alone. Each uses its own service name
  (`FioriVoceTest.<process id>`), so they share no entry. On 2026-11-15 line 216
  (`backend.get(s, "K") == "mine"` right after `backend.set(s, "K", "mine")`) failed once.
  `WinCredBackend::set` and `get` hold no state and throw on every Credential Manager error but
  `ERROR_NOT_FOUND`; no race was found in them. Log what `get` returned before calling it fixed.
- Tests that fail now and then when the machine is loaded and pass alone:
  `download.cancel during a transfer keeps or discards the partial` (the partial file is
  still empty when the cancel lands),
  `dictation.test_a_second_press_soon_after_a_tap_keeps_recording` ("the hotkey dispatcher
  did not drain"), `focus.a fast read resets the miss count` and `db.readers are pooled, not
  kept per thread` wait on real time and need a load-proof wait; `cmake.feature_registry`
  builds a fixture and fails on file locks (LNK1104, LNK1168, C1041) under load. So do
  `ui_record.expanding the card tells the window and applies the stretch after
  the animation` (`tests/ui_record/test_quick_record_tab.cpp:263`), `ui_widgets.AnimatedProgressBar
  ticks only while shown and eases 18 percent per tick` (`tests/ui_widgets/test_settings_switch.cpp:136`:
  the bar was already full at its first grab, 299 and 299), `speech.an upload cancelled
  in flight is abandoned at once and never retried` (`tests/speech/test_cloud_backend.cpp:512`)
  and `ui_widgets.the hint shows on Enter only with a hotkey and an enabled button, and hides
  80 ms after Leave` (`tests/ui_widgets/test_buttons.cpp:256`, a timing check), and `ui_shell.a
  request that is not show calls nothing` (`tests/ui_shell/test_single_instance.cpp:142`:
  `SingleInstance::requestShow` returned false once while six workers were building; 15 of 15
  alone), and `ui_record.transcription tabs do not scroll at minimum height`
  (`tests/ui_record/test_upload_file_tab.cpp:1719`, once in M3.10's review gate; 3 of 3 alone),
  `ui_record.reference screenshot upload_progress_panel/batch light` (10 pixels, max channel
  delta 4, once in M4.3's gate), `ui_controller.M2 end to end: a long dictation decodes its
  windows while it is recorded` (`tests/ui_controller/test_m2_end_to_end.cpp:174`, 46 s waiting
  for incremental decodes), `hotkeys.test_mouse_hook_thread_installs_dispatches_and_unhooks`
  (both in M4.9's gates), and `base`'s "a moved timer handle keeps the timer..." (M4.13's),
  `base`'s "timers fire after their delay, in deadline order" and `process`'s "a clean exit with
  a helper holding stdout returns within the grace" (once each in M5.6's second gate, again in
  M5.7's), `remote.dropped and delayed datagrams` (`tests/remote/test_discovery_codec.cpp:318`,
  a loopback UDP timing check, in M5.3's and M5.8's gates; 5 of 5 alone),
  `credentials.CredentialStore over Credential Manager keeps a key under its service` (M5.8's
  gate) and `process.start_detached uses an independent process and reports failure`
  (`tests/process/test_detached.cpp:65`, the detached child had not yet written its cwd and
  arguments; M5.14's gate). In the M6 review gates of wave N1, once each under load and passing
  on rerun: `llm.a cancelled token ends the wait within 50 ms`,
  `ui_controller.test_profile_shortcuts_refresh_and_capture_preserve_standard_hotkeys`,
  `ui_personalize.test_dictating_shows_a_live_strip_and_a_red_stop`
  (`tests/ui_personalize/test_rules_page.cpp:91`, a `pumpUntil` timeout), and M6.3's reviewer
  saw `ui_record.reference screenshot upload_progress_panel/batch dark` fail in every run in
  one worktree (10 of 37440 pixels, max channel delta 4) while the other reviews' gates passed
  it. `agents`' "opencode timeout and cancel end the session"
  (`tests/agents/test_ask.cpp`) keeps Python's 2 s limit: an initialize slower than that under
  load or in Debug would send no `session/cancel` (3.3-3.6 s wall in every run so far). In
  one Catch process (not under ctest) `upload_queue/three light` and `upload_file_tab/queue
  light` differ by 52 pixels each, which suggests state left over between tests. The linear-time
  check of `tests/text/test_vocabulary.cpp` sits near its bound by design (ratio about 16
  against 20) and may need a rerun under heavy load or in Debug. In the M7 review gates of wave
  N1, once each under load and passing alone: `clipboard.cleanup drops the prefetch`
  (`tests/clipboard/test_temporary_clipboard.cpp:503`) and `ui_widgets.a field with help has a
  caption that opens its card and follows its links` (`tests/ui_widgets/test_engine_field.cpp:148`).
  In the M5 gates of wave N2, once each under load and passing alone:
  `ui_controller.M3 end to end: back up, restore and restart…` (M5.18's review), and
  `dictation.a cancel while recording throws the capture away`, which crashed with 0xc0000374
  (heap corruption) in M5.9's gate and passed 5 of 5 alone: a crash, not a timing check, so look
  at it before calling it flaky. In the M6 gates of wave N2, once each and passing alone:
  `process.a clean exit with a helper holding stdout returns within the grace`
  (`tests/process/test_run_once.cpp:548`, M6.6's review), the WinCred test above and `ui_shell.a
  request that is not show calls nothing` (M6.9's), and `ui_record.reference screenshot
  upload_progress_panel/batch dark`, then `... light` (M6.14's two gates, 10 pixels, max delta 4).
  In the M7 and M9 review gates of wave N2, once each under load and passing alone: the WinCred
  test above and `db.readers are pooled, not kept per thread` (M7.10's), `ui_record.reference
  screenshot upload_progress_panel/batch dark` (M7.12's), `llm.a cancelled token ends the wait
  within 50 ms` and `ui_widgets.AnimatedProgressBar ticks only while shown...` (M7.13's), and
  `automation.test_a_judge_that_never_answers_is_cut_off_at_the_timeout`
  (`tests/automation/test_routing_decide.cpp:757`, 1.98 s against a 1.9 s bound, M9.3's). In
  the step's Debug run, `automation.a filter slower than the stage returns the words after 3
  seconds` (`tests/automation/test_filters.cpp:453`: the filter started late and had 2.305 s of
  the 3 s left, against `limit >= 2.9`; 5 of 5 alone). In the M5 review gates of wave N3, run
  while five other workers built, and passing alone: `dictation.a combined batch whose model
  never answers says why in the status`, `automation.test_the_stage_budget_holds_and_skips_what_is_left`,
  `automation.test_setting_up_requests_is_not_charged_to_a_filters_time`,
  `automation.test_a_hanging_program_filter_is_stopped_at_its_limit`, `meeting_capture.TestRateChangeAndLargeGaps
  rate change preserves session origin and gap`, `meeting_capture.a block wider than a queue slot
  is kept whole`, `process.a helper holding the output open survives too`, `process.start_detached
  runs in the program's folder by default`, `recorder.near digital silence floor still ends
  early`, `ui_settings.the page starts one scan when none is cached, and none when one is`, two
  `llm` cancel and budget timing tests, `ui_controller` end-to-end tests, and several named above.
  Two failed alone as well: `process.a clean exit with a helper holding stdout returns within the
  grace` (M5.15's review, 1.68 s against `OUTPUT_GRACE_S + 0.5` = 1.5 s, under heavy load; it
  passed on M5.16's second rerun) and `remote.dropped and delayed datagrams` (1 time in 3 alone
  in M5.17's review, a 20 ms receive window): their bounds may need slack. In the M6 gates of
  wave N3, run while five other workers built, and passing alone or on rerun: `agents.closing
  fails what is still pending`, the automation program filter's `can_cancel` and `no_output`
  tests, `net`'s keepalive pings, `recorder`'s lead-in, `ui_widgets.a field with help has a
  caption that opens its card and follows its links`, `ui_controller.an OpenWhisper install's
  runtime and model are adopted before the first load` (`tests/ui_controller/test_engine_setup.cpp:132`,
  `startEngine()` false after 17 s; 43.6 s alone with a real engine load), five `ui_controller`
  M2 and M4 end-to-end tests, one of them a SEGFAULT (M6.12's review: a crash, so watch it),
  failures in `credentials` and `documents` (M6.15's review), and several named above. Two
  `ui_hotkeys` tests, "binding rows show, refuse, clear and explain shortcuts" and "capture
  applies and persists immediately", hung for over 40 minutes in M6.11's gate and passed alone;
  M6.10's gate had five test executables fail to start (BAD_COMMAND), which passed on rerun.
  The step's own gate after the M6 merges of wave N3, on a quiet machine, failed four that passed
  5 of 5 alone: `automation.test_a_judge_answering_in_slow_parts_is_cut_off_at_the_timeout` and
  `test_a_judge_that_never_answers_is_cut_off_at_the_timeout` (`test_routing_decide.cpp:733`
  and `:757`, 3.98 s and 3.99 s against 1.9 s), `process.a clean exit with a helper holding
  stdout returns within the grace` (`test_run_once.cpp:544`, 4.34 s against 1.5 s; about 1.43 s
  alone, close to its bound) and `ui_controller.M4: a dictated rule is polished by the model and
  saved` (`test_m4_end_to_end.cpp:500`, no review within 30 s). The first three are each about
  4 s late, which looks like one stall of the machine rather than three faults. In the M8 and M9
  review gates of wave N3, run while five other workers built, and passing alone or on rerun:
  `recorder.stop wait allows final storage checkpoint` and two recorder recovery tests,
  `ui_record.reference screenshot upload_progress_panel/batch dark` and `... light`,
  `ui_widgets.AnimatedProgressBar ticks only while shown...`, `cli.the installed self-test lists
  what the folder lacks and fails` (its child ran past 60 s), `meeting_capture.spool whole
  SpoolWriter runs` (58 s), the automation program filter, `log`'s diagnostic write, two `net`
  WebSocket tests, `remote.dropped and delayed datagrams`, `ui_downloads` Remove 600 ms,
  `ui_shell.a request that is not show calls nothing`, `clipboard.cleanup drops the prefetch`,
  `llm` cancel and judge-budget tests (1.92 s against 1.6 s once alone), `ui_controller` M2
  end-to-end, CPU-runtime offer and adoption tests, and `process.a clean exit with a helper
  holding stdout returns within the grace` (3.67 s to 4.15 s under load; 1.80 s alone in
  M9.6's run, and 1.44 s against its 1.5 s bound in the step's quiet gate, so its bound needs
  slack). One was a crash, not a timing check: `focus.the service's own deadline timers resolve a
  stuck read` ended with SEGFAULT in M8.2's review gate and passed alone; it passed in the
  step's own gate after the M8 and M9 merges of wave N3 (quiet machine, 10,332 tests, none
  failed). Seen once under load: look at it before calling it flaky.
- Flaky tests, 2026-11-19 (Linux container, GCC, Debug, often `-j 8` beside other builds).
  Made load-proof or fixed that day, from the list above or new: `download.cancel during a
  transfer keeps or discards the partial`, `process.a clean exit with a helper holding stdout
  returns within the grace`, the timer and pool shutdown tests of `tests/base/test_executor.cpp`,
  `ui_record.the copy buttons copy what is shown and say how it went` (the 1,500 ms reset timed
  from its restart, within `QTimer`'s accuracy), two `net` tests ("a 300 MB file is streamed
  without being read whole", "a server that stops closes the client with its 1012"), and
  `focus.a fast read resets the miss count`, which was a real ordering fault in the capture
  service (the miss count is now reset before the answer is seen), `ui_shell.a transient status
  reverts to ready after two seconds` (`tests/ui_shell/test_recording_relay.cpp`: Qt's coarse
  two-second single shot may read 1,899 ms on a finer clock), `net.the frame being written is
  still owned when on_close runs` (`tests/net/test_websocket.cpp`: a 16 MB frame, which loopback
  cannot absorb before `stop`) and two load-sensitive `ui_controller` tests (a single
  `processEvents` after a posted signal in `test_wiring`; the controller's `loaded_engine`
  trailing the speech service's Ready by one UI hop). Seen and not yet made load-proof:
  `meeting.test_rejected_key_is_named_in_the_offline_message` under `-j 8`,
  `meeting.test_remote_connection_update_keeps_backlog_and_chunk_error` (once, while the meeting
  tests were being edited), `ui_meeting.test_switch_to_and_from_meeting_mode_adjusts_height`
  (12 s under load), `remote_host.a thousand requests on one connection keep the reactor quick
  and the host's state flat` (`tests/remote_host/test_speech_host.cpp:1358`, under load; its
  slowest-reactor-callback bound is named under `remote_host` (M9.6) below), and, while other
  tests in the same directories were being edited, two cases of
  `tests/ui_controller/test_thread_hops.cpp` and `ui_controller.a judge that does not answer in
  time is published and the paste says why` (`tests/ui_controller/test_route_by_voice.cpp:116`):
  a full run on a quiet tree decides whether these three are flaky. One crash, not a timing
  check: `ui_meeting.Meeting Mode starts and ends through the desktop presenter` ended with
  SEGFAULT inside fontconfig once while other workers relinked, and passed on three reruns.
- ONNX Runtime (MIT) has no entry in `tools/release/notices/` yet: add one, from
  `third_party/onnxruntime/LICENSE`, before the `onnxruntime-cpu` component is published or
  the runtime ships with the app.
- Programs that a test starts as a child process (`fiorivoce-cli`, the helpers, `fv_process_child`,
  `tests/remote/fake_tailscale`) are not covered
  by the guard that keeps test executables from showing an abort or assertion dialog in a Debug
  build (`cmake/fv_test_no_dialogs.cpp`): give them the same guard when a test first aborts one.
- `dictation/streaming_runtime.cpp`: `cancel_session` of a recording whose preview start was
  refused still cancels the shared preview, which drops the earlier recording's pending
  finalize, as OpenWhisper's `StreamingTranscriber` does. (The refused recording no longer falls
  back to the earlier recording's kept text: difference `a04-refused-preview-no-fallback`,
  `tests/dictation/test_sweep_refused_preview.cpp`.)
- `tests/dsp/test_window_splitter.cpp:94`: GCC's `-Wdangling-reference` fires on a reference
  into a cache (a false positive); the only warning left in the Linux build.
- `tests/speech`: no Python transcript of a real speech fixture is committed, so real-decode
  parity rests on the tone fixtures. A committed speech recording would fix it; the specs'
  `tests/fixtures/audio/hello_44k1_mono.wav` does not exist, so the dictation end-to-end test
  and the Groq test use the looped tone.
- `audio/wav_metadata.h` has readers for LIST INFO `ICRD` and `ISFT` but none for the bext
  originator string, so no test reads it back. (The failing metadata stamp is tested now, through
  `audio::testing::exchange_origination_stamp`.)
- `recorder/detail/capture_ring.h`: no atomic peak or RMS meter in the device callback.
- `base/events.h`: `EventSink` has no `clear_replay()`; history clears it through a callback.
- `history/dictation_stats.cpp`: a list or dict in `stats_counts` is written as JSON, not as
  Python `repr`.
- `clipboard/temporary_clipboard.cpp`: `foreign_copies_visible()` is false on all Wayland.
- `uimath/particles.cpp`: `random_int` scales `random()` instead of rejection sampling.
- `ui_theme/font_scale.cpp` reads the literal key `"ui_theme"` instead of `keys::ui_theme`.
- `hotkeys`: the hook callback allocates on a claimed press (as Python does).
- `pyport`: no `\N{name}` escapes, no `re.ASCII`, no integers past 64 bits in JSON.
- `tests/ui_controller`: the device-switch test does not check the overlay caption (no
  accessor); the recording-storage failure is ported for `has_audio` False only; the hotkeys
  backfill test reads the map the UI is told, not the buttons' text.
- Stale pending rows: 271 `pending` rows in 26 `tests/*/PARITY.json` files name a function that
  another ledger marks ported, golden, dropped or other-platform (`base` 100, `settings` 25,
  `speech` 24, `agents` 18, `ui_downloads` 18, `process` 12, `dictation` 8, `ui_widgets` 8,
  `ui_overlay` 7, `ui_theme` 6, `ui_hotkeys` 5, `ui_walkthrough` 5, `agentapi` 4,
  `credentials` 4, `automation` 3, `clipboard` 3, `focus` 3, `log` 3, `ui_settings` 3,
  `ui_shell` 3, `appkit` 2, `history` 2, `ui_record` 2, `net` 1, `ui_controller` 1,
  `ui_history` 1). `parity_report.py` counts each function by its best row, so they are not open
  work and `--check` does not flag them, but a reader of one ledger sees them as pending. Among
  them: `tests/base`, `tests/appkit` and `tests/log` hold four rows with owner M1.16 that
  `tests/ui_controller/PARITY.json` lists as ported; `tests/speech` nine `test_audio_processor.py`
  preview rows (M2.3) ported in `tests/audio`; `tests/ui_hotkeys` (`TestHotkeySettingsNavigation`'s
  two), `tests/ui_theme` and `tests/ui_overlay` rows with owner M2, M2.19 or M2.24 ported in
  `tests/ui_settings`; `tests/ui_record` (`TestUiControllerHooks`' two) and `tests/ui_overlay`
  (`test_language_menu_is_filled_by_its_module`,
  `test_overlay_chip_and_shortcut_cycle_the_language`) with owner M2.28 ported in
  `tests/ui_controller`; `tests/history` (`test_source_reads_meetings_by_their_start` and
  `test_meeting_summaries_can_be_bounded_by_start`) ported in `tests/meeting_state`;
  `tests/agentapi`'s two M5.20 CLI rows ported in `tests/cli`. Drop them or point them at the
  ledger that ports them.
- `ui_widgets/local_engine_controls.cpp`: the Device and Language help texts drop the mentions
  of other engines, with no ledger entry. `ui_record/stats_display.cpp` shows `--` for a
  missing file size where Python would raise (its callers always pass one).
- `dictation/batch.cpp`: `source_name` follows Windows `ntpath` rules on every platform.
  `audio/file_preview.cpp`: `duration_formatted` copies `format_audio_duration` (a golden
  replay keeps them in step).
- `speech/upload_splitter.cpp`: more than 2 channels are averaged, not mixed by FFmpeg's layout
  matrix; compressed stereo can differ from PyAV by 1 LSB; `speech/detail/url.cpp` does not
  validate a bracketed IPv6 host.
- `models/component_details.cpp`: the component rows carry one source URL (the python.org link
  is dropped), and the CTranslate2 half of `test_no_gpu_runtime_offer_where_auto_would_not_use_it`
  has no counterpart (both recorded in `PARITY.json`).
- `hotkeys/win/mouse_hook.cpp`: destroying the manager still waits without a limit for a
  side-button callback that is running, as the keyboard hook's uninstall does, because the
  callbacks call into the owner and a thread let go would outlive it. Since 2026-11-19 it waits
  for that one callback only (events queued behind it are dropped) and logs a callback still
  running after 1 s.
- `uimath/sample_voice.cpp`: `sample_level` clamps to 0 where Python raises TypeError (5 golden
  cases skipped). The ocean school golden allows 1e-7 for GCC's drift; the Linux gate should
  confirm it. `tests/golden/ui_qt.overlays.recording_looks.json` grew by about 5.5 MB.
- `tools/lint/check_ownership.py` passes when no owned paths are given, so a worker's "passed"
  checks nothing.
- `tools/release/check_release_qualification.py`: OpenWhisper requires
  `source_qualification == 'passed'` and schema 3; FioriVoce writes schema 1, defaults to
  `not-run` for local builds and requires `passed` only with `--require-source-qualification`
  (README, `tests/release/PARITY.json`). Confirm against the ledger.
- Settings references (M2.19, M2.24): each page and Home is rendered as its body (pages at
  900 px, Home at 1000 and 700 px), the search palette over a plain host, not inside the
  1315x850 dialog with its rail, because OpenWhisper's rail cannot be cut to the M2 pages
  without rewriting its shell. The palette's "mic" query lists 2 results, not the spec's 3.
  The Personalize pages (M4.7, M4.8, M4.12: Dictionary, Snippets, Apps & styles, Commands,
  Profiles, AI cleanup, Learned rules) follow the same convention, and the Cleanup model card is
  rendered alone at 680 px. Render them in the dialog once every page is ported.
- `ui_settings/api_keys_section.cpp`: Save and Remove saved key run on the worker thread
  (`CredentialStore` refuses the GUI thread), so the store caption and status line are blank
  until the first read returns.
- `tests/ui_settings`: no test for Refresh refused while playback plays, nor for a refresh that
  comes back busy ("Microphones are in use…"); the navigation tests do not check that the
  dialog was refreshed, which Python asserts. No test covers `HomePage::apply`'s guard for a
  page whose create throws.
- `ui_downloads/downloads_page.cpp` `refresh(false)`: before the first scan a Settings window
  built hidden shows "0 of 2" on the rail and stats until it is shown and rescans; OpenWhisper
  reads the Parakeet inventory synchronously (deliberate: the GUI thread never touches the
  disk). Half of `test_component_profile_hides_duplicate_link_button` has nothing to test (no
  component has separate origin and source links).
- `ui_record/scratchpad.cpp`: no test drives a real `TranscriptionRuntime` through the
  `ScratchpadService` that `RecordFeature` installs.
- Omarchy (Linux look) variants not ported: `test_it_fits_narrow_windows_at_large_fonts` and the
  chart radius (`ui_history/stats_charts.cpp`), Home's 460 px variant of
  `test_home_rows_fit_narrow_windows_and_large_fonts`, and the compositor-managed Scratchpad
  window (`test_omarchy_gets_a_compositor_managed_window`, recorded dropped), and the Omarchy
  `ui_mode` variants of the two parametrized `test_settings_backup.py` tests (noted in
  `tests/ui_settings/PARITY.json`), the Actions page at 460 px (`test_automation_ui_actions_fit.py`,
  M5.13) and the agent picker's narrow layout (`test_cleanup_picker_labels_usage_and_narrow_layout`,
  M5.18).
- `sql/connection.cpp` `Connection::backup_to` copies 1024 pages per step and waits 250 ms
  while the source is locked; OpenWhisper's backup uses 256 pages and 50 ms (`backup.py:411`),
  so a cancel takes hold a little later. Give `backup_to` the two values as parameters.
- `backup/database_sanitizer.cpp` `snapshot_database`: with no source database it runs
  `Database::open_or_create`, which also creates the FTS index and the delivery trigger, where
  Python runs only `create_all` and the schema_version row (deliberate, harmless).
- `backup/coordinator.cpp` `remember_backup`: its lock fix has no deterministic test (no seam
  stalls `save_preferences` or the prune).
- `update/update_copy.cpp` keeps a third private copy of `format_size_bytes` (the others are
  `download/detail/format_size.h` and `history/format_utils.h`); a shared Qt-free home would
  remove it. The golden replay keeps them in step.
- `src/update` deliberate differences, stated in its headers: `detect_channel` needs a registered
  install whose folder is the running folder (Python: frozen means installer); without a GitHub
  repo a 429 or 403 from a loopback feed reads as a plain HTTP error and a release with no
  `html_url` gets an empty one; `consume_apply_error` leaves a file that is not UTF-8 in place
  and returns nothing (Python raises); `setup_command` reads `/ALLUSERS` from the startup probe,
  not from the registry at hand-off.
- `cli/win/installed_layout_win.cpp` `is_system_library` counts only the Windows libraries on
  its list (`kWindowsLibraries`, taken from `dumpbin /dependents` over the stage and the build
  folder on 2026-11-19) as found in System32; a Qt update that imports one more Windows library
  makes `--self-test --installed` report it until it is added. `cli/pe_imports.cpp` ignores
  delay-loaded imports, which `tests/cli/test_cli.cpp` "the executable imports no Qt library"
  now relies on.
- The engine-only `default` preset build of `src/cli/main.cpp`'s new dispatch (where
  `--workflow-smoke` and `--self-test --installed` print "not in the engine-only build") has
  not been built.
- `tools/release/generate_notices.py:32` keeps `PRODUCT = 'FioriVoce'` as a literal and the
  `notices/*.md` fragments name the product in prose: the release identity has no product key.
- Tests that compose `SetupFeature` (the Welcome page or the Backup page call
  `OpenWhisperImport::refresh()`) move only `LOCALAPPDATA`; an `OPENWHISPER_DATA_DIR` set in the
  environment would still point discovery at real data, which it reads and whose lease it
  probes. Clear it in those rigs (`fv::pin_per_user_folders` in `base/testing/map_env_reader.h`
  pins the per-user and home folders, not that variable).
- `tests/ui_controller/test_m3_end_to_end.cpp` part 1 drives the `BackupCoordinator` from the
  context, not the Settings > Backup page (M3.9 was not in M3.10's base): add the page step
  (create, then "Restore and restart"). Its `runUntilExit` arms a `QTimer::singleShot` with no
  context object (line 411; a scoped `QTimer` would keep the 60 s timer from outliving its run).
  `tests/ui_shell/test_bootstrap.cpp:111` "a pending restore is applied before the settings
  open" should also check that the restored settings hold the bundle's `selected_model`
  `parakeet`.
- `ui_controller` (M3.10), deliberate: `exit_for_update` and `restart_app` do not set
  `MainWindow`'s private `forceQuit_` (`QCoreApplication::exit` cannot be vetoed); the update
  dialog is shown application-modal instead of `exec()`; the updater has its own `CancelSource`,
  cancelled at `ShutdownStep::CancelRoot`; `UpdateServices`, not `UpdatePresenter`, owns the
  `UpdaterRuntime`. Nothing tests `src/app/main.cpp`'s `LaunchGateFeature` in a real GUI
  launch.
- `ui_settings/settings_backup.cpp` (M3.9), accepted: an out-of-range aware timestamp shows the
  stored text instead of raising; a `next_backup_at` before 1970 is unreadable; failures log
  with `error_with` where Python warns; the refresh after a schedule save is queued after the
  confirmation text.
- Generator checks `text.common_words_are_current`, `text.cleanup_prompts_are_current` and
  `llm.text_model_catalog_is_current` are registered only when `FIORIVOCE_OW_PYTHON` and
  `FIORIVOCE_OW_ROOT` are set at configure, so the gate never runs them; M4.1's reordered
  `tools/gen/dictionary_words.py --check` has never run. M4.4's golden vectors use curated
  inputs only (no trace of OpenWhisper's pytest runs).
- `src/text/detail/cleanup_rules.{h,cpp}` is M4.1's private copy of
  `resolve_transcript_cleanup_rules`; the public one is now `text/cleanup_prompts.h`: use it and
  delete the copy. `tests/ui_personalize/medium_prompt.h` is a hand-held copy of the Medium
  preset: read it from `fv_text`. `test_settings_key_and_config_default` (tests/llm) does not
  assert the prompt's "speech-to-text" wording, which `fv_text` now has.
- `src/llm/detail/url.cpp` is a 160-line copy of `src/speech/detail/url.cpp` (`urlsplit`,
  `urlunsplit`): move one `urllib.parse` port into `fv_pyport`.
- `llm`: `OpenAiCompatClient` parses a 2xx JSON body sent with a text content type for the
  Anthropic and Google adapters too (the SDK returns text there and fails); an HTTP-date
  `Retry-After` without a zone is read as UTC (`email.utils` reads local time); it leaves out
  `OPENAI_BASE_URL`, `OPENAI_ORG_ID`, `OPENAI_PROJECT_ID`, the `X-Stainless-*` headers and the
  idempotency key, and sends `FioriVoce/<ver>` as User-Agent (deliberate).
- `llm/local_servers.cpp` `detect_running_servers` waits for every probe (0.6 s connect and
  idle timeout per request), as Python's executor does, where the spec said it joins within
  0.6 s (deliberate).
- `llm/rewrite.cpp`: the swallowed-exception branch of `dictionary_block` has no test (the C++
  parser never throws).
- `text/cleanup_profiles.cpp` `conflict_options` passes each "" standard shortcut as null to
  work around `hotkeys` `truthy()`, which read "" as set; `truthy()` is fixed now, so the
  substitution can go.
- `ui_widgets/text_model_picker.cpp`: `get_profile(id, Json::object())` knows only the default
  built-ins, so `setProvider` ignores an endpoint the host did not pass through `setProfiles`
  (documented in `text_model_picker.h`, where OpenWhisper's picker reads the saved settings).
  `tools/parity/builders/a13.py` `_picker` replaces
  `text_model_picker.credential_status` for good, which a later builder in the same process
  would inherit.
- `ui_personalize/personalize_host.cpp`: with the dialog gone `setStandardHotkey` says "That
  shortcut could not be saved." (no OpenWhisper text). The message exists only while the
  Settings dialog is being torn down, so no user is expected to read it; keep it or drop it.
- `tests/ui_personalize`: no direct test of `_follow_saved_hotkey` (refresh keeps an unsaved
  draft but takes a shortcut saved elsewhere unless the draft changed its own) nor of duplicate
  naming (`<name[:65]> copy` for profiles, `<name[:72]> copy` for transforms, then ` copy 2`);
  M4.8's reviewer wrote both but could not commit them.
- `ui_personalize/commands_page.cpp`: call `CommandReadiness::setCheck` if the controller's
  credential sources ever differ from the context's.
- `dictation/transcription_runtime.cpp` `decode_path` treats `parakeet` and `remote` as taking a
  recognition context through `|| is_local_engine(engine)`, because `ParakeetBackend` does not
  declare `supports_recognition() const override { return true; }` (Python's
  `LocalSpeechBackend` does) and `speech::FakeScript` cannot set it. Add both, then drop the
  fallback.
- `tests/dictation`: `test_a_whole_formatted_snippet_gets_its_leading_space` checks the
  clipboard flavours, not what a `QTextEdit` shows after the paste; the raising half of
  `test_recognition_carries_language_and_phrases_when_steering_is_on` has no input to reach it.
  `test_a_transform_run_through_the_runtime_is_named_once` ports only the runtime half (not the
  History chip). `dictation/command_runtime.cpp` `run_transform` checks readiness on the `jobs`
  executor, so the focus probe and the slot claim can wait up to about 1.2 s behind another
  selection read.
- `ui_settings`: nothing makes the Settings window re-list the API keys page when a custom text
  endpoint is saved (`SettingsDialog::refresh()` would), and no test covers it. The text key
  check uses a default `CancelToken`, so closing Settings does not cancel it (as in Python), and
  says "Verification failed." without trying when the context has no HTTP client.
- `ui_personalize`: the `cleanup_rail_value` golden wrapper copies `_render_rail_values`'
  formula instead of calling OpenWhisper.
- `ui_record/command_readiness.cpp`, `record_feature.cpp`: `CommandHintReadiness` answers
  "ready" until its first io check lands, so at startup the hint can switch to "Set up Command
  Mode" a moment later. Without a credential store `makeTransforms` returns null and the
  Transform menu says "No transforms yet" even with transforms saved (partial startup only).
- `tests/ui_controller`: the backup gate's rule-dictation state is tested for Recording only
  (Python also checks "transcribing").
- `process` (M5.1): `win/pipes.cpp` `kCancelLimit` keeps cancelling a pipe thread for up to 10 s
  before detaching it (Python gives up after about 2 s), and a detached thread's sink may still
  be called, so later `on_line` users must not capture what they free; the longer limit is kept
  because a shorter one detaches more often. `win/system_env.cpp` `WindowsEnv::set` with ""
  removes the variable, as CPython's `_wputenv` does, but `os.environ` still answers '' for it,
  so a later `keep_existing=true` set takes effect here and not in Python; only tests set an
  empty value. `win/launcher_win.cpp` `make_pipe`: the child's pipe ends are inheritable until
  `CreateProcessW`, which a launcher that inherits every handle could catch; every launcher in
  this process passes a handle list or inherits nothing (`WinLauncher`, `app/main.cpp`
  `forwardToCli`, `start_detached`, the health child, `QProcess::startDetached` without
  redirection), so only a third-party one would. `RunOnceResult::exit_code` is an int, so a
  status of 0x80000000 or more is negative (Python shows it unsigned). `tests/process/test_run_once.cpp` "a clean exit with a helper holding stdout
  returns within the grace" asserts `< OUTPUT_GRACE_S + 0.5` from before both starts, not the
  acceptance's 1.1 s.
- `net` (M5.2, M7.1, M9.1), differences from uvicorn and websockets that remain: the 5 s
  keep-alive timeout covers the whole next header read; 64 KiB header limit; no
  `server: uvicorn`; a handler exception is answered 500 with `connection: close`; 100 Continue
  before dispatch; the reset-while-closing DEBUG text; absolute-form targets and `#` fragments
  are not split as httptools does (`detail/session.cpp`). On a keepalive timeout `on_close`
  reports 1011 (uvicorn 1005) and the socket is cut 1 s after the close frame; `server_stopping`
  drops queued frames not yet started; bytes before the 101 are dropped
  (`detail/ws_session.cpp`). WebSocket routes match the percent-decoded path, where websockets
  compares the raw one. `http_server.cpp` `close_listeners`: a theoretical race when a
  `Reactor` is stopped before its servers. The keepalive ping timeout has no 40 s test.
- `llm` (M5.4): the curl error to Python exception-name mapping is approximate
  (`detail/keep_alive.cpp`).
- `llm` (M8.1): a Chat stream that delivered a choice and then ended with `[DONE]` but no
  `finish_reason` is read as a transient dropped stream (`detail/stream_chat.cpp`,
  `tests/llm/test_sweep_stream_end.cpp`). What OpenWhisper's Pi sidecar does there rests on
  pi-ai, whose source is not in the OpenWhisper checkout, so the reading is unconfirmed; check it
  with pi-ai's source and then record a difference or change it. The ported TypeScript tests
  (`sidecar/src/pi-integration.test.ts`, `sidecar-opencode/src/adapter.test.ts`) have no
  `PARITY.json` entries: `parity_report.py` accepts only `tests/` files listed in
  `tools/parity/classification.json`.
- `agents` (M5.6): `detail/probe.cpp` copies `fv::run_agent_probe`, which always uses the
  process-wide `child_env()`; an env parameter there would remove the copy. The scan and lookup
  threads carry no thread names (base's helper is private), and a failed probe's debug log
  prints `argv[1]` only, where Python prints `cmd[:2]`.
- `remote_host/host_engine.cpp` `SpeechServiceEngine::stream` throws "The host's engine has no
  live stream" where Python forwards to the backend and fails with the runtime's own error. No
  user can reach the text: no FioriVoce client sends a "stream" request, and the host advertises
  `streaming` false for Parakeet TDT, which the runtime decodes offline only. Forwarding would
  need `streaming_recognize` in `src/engine`; later work, together with any streaming backend.
- `automation` (M5.3): `detail/pyvalue.cpp` `error_type` logs a non-pyport exception's type
  from `typeid().name()` (MSVC "std::runtime_error", GCC a mangled name; log text only).
  `tests/dictation/test_event_payloads.cpp`'s `test_completed_payload_for_a_file_and_for_a_job`
  keeps its Python name, so `ctest -R ^dictation\.event_payloads` selects only the three golden
  replays (it still runs under `-L dictation`).
- `agents` (M5.7) `agent_actions.cpp` `ask_headless`: as in Python, only the
  `fiorivoce-ask-*` folder is removed after an ask and the shared agent workspace
  (`%TEMP%\fiorivoce-agent-workspace`) stays, where the spec's acceptance says both go. Accept
  it or amend the spec.
- `ui_settings` (M5.19) `fast_judgments_page.cpp`: `typesafe_feature_tiles` (a dict in Python)
  is not a registered control name; the tiles are `featureTiles()`.
- `agentapi` (M5.5): the 307 Location rebuilds the query from the decoded pairs
  (`net::HttpRequest` has no raw query string), which can differ from Starlette's raw copy for
  unusual encodings (`history_routes.cpp` `route()`); `test_database_errors_do_not_leak_sql`
  uses a real SQLite error instead of Python's monkeypatched `check()`. `tools/parity/README.md`
  could list `record_transcripts.py` and the `transcripts/<name>.py` scenario format, now
  documented only in the script's docstring.
- `automation` (M5.8): a judge that never answers returns within one 20 ms poll of the budget,
  but `test_a_judge_that_never_answers_is_cut_off_at_the_timeout` allows Python's 0.4 s, not the
  acceptance's 50 ms.
- `dictation` (M5.14): no test asserts the Stats `target` count of an Action job
  (`_entry_stats_counts`' action name), though the code is written.
- `walkthrough` (M6.1), deliberate: `draft.json`'s
  `started_at` has no microseconds and drops Python's fold (a walkthrough started in the
  repeated autumn hour gets the daylight-time offset); `atomic_json`'s temporary file is
  `.<name>.<hex>.tmp`; the `ScreenshotNotSaved` test makes the root a file and checks only that
  the error code and message are set, not Python's "Permission denied" (`session.cpp`,
  `files.cpp`, `tests/walkthrough/test_session.cpp`).
- `agents` (M6.2): Python writes `system.md` and `instructions.md` in text mode (CRLF on
  Windows); the drivers write LF in binary while the stdin prompt gets CRLF, and the golden
  replay reads the files back in text mode so it cannot see it: match it or record it
  (`detail/claude_driver.cpp`, `detail/codex_driver.cpp` `write_text`).
  `tests/fixtures/transcripts/loopback_mcp.json` has its own shape (`{scenario, source_commit,
  normalize, tools, calls, listed, exchanges}`) written by `tools/parity/transcripts/
  loopback_mcp.py`, because M5.5's `record_transcripts.py` was not in its tree; M5.5 is merged
  now, so convert it or confirm the format. `loopback_mcp.cpp` `authorized()` sees the
  percent-decoded path, so `/%6Dcp/<token>` reaches the token check where OpenWhisper answers
  404 (harmless: the bearer token is still required).
- `tools/parity/builders/a07.py` `_walkthrough` sets
  `waveform_overlay._platform_takes_overlay_clicks` for the whole render process and never
  restores it (the overlay paints after the builder returns); `a23.py` `_Script` replaces
  `walkthrough_panel.time` for the whole process. No image is affected today; later a07 or a23
  scenes in the same run would inherit the patches.
- `tests/ui_overlay/test_walkthrough_controls.cpp`
  `test_a_capture_leaves_the_recording_overlay_as_it_was` hides and restores with
  `QWidget::hide`/`show`, because `ui_snap`'s `hideWindows`/`restoreWindows` were not in its
  tree; they are now.
- `ui_walkthrough` (M6.5): deliberate leniency where Python raises `KeyError`:
  `copyTipParagraphs` returns an empty list for an unknown format (the golden vector pins the
  `KeyError` case), `paintWalkthroughIcon` draws nothing for an unknown glyph, `snapModeLabel`
  returns the raw mode. `tests/fixtures/walkthrough/shot-1.png` and `shot-2.png` were drawn by an
  uncommitted PyQt6 script and cannot be regenerated (inputs, read by both renderers).
- `documents` (M6.7): Qt 6 cannot swap `QLocale::system()`, so the default paper's Letter branch
  is never reached on a machine set to A4 (`pageSizeFor(MeasurementSystem)` is tested on its
  own). Its review and M6.14's ran `windows-ci` only; the `documents` and `archive` tests first
  ran in Debug on 2026-11-19, and pass.
- `documents` (M6.14), deliberate: zip entries keep ZipWriter's 1980-01-01 date; the error's
  reason is the C++ exception text, not errno's; a core title over 255 characters is written and
  control characters are dropped, where python-docx raises; `w:tblBorders` follows `w:tblLook`,
  outside schema order, as `word.py` appends it.
- `walkthrough` (M6.6), deliberate: `set_retention` takes an int, so Python's refusals of `True`
  and `"30"` cannot be tested; a manifest start with a UTC offset is read in this computer's zone
  and drops fractional seconds (differs only for a walkthrough saved in another zone);
  filesystem error texts use the C++ wording, not `[Errno N]`; in `save_review` an empty `order`
  means the saved order, so an explicitly empty order with sections present is not refused
  (`walkthrough/library.cpp`). Load and delete's symbolic-link refusals are tested only by two
  `[platform]` tests that skip without Developer Mode (`tests/walkthrough/test_library.cpp:800`).
- `ui_settings` (M6.9), accepted: the Walkthroughs page registers in `SettingsFeature::provide`,
  not `install()` (the registry is read when the window is built, as for Backup and Fast
  judgments), and the row factory is `HotkeysPage::shortcutRow` in `ui_hotkeys/hotkeys_page.h`,
  not in `standard_hotkeys`, which gains only `walkthroughRows()`.
- `ui_snap` (M6.3, M6.8), deliberate: the excluded HWND lives in a `QHash` keyed by the
  top-level widget, not in the dynamic property `openwhisperSnapExcludedHwnd` (`lint.style_props`
  forbids a raw `setProperty` in `src/ui_*`). The `LabelEditor`'s placeholder colour (ink at
  60 % alpha) is set as Python sets it but untested. `tests/ui_snap/CMakeLists.txt` (M6.3's)
  links `Qt6::GuiPrivate` for `two_screens.h`'s offscreen screen configuration.
- `meeting_state` (M7.3): `FakeMeetingRepository` is simpler than SQL in two documented places:
  `replace_final_transcript` matches no speakers by time IoU, remaps no evidence and returns an
  empty `id_map`; `search_transcripts` is a substring match, not FTS5. Tests that need the real
  behaviour use `SqlMeetingRepository`. Deliberate, on corrupt input only: a non-string value in
  a text field reads as its default; evidence ids that are not text are dropped, or rejected as
  `invalid_evidence` when no `segment_exists` predicate is set (Python keeps them);
  `FinalizationState` coerce throws for a step given as a list of pairs, which Python's `dict()`
  accepts (`schema.cpp`, `patches.cpp`).
- `meeting_state` (M7.7): the SQL uses literal table and column names, not `db/schema.h`'s
  (correct today: the OpenWhisper fixture test reads every row back), and nothing ties them at
  compile time, since `schema.h` has no column arrays for the meeting tables. Accepted contract
  deviations: `MeetingStateStore::apply` and `undo` take `const std::optional<std::string>&`
  actor ids (a `std::string_view` argument does not compile), the constructor's sixth
  `SegmentPredicates` parameter, the extra `open_store` and `load_state` overloads,
  `PastMeetingQuery`, and a whole `MeetingState` copy per batch instead of Python's per-domain
  copy-on-write.
- `meeting` (M7.9): `PlatformInfo` reads an empty platform as the running one but keeps an empty
  machine, so `meeting_audio_support_copy({"linux"})` reads Linux as not ready (Python reads
  `machine=None` as the running machine; `platform_policy.h`). `run_agent_call`'s thread has no
  name (base's `set_current_thread_name` is private). The macOS Screen Recording checks are
  stubs that return false on macOS.
- `agentapi` (M5.9): the legacy-wire envelope errors for an invalid id on a request that is not a
  notification, and for a malformed `JSONRPCError.error`, are written by analogy with pydantic
  and are not in the transcript (`mcp_http.cpp` `classify_envelope`); `net::HttpServer` manages
  the keep-alive header of the held-open legacy GET stream, whose 15 s pings were checked only
  through `sse_ping_frame`, never over a socket (`history_server.cpp` `McpApp::serve`). No
  equivalent of uvicorn's "MCP stopped unexpectedly. Turn it off and on to retry." (the server
  never exits by itself), and JSON-RPC ids beyond 64 bits lose precision where Python echoes
  them (`mcp_runtime.cpp`, `mcp_http.cpp`).
- `automation` (M5.11): `http/curl_client.cpp`'s idle timeout is `CURLOPT_LOW_SPEED_TIME` in
  whole seconds, so a `post_signed` with a fractional timeout and no read limit (1.5 s) waits up
  to 2 s where httpx waits 1.5 s; deliveries (5 s) and anything with a deadline are unaffected.
  Hook requests now set `http::Request::log_request` to false, so
  `test_logs_never_carry_urls_event_data_or_secrets` could drop its "httpx" WARNING hold and run
  at DEBUG as Python's does (`tests/automation/test_webhooks.cpp`).
- `ui_remote` (M9.10): the client-unpaired reference shows two found hosts with the "Signed in to
  Tailscale" description, not the spec's Tailscale note, which OpenWhisper shows only when no
  host is found: accept it or amend the spec (`tests/reference/remote_engine_page/
  client-unpaired`). `forget()` while a search runs does not redraw, so the paired tile stays
  until the next change event, as in OpenWhisper. The macOS Keychain wording and "Open Keychain
  Access" are not ported (the button stays hidden, as on Windows).
- `meeting_state` (M7.11), accepted: `repair_meeting_state` returns the number of ops applied
  (Python's int) where the spec says void; the review, custom-report and fast handlers are added
  when `patches.cpp` first builds its registry, so the store need not call
  `register_meeting_ops`; `store.cpp`'s `invalidate_checks` hook defaults to `review_ops`'; every
  exporter takes an extra `Clock&`; `voice_feedback` has the `feedback` domain where store.py's
  `_OP_DOMAINS` lists `()`.
- `meeting_state/content.cpp` `summarize_meeting_content` reaches `SqlMeetingRepository`'s
  aggregate through `dynamic_cast` (line 131): give it a narrow port with an adapter, as lead
  amendment 26 has consumers do.
- `ui_meeting` (M7.10) `MeetingModeTab::setFinalization`: a malformed finalization whose steps are
  a string or a dict is stored and its steps read as empty; Python stores it and raises
  AttributeError on every layout pass.
- `tools/parity/README.md` has no section for `remote_wire.py`, `remote_peer.py` or the
  `transcripts/remote_*.py` scenario format (M9.3), nor for `meeting_ws.py` (M7.13); they are
  documented only in the scripts' docstrings. `record_transcripts.py --all` now skips them.
- `agentapi` (M5.12): the TLS half of the pinned-request test (SNI and the certificate checked
  against the host name under a pinned address) is not exercised: it rests on `http::Client`'s
  `pinned_address` (`CURLOPT_RESOLVE`), and `tests/agentapi` has no TLS server
  (`tests/agentapi/test_mcp_events.cpp:1108`). Not compared with OpenWhisper: `iso()` for years
  below 1000 and `parse_iso` with non-ASCII digits (Python's `\d` matches them), reachable only
  through a hand-edited subscriptions file (`mcp_events.cpp:603`). Each delivery worker wakes
  every 100 ms while a retry waits (`kLongestWait`, `mcp_events_delivery.cpp:25`), where Python
  sleeps until the job is due. Subscriptions saved by OpenWhisper carry ids hashed with
  `openwhisper-mcp-token` and secrets named `OPENWHISPER_MCP_EVENT_SECRET_*`: once imported, a
  refresh from FioriVoce creates a second subscription under the new id (`mcp_events.cpp:254`,
  the OpenWhisper import).
- `automation` (M5.15): the lease-routing tests check `channel.message`'s `origin` field, because
  M5.20's channel bridge was not in the tree; with it merged, they can check
  `channel_params(...)['meta']['origin']` as Python's do (`tests/automation`).
- `agentapi` (M5.16): a request body nested deeper than `py::loads` allows is answered 503
  "couldn't do that", where OpenWhisper's unhandled RecursionError gives Starlette's 500
  "Internal Server Error" (`live_api.cpp` `read_json_body`). A client that disconnects is
  noticed at the stream's next 0.5 s check, so `/v1/live/state` counts it until then
  (`sse_stream.cpp` `tick`). M5.9's `tests/agentapi/test_mcp_runtime.cpp` still declares
  stand-in live routes "until M5.16 installs them": use the real routes.
- `automation` (M5.20): a raw UTF-8-encoded surrogate (bytes ED A0 80) on a protocol line is
  answered -32700, where Python's `json.loads(bytes)` accepts it (`channel_bridge.cpp`
  `parse_message`).
- `ui_automation` (M5.21): `test_restore_is_opt_in_and_validates_port` is ported as a test of
  the settings values restore reads; the restore itself is covered by
  `tests/agentapi/test_mcp_runtime.cpp`. The `settings_mcp_page/running` reference shows the
  Claude Code command preview, not the spec's setup prompt preview, because `agent_prompt` names
  the computer it is made on (`tools/parity/builders/a21.py`).
- The Omarchy halves of the Hooks page's `test_sections_fit_narrow_columns_at_every_scale` (M5.17)
  and of the API & MCP page's parametrized `OPENWHISPER_UI` tests (M5.21) are not run: that look
  is not ported, so each runs once per font scale (100 and 130 %).
- `dictation` (M6.11), deliberate: `recover_walkthrough` reads the WAV header with its own parser
  in Python wave's order and error texts, not `walkthrough::read_pcm16_wav`; a missing file gives
  a C++ `filesystem_error` text instead of `[Errno 2] ...`, and a sample rate of 0 reports "bad
  sample rate" where Python raises ZeroDivisionError (`walkthrough_job.cpp` `wave_duration`). Its
  golden cases are an `M6.11` section of `tools/parity/cases/services.runtime.transcription.py`,
  not the spec's own file, because `golden_vectors.py` imports the module the file stem names.
- `ui_walkthrough` (M6.12): dragging a row by its grip is not tested end to end (a `QDropEvent`'s
  source needs `QDrag::exec`): `slot()`, a foreign drop's rejection and `moveRow` and Alt+arrows
  are, `dropEvent`'s start and end arithmetic is not. The debug-build `assert_not_gui_thread()` in
  the review's worker is inert in tests, which never mark a GUI thread; `showFolder` and copy read
  files on the GUI thread, as Python does (`walkthrough_review.cpp` `run()`,
  `tests/ui_walkthrough/test_review.cpp`).
- `ui_walkthrough` (M6.15): the storyboard header uses `walkthrough_review::today()`, the real date,
  which no builder or test can pin; the storyboard and tab references hold only while today is not
  2026-10-20 or 2026-10-21, and their wording changes in 2027 (`walkthrough_review.cpp`,
  `tools/parity/builders/a23.py`). The tab's `kMenuStyleSheet` copies `ui_widgets/context_menu.cpp`'s
  sheet with `@surface` for the translucent colour, as Python's `.replace` does; `ui_widgets` does
  not expose that sheet, so a change to it must be made in both places (`walkthrough_tab.cpp`).
- `ui_meeting` (M7.15): the card pills set `pillTone` through
  `ui::setStyleProperty` with a string literal, because `ui_theme/style_props.h` has no
  `prop::pillTone` (`past_meeting_item.cpp`). `PastMeetingsSource` (`ui_meeting`) and M7.16's
  `PastMeetingPages` are two narrow ports over the same reads, as amendment 26 allows.
- `meeting_web` (M7.16): no golden cases for `_filter_history_page` and `state_for`'s historical
  projection, because the case file `tools/parity/cases/meeting.web.api.py` is M7.13's; the
  recorded meeting_api transcript covers both (the digest-filtered pages, detail of `m_corrupt`,
  `m_untitled` and `m_attention`, the state polls). Adding them needs a harness run against
  OpenWhisper.
- `meeting` (M7.17): listeners run inline when their executor is current on the emitting thread,
  so an inline listener that calls `pause()`, `resume()` or `end()` while the engine holds its
  lifecycle lock (during an ASR adoption or stop, whose callbacks emit) would deadlock; the app's
  listeners are posted to the GUI executor, and the contract is that callbacks post
  (`engine.cpp` `emit`).
- `meeting_agent` (M8.2): malformed state shapes that Python rejects with AttributeError or
  TypeError are read as empty (a non-dict card item, participant or question, a question without
  an id, a non-string `asked_at`; `prompts.cpp` `render_state_compact`, `question_guidance`,
  `format_segment_line`); no golden vector covers them. `meeting_state::is_correction_author`
  (M7.3) returns false for a system/voice_command note whose data is a truthy non-object, where
  Python raises AttributeError.
- `meeting_intel` (M8.3): PDF text is compared with pypdf's by words and their order, not
  character for character, since PDFium lays out whitespace differently (`pdf_text.cpp`).
- `remote_host` (M9.6): `DiscoveryResponder::start` casts an out-of-range port to `uint16_t`
  where Python raises OverflowError (`discovery_responder.cpp`; M9.12 passes 47821). "A thousand
  requests on one connection" checks bounded host state, not process memory, and its
  slowest-reactor-callback < 5 ms check may be timing-sensitive in Debug or under load (it
  failed once under load on 2026-11-19); `test_a_slow_answer_still_beats_the_shutdown` is
  adapted to check only that `pair_closed` and its 4401 close arrive when `stop()` runs
  (`tests/remote_host/test_speech_host.cpp`).
- `meeting_agent` (A27-19): the built-in loop settles a pass cut at the output limit, as Pi's
  `turnFailure` does, and fails one stopped by a content filter or a refusal, on the reading that
  pi-ai maps those stop reasons to `error`. pi-ai's source is not in the OpenWhisper checkout,
  so that mapping is unconfirmed (`meeting_agent/agent_loop.cpp`, `llm/streaming.cpp`
  `StreamOptions::settle_length_stop`).
- `src/bench` (M10.4), not recorded as differences: the replay's "final" transcript equals its
  draft; the meeting session audio is resampled in windows of `SESSION_RESAMPLE_WINDOW_S`, as the
  live writer does, not in one pass over the meeting (`meeting_run.cpp`); live-preview clips are
  resampled with `dsp::fft_resample` (`live_preview.cpp`). OpenWhisper's
  `prepare_live_preview_corpus.py` is not ported. `fv_bench` includes
  `base/testing/manual_executor.h`, a test header, in production code (`live_preview.cpp`). Not
  compiled anywhere: `src/bench/win/peak_memory.cpp`.
- `ui_shell/bootstrap.cpp` `waitForRunningTasks`: Quit waits for the jobs strand's and the io
  strand's running tasks with no upper bound (a warning after the first budget), because they
  use the database and the context destroyed after them; OpenWhisper abandons its daemon
  threads (difference `a03-quit-aborts-http`). Cancelled when Quit starts: every HTTP transfer
  (`http::AbortableClient`), `RemoteEngineService::remote_model_request` (the shutdown token),
  the installed-agent scan and its probe's process tree, and EngineSetup's adoption of an
  OpenWhisper runtime and model. Still bounded but not cancelled at Quit, so an io task in one
  of them holds Quit for that long: the Tailscale CLI probe (4 s), the Windows network-profile
  reader (8 s, `remote_host/reachability.cpp`), the nearby-hosts scan (about 20 s),
  `pair_with_host` (connect plus a 10 s handshake), the meeting speech test (6 s plus 10 s),
  `RemoteConnection::connect`'s TLS open (`detail::open_engine_socket`, 6 s,
  `remote/connection.cpp`) and OpenCode's `list_models` ACP handshake (up to 20 s,
  `agents/acp.cpp` `list_opencode_models`); each needs a cancel token threaded through its API
  in `src/remote`, `src/remote_host` or `src/agents`. No `agentapi::McpEvents` is built in the
  app: whoever wires it must pass the root token in `McpEventsOptions::network`
  (`events_ext::system_network(CancelToken)`). `keyring_did_not_respond()` in `bootstrap.cpp` is
  unreachable on Windows, as in OpenWhisper, whose Linux Secret Service wrapper alone raises
  `CredentialStoreTimeout`.
- `remote`: the settings key `remote_client_history` is read only by `client_shares_history`
  (`remote/pairing_settings.cpp`), which nothing calls since the paired host's history tile
  became an explicit session (`a21-history-tile`); the key is in the frozen `settings/keys.inc`
  and `backup/sanitize.cpp`. Drop the function, and the key with the lead.
- On Linux (GCC 13, Qt 6.10.3, offscreen), tests that fail for reasons that are not port
  defects: the reference-screenshot widget trees and the goldens that replay label widths,
  recorded with Windows font metrics (Segoe UI) and the windows11 style (`ui_settings` 56,
  `ui_widgets` 47, `ui_record` 52, `ui_meeting` 46, `ui_overlay`, `ui_hotkeys` nine, `ui_snap`
  four toolbar trees and the `_layout`/`_wrap` goldens, `ui_walkthrough` six ready-card trees,
  `ui_shell`'s title bar and footer, the four `documents` layout and Word goldens, whose frames
  move 8-11 layout units with DejaVu Sans and whose `word/styles.xml` names another Normal
  font; counts from the first runs of 2026-11-19); ten `ui_snap` multi-screen session cases
  (`REQUIRE(screens.available())`: the offscreen platform of this Qt kit lacks
  `setConfiguration`); `ui_setup` "the update dialog's natural size is the references' size"
  (fonts). The pixel halves of the reference tests skip there. The Linux-only failures that
  were not fonts (the CLI's channel fixture, side-button wording, the privacy strip, fitDialog,
  Explorer's `/select`) were made portable that day. A Linux CI job would need Linux references
  or a tree comparison that ignores widths.
  The fonts also hide tree regressions there: the 36 reference failures of the first Windows
  run (2026-11-19, widgets the port gained that day) failed on Linux among the font ones. A tree
  comparison that ignores geometry and fonts would catch those on Linux too.
- On Windows, `fiorivoce.exe`, the CLI and the test programs declare no `longPathAware` in a
  manifest, where `python.exe` does: a data root, backup target or archive past MAX_PATH works
  in OpenWhisper and fails here unless the path is in the `\\?\` form (the long-path zip test
  uses that form). The manifest is a change to the root build, which is frozen.
- CTest hands a test the environment through the ANSI code page: with `%TEMP%` naming a folder
  outside it, `fv_test_isolation` finds no temporary folder and every test stops at start. To
  try such a folder, run a non-widget test program directly (the lease and detached helpers'
  tests passed that way on 2026-11-19).
