# open-items.md triage (stage 1b, 2026-11-19)

Scope: "Lead work before a wave" (4), "For a named later package" (23 after this pass), "For the M10
sweep" (161): 188 items. One line per item: first words | class | directories | size (S < 1 h, M a few
hours, L more). Grep the first words in docs/port/open-items.md.

Change made in this pass: removed the named-later row "`transcription_readiness_message` has no
remote branch" (closed: appcore/readiness.cpp has the "Connecting to {host}..." branch and
`request_remote_retry`; application_controller.cpp:359 calls `remote_.retry_now`, wired in
ui_remote/remote_feature.cpp:494-522; audit A03-15 now `fixed`). Recorded in
open-items-changes.md.

## Counts

| Class | Items |
|---|---|
| A parity | 38 |
| B defect | 33 |
| C test only | 33 |
| D later | 79 |
| E owner or Windows | 5 |
| Total | 188 |

Where an item mixes classes it is counted once, under the class of its most serious part, and the
other parts are named on its line.

## A: parity (user-noticeable difference from OpenWhisper)

Groups: A-G1 (Home and Settings composition: Home shortcut, Home cards, Learned rules item, level
combo, controlDestination, open("overview"), savePageDrafts order) is one Settings package, M-L.
A-G2 (OpenWhisper import: import credentials, `open()` refresh, import messages) is one ui_setup /
backup package, M. A-G3 (record tabs: setApiServer focus chain, upload_presenter unbuilt tab) is
one ui_record package, S-M.

1. `The first-run offer of Claude Code or Codex` | A | ui_personalize, ui_widgets (AgentPicker), docs/port/audit/a13 | M-L. OpenWhisper's cleanup card shows agent tiles with discovery and a model catalog; OpenWhisper_CPP has a runner combo and a free model-ID field. A worker either hosts `AgentPicker` (already used by the meeting intelligence page) in `CleanupModelSection` with the installed-agent scan and per-agent models, tested in tests/ui_personalize with `agent_picker::scanSeam` stubbed, or writes the difference record if the lead keeps the combo (a worker is writing that record now: recheck).
2. `The Downloads usage chip's "Meetings" half is never filled` | A | ui_downloads | S. Set `options.meetingModel` in `downloadsPageOptions` to `resolve_meeting_whisper_model(settings)`, "" when `resolve_meeting_asr_source` is remote; test in tests/ui_downloads that a model used by meetings shows the Meetings usage, and none with remote meeting speech.
3. `HistoryPanel::refreshPastMeetings refreshes the sidebar when its page` | A (unsure: may already agree) | ui_history, ui_shell | S. Check whether the sidebar's page always follows the Meeting Mode tab; if not, refresh when the main window's current tab is Meeting Mode as main_window.py does; test with the History feature and the Meeting Mode tab in one window, switching tabs and emitting a meeting change.
4. `MeetingAsrEngine::start_preview previews whenever accepts_files` | A (unsure: depends on whether Slot::Meeting ever holds a RemoteSpeechBackend) | meeting_capture, speech | S-M. Gate preview on the backend's `live_preview` capability or backend id (SpeechService accessor), as Python previews only with local Parakeet; test with a fake remote-like backend in Slot::Meeting that start_preview starts nothing.
5. `merge_usage reads the usage's JSON object members` | A (decision: the custom report's usage total differs) | meeting_agent, meeting_intel | S. Either reproduce Python's token-less total for custom reports or record a difference; a golden or unit test of the custom report's usage pins whichever is chosen.
6. `Paths::components_root() and speech_models_root() ignore OPENWHISPER_CPP_DATA_DIR` | A (unsure: depends on what OpenWhisper's override moves) + C (test isolation) | base, tests rigs | M. Read OpenWhisper's data-folder override, make `Paths` follow it, give the listed rigs all four variables; test in tests/base that the override moves (or does not move) components and speech models as OpenWhisper's does.
7. `Import credentials (amendment 25): copy the credentials named by api_key_env` | A (A-G2) | backup | S. Add speech endpoints' `api_key_env` and local servers' `<id>_api_key_env` to `legacy_credential_names`; extend tests/backup/test_import_credentials.cpp with both kinds.
8. `Home's shortcut after a hotkey save or a latch toggle` | A (unsure, A-G1) | ui_settings, ui_hotkeys | S. Save a shortcut and toggle the latch on the Hotkeys page with Home built and check Home's shortcut text updates; if it does not, call `HomePage::refreshShortcut` on the hotkeys change signal. Widget test in tests/ui_settings.
9. `ui_history/history_entry_dialog.cpp: "Use this version" runs on io` | A (unsure) | ui_history | S. Refresh the list (or apply the result) when the io result arrives after the dialog closed, as Python's signal does; test by closing the dialog before a gated io executor runs.
10. `ui_setup/runtime_setup_dialog.cpp: while the runtime is missing` | A + B (closing waits for a hash on the GUI thread) + E (real file pickers) | ui_setup | M. Show the model line while the runtime is missing as OpenWhisper does and make closing cancel the hash instead of waiting; widget tests for both; the real pickers stay for the owner's demo.
11. `ui_settings/settings_window.cpp open("overview") scrolls Home` | A (A-G1) | ui_settings | S. Scroll to top only on the explicit open path, not through settingsDestinationRequested; test both entry points.
12. `ui_record/transcription_tab_base.cpp setApiServer: the API fields` | A (A-G3) | ui_record | S. Chain the tab stops after the local fields with `QWidget::setTabOrder`; test with `nextInFocusChain` as the item says.
13. `ui_record/upload_presenter.cpp: applyStatus defers the engine message` | A (A-G3) | ui_record | S-M. Build the Upload tab when a transcript or error arrives for an unbuilt tab, as Python does; test with a never-shown Upload tab receiving `displayTranscript`.
14. `clipboard/temporary_clipboard.cpp: a lease whose marker reappears` | A (+ D: Wayland visibility) | clipboard | S-M. Restore instead of reporting Skipped when the marker reappears after a failed restore; test with the fake clipboard backend failing the first restore.
15. `ui_shell/bootstrap.cpp openDatabase: the database opens on io` | A | ui_shell, db, history, ui_history | L. Give `Database` an unopened state so startup paints first and a database that will not open leaves the app running with failing saves, as OpenWhisper; test in tests/ui_shell with a database path that cannot be opened (startup completes, a save reports the failure).
16. `log/diagnostics_report.h: GpuInfo::compute_capability is a string` | A (diagnostics report content) | log | S. Make it `std::optional<std::pair<int,int>>` written as `[major, minor]` or null; golden or unit test of the report JSON.
17. `appkit/settings_page_registry.cpp: controlDestination returns the first page` | A (A-G1; visible only when two pages list a control) | appkit | S. Let the last registration win as `merge_page_module` does; unit test with two pages listing one control.
18. `dictation/batch.cpp: source_name follows Windows ntpath` | A (only the locked-file sentence is user-visible; rest D) | audio, dictation | S. Map a file locked between header open and decode to the "isn't allowed" sentence; test with a reader seam that fails with a sharing violation.
19. `speech/endpoint_key_check.cpp: an empty key does not fall back` | A | speech | M. Fall back to the stored key on empty input, honour `OPENAI_BASE_URL` and HTTP-date `Retry-After`; unit tests over the fake HTTP client for each. (Upload splitter channel mixing and IPv6 validation: D.)
20. `ui_settings/home_page.cpp: choosing a voice on Home builds` | A (A-G1) | ui_settings | S-M. Keep the Voice model page unbuilt when a voice is chosen on Home and always show the Personalize and Meetings cards; Home widget tests.
21. `ui_record/scratchpad.cpp, scratchpad_saver.cpp: QString::fromUtf8 drops a leading BOM` | A (+ C: no test through a real TranscriptionRuntime) | ui_record | S. Keep a leading BOM and write it back, match `errors='replace'`; unit tests with a BOM file and a truncated sequence.
22. `ui_setup/openwhisper_import.cpp:146-161: open() calls refresh()` | A (A-G2) | ui_setup | S. Keep a folder the user picked when the dialog opens again; widget test choosing a folder, closing, reopening.
23. `backup/import.cpp (M3.7): every discovery` | A (the user is not told when keys were not copied; refusal where OpenWhisper loads defaults) + B (whole database copied to %TEMP% at every discovery) | backup, sql, ui_setup | M. Surface `copied_keys`/warnings in the import result, follow OpenWhisper on unreadable settings and empty history, count rows with an `immutable=1` open; tests in tests/backup.
24. `llm/detail/sdk_request.cpp sdk_send (verify_api_key, list_chat_models)` | A (retry timing; rarely visible) | llm | M. Move `sdk_send` onto `OpenAiCompatClient`'s retry (120 s, HTTP dates); tests with fake 429 responses carrying both forms.
25. `text/personalize_copy.cpp dictionary_page::engine_info` | A (unsure) | text, ui_controller | S. Check the Dictionary page's provider passes the selected endpoint for `api`; test the Dictionary page line with an API endpoint selected.
26. `ui_personalize/personalize_host.cpp: with the dialog gone` | A (texts and logger names) | ui_personalize | S. Use OpenWhisper's sentence and logger (`settings_dictionary`, "Couldn't save the dictionary"); test the failure path's message and log.
27. `ui_settings/settings_dialog.cpp savePageDrafts asks Profiles before Commands` | A (A-G1; question order) | ui_settings | S. Ask the page modules first and Profiles last; test with drafts on both pages and a recording host.
28. `ui_settings: Python's always-local "Learned rules" where-strip item` | A (A-G1) | ui_settings, ui_personalize | M. Add the Learned rules where item, fix the stale comment, re-list the API keys page when a text endpoint is saved; Home and Settings widget tests.
29. `ui_personalize/cleanup_page.cpp addCleanupHomeRows: Home's level combo` | A (A-G1; + B: journal folder per rule dictation, as OpenWhisper) | ui_personalize, ui_settings | S-M. Register the combo as `HomePage::control("transcript_cleanup_level")` and check the double reload; test search/destination reaches the combo and the cleaner reloads once.
30. `remote_host/host_engine.cpp SpeechServiceEngine::stream throws` | A (error text a client sees) | remote_host | S. Forward to the backend and report the runtime's own error; test with a fake backend that rejects streaming.
31. `ui_settings (M5.19) fast_judgments_page.cpp: key presence is read` | A | ui_settings | S. Show "No {key label} to test…" at once when the key read is pending and re-read keys set in `.env`/environment; widget test with a slow key reader.
32. `agentapi (M5.20): HistoryStore's expand_user` | A (`--database '~...'`) | agentapi, base | S. Use base's home rules (HOMEDRIVE+HOMEPATH, `~\`, trailing slashes); unit tests per rule.
33. `documents (M6.14): styles.xml carries only the template's docDefaults` | A (Word's Styles gallery differs) | documents | M. Embed the full template styles (or record the difference); golden check that styles.xml matches python-docx's template.
34. `meeting_capture (M7.8): the watchdog reads the default render endpoint` | A (no fallback loopback; centre channel) + B (COM/thread churn every second) | meeting_capture, audio | M (+ Windows to verify). Port `start_fallback_loopback`, mix surround channels, add a cheap default-endpoint query; tests over the fake audio backend; the WASAPI half needs Windows.
35. `ui_meeting (M7.15): openMeetingsFolder ignores a create_directories failure` | A (no error shown) | ui_meeting | S. Report the failure as OpenWhisper's raise does; test with an unwritable folder.
36. `meeting_intel (M8.3): PDF text is compared with pypdf's` | A (XLSX date cells reach the agent as ISO text) | meeting_intel | S. Parse "d" cells as openpyxl does; unit test with a dated XLSX fixture. (PDF word comparison: D.)
37. `update_native: after a healthy, rolled-back or recovered transaction` | A (disk use until the next start) | update_native | S. Remove the archive in the terminal cleanup as `_remove_transaction_payloads` does, or record a difference; test in tests/update_native that a healthy terminal cleanup removes the archive.
38. `remote and remote_host: the settings key remote_client_history` | A (the refusal names a switch the app does not show) + D (unread frozen key) | remote_host, tests/fixtures/transcripts, settings (lead) | S. Reword the host's refusal for the session grant and update the wire transcripts with the lead; test the refusal text.


## B: defects

Groups: B-G1 (unbounded joins at quit or destruction: `~InstalledAgents`, `waitForRunningTasks`, `ChannelServer`, `hotkeys/win/mouse_hook.cpp`, `~WalkthroughSession`, `~MeetingEngine`, `remote_host ~Impl`, `process win/pipes.cpp`) is one shutdown package with a test per wait, L, and goes with QA-05. B-G2 (raw pointers and lifetimes in widgets: `profileCard_`, `LiveCanvas::live_`, `mcp_widgets.h` uninitialised members) is one S-M package.

1. `describe() and is_installed() do not show a change staged for restart` | B | models, ui_downloads | S-M. Report a staged change (pending install or removal) in `ComponentInfo` and the Downloads row; test in tests/models/test_component_lifecycle.cpp that `describe` after `StagedForRestart` says so.
2. `~InstalledAgents joins a running known_agent lookup` | B (B-G1) | agents, ui_shell | S. Add a stop flag checked between probes (or release the instance off the GUI thread at quit); test that destroying during a slow fake probe returns within one probe.
3. `Remote engine page: a pairing request waiting on the host's owner` | B (thread outlives Settings; as in OpenWhisper) + C (Use-for-dictation wiring untested) | ui_remote | S. Cancel the request's `CancelSource` in `~RemoteEnginePage` or at quit; tests: destroying the page cancels the fake pairing call; RemoteFeature test that Use for dictation saves the remote selection and reloads.
4. `If the peer stops reading while a large frame is written` | B | net | M. Start `close_timeout` on a dropped client connection even while a frame is being written; test with a raw peer that never reads.
5. `~MeetingEngine joins the End, start and diarizer-start threads` | B (B-G1, latent) | meeting | S. Detect destruction on an engine thread (assert, or hand the destruction to another thread); test that destroying from a listener does not terminate.
6. `WsSession::send_text and send_binary count a frame` | B | net | S. Make `post` return bool; on false subtract the size and call `done(false)`; test with a stopped reactor.
7. `models/model_runtime.cpp: while one runtime question is open` | B (delete races a load) | models | M. Take the engine lock (or the runtime's equivalent) around the delete worker's recheck; test with a load started between the check and the delete.
8. `ui_shell/single_instance.cpp: the server name has no user part` | B (two users sharing a data folder collide) | ui_shell | S. Add the user to the server and lock names; unit test of the name.
9. `dictation/streaming_runtime.cpp: when a recording's preview start is refused` | B (previous recording's text as the final transcript, as OpenWhisper) | dictation | S-M. Clear the kept preview text when the new session's preview is refused; test in tests/dictation/test_streaming_runtime.cpp.
10. `ui_downloads/voice_model_page.cpp: each new page shows "0 of N"` | B (NVML probe on the GUI thread) + A ("0 of N" vs shared scan) + C (WindowRig without a fake CredentialStore) | ui_downloads, models | M. Move the runtime check off the GUI thread and share the last scan across windows; test with a slow fake probe that the GUI thread is not blocked.
11. `speech/speech_service.cpp: cancel_all and the destructor wait` | B | speech | M. Wait for a model load in `cancel_all`/destructor, report the last error for a Loading slot, emit readiness after releasing the mutex; tests with a gated fake load.
12. `models/engine_resolution.cpp: a fv_runtime_dir override is not checked` | B | models | M. Check the override for the runtime library and build, add a free-space check to the copy fallback, remove `<key>.lock` files; unit tests per case.
13. `ui_controller/engine_setup.cpp: the "speech engine crashed" CPU-runtime offer` | B (declined offer returns every start) | ui_controller | S-M. Compare with OpenWhisper, then mark the crash line handled when declined; test two starts after a decline.
14. `hotkeys/win/mouse_hook.cpp: destroying the manager waits` | B (B-G1; Windows) | hotkeys | S. Bound the wait for a side-button callback; test needs Windows (the hook) or a seam.
15. `process/posix/start_detached.cpp never reaps the child` | B (zombie until exit; harmless today) | process | S. Reap with a double fork or SIGCHLD handling; test that no zombie remains.
16. `cli/win/installed_layout_win.cpp is_system_library counts any DLL` | B (installed check passes wrongly; Windows) | cli | M. Use an allow-list of OS libraries; test with a fake System32 listing.
17. `ui_setup/openwhisper_import.cpp:249-253: every restore_staged emission` | B | ui_setup | S. The review's `ownsJob_` fix; test by posting a `prepare_restore` after `prepare_import` claimed the coordinator.
18. `ui_widgets/text_model_picker.cpp: get_profile(id, Json::object())` | B (wrong credential status between endpoints at one address) | ui_widgets | S-M. Key late answers by provider id, accept endpoints not passed through `setProfiles`; widget tests.
19. `ui_record/quick_record_tab.h: profileCard_ is a raw pointer` | B (B-G2) | ui_record | S. `QPointer` with guards; test deleting the card then refreshing.
20. `process (M5.1): win/pipes.cpp kCancelLimit` | B (B-G1 for the 10 s; env, handles, exit code) | process | M (Windows). Cut the cancel limit to Python's ~2 s, keep '' values, non-inheritable pipe ends, unsigned exit code; Windows tests.
21. `llm (M5.4): run_cancellable's fallback` | B (a judgment can be sent three times) + C | llm | M. Disable curl's resend for judgments or the pool's retry, resend on SSLEOF; tests with a fake transport failing twice.
22. `llm (M8.1): a Chat stream ending with [DONE]` | B | llm | M. Treat a stream without finish_reason as an error, mark 5xx `response.failed` transient, join cancelled transfers promptly, measure retry-after on the injected clock; streaming tests per case.
23. `models (M7.5): discover() offers OpenWhisper's speaker model` | B (26 MB hashed and logged at every start) | models | S. Remember a rejected digest (path, size, mtime); test two discoveries hash once.
24. `walkthrough (M6.1): ~WalkthroughSession waits up to 10 minutes` | B (B-G1; UI thread blocked) | walkthrough | M. Let `end()` hand pending writes to a writer that does not capture `this`, or never drop the last reference on the UI thread; test with a gated writer.
25. `archive::ZipWriter::create (M3.1) inherits minizip-ng's` | B | archive | S. Check the parent folder and path length in `create()`; tests with a missing parent and a long path.
26. `diarize (M7.4): DiarizerHost::create_participant returns std::optional` | B (rejection reason lost) | diarize | S. Return the reason; unit test of the message.
27. `ui_automation (M5.13): Route by voice's status is worked out` | B (a second secret requester cuts off the first) + A (status empty at first) | ui_automation | S-M. Token-gate all relay replies; test two requesters.
28. `automation (M5.20): after stdin closes, ChannelServer's destructor` | B (B-G1; exit waits 10-20 s) | automation | S-M. Detach or cancel reply threads at exit; test with an app that never answers.
29. `ui_automation (M5.21): in mcp_widgets.h, AdaptiveRow's` | B (B-G2) | ui_automation | S. Initialise the members to nullptr and prefix the class names; compile-only plus an existing widget test.
30. `ui_snap (M6.10): LiveCanvas keeps a raw LiveAnnotations*` | B (B-G2) | ui_snap | S. `QPointer` or delete retired canvases in `~LiveAnnotations`; test deleting `LiveAnnotations` directly.
31. `meeting (M7.14): SpeechModelLease::release pairs overlapping acquires` | B + C (engine stop not asserted) | meeting | S-M. Pair releases by token, not LIFO; narrow the polish/consolidation catches to Python's; test a predicate change between two acquires.
32. `remote_host (M9.6): if a reactor callback holds the last reference` | B (B-G1) | remote_host | S-M. Release the pool off the reactor thread; test a model switch running while SpeechHost is destroyed.
33. `ui_shell/bootstrap.cpp waitForRunningTasks: Quit waits` | B (B-G1, QA-05) | ui_shell | M. Bound the wait (detach or force-exit after a limit, logged) and pin it with a test that a hung io task does not hold Quit forever.

## C: test only

- `JudgeKeys::over(AppContext&) and over(JudgeEnvironment&) have no test` | C | tests/ui_settings | S
- `The Meeting Mode half of test_opening_a_studio_tab_resizes` | C | tests/ui_walkthrough, ui_meeting | S-M
- `Meeting Mode tab, left for the main window` | C (+ D: header comment) | tests/ui_meeting, ui_shell | S
- `M9.3's transcripts group frames by connection` | C | tests/support/remote_wire | M
- `setup_deps(), the production wiring of the meeting setup seams` | C | tests/meeting | M
- `net.a send that races the close leaves the client's queued_bytes` | C | tests/net | S
- `Never reviewed or run, because they need OPENWHISPER_CPP_OW_PYTHON` | C (needs OPENWHISPER_CPP_OW_PYTHON at configure) | tests/credentials, remote_host, settings, models, hotkeys | M
- `tests/ui_controller/test_m2_end_to_end.cpp proves part of M2's acceptance` | C | tests/ui_controller | M
- `Tests that fail now and then when the machine is loaded` (the long list) | C | many tests | L
- `Flaky tests, 2026-11-19` | C (a worker is load-proofing some: recheck) | tests/ui_shell, net, meeting, ui_meeting, ui_controller | M
- `tests/speech: no Python transcript of a real speech fixture` | C (a speech recording to commit) | tests/speech, tests/fixtures | S-M
- `audio/wav_metadata.h has readers for LIST INFO` | C | audio | S
- `tests/ui_controller: the device-switch test does not check` | C | tests/ui_controller | S
- `Settings references (M2.19, M2.24): each page and Home` | C | tests/ui_settings, tools/parity/builders | L
- `tests/ui_settings: no test for Refresh refused while playback` | C | tests/ui_settings | S
- `backup/coordinator.cpp remember_backup: its lock fix has no` | C | backup | S
- `tests/backup/lease_helper/main.cpp, tests/process/detached_helper` | C (Windows non-ASCII temp) | tests/backup, tests/process | S
- `The engine-only default preset build of src/cli/main.cpp` | C (a build check) | cli | S
- `Tests that compose SetupFeature (the Welcome page or the Backup` | C | tests/ui_setup, ui_settings | S
- `tests/ui_controller/test_m3_end_to_end.cpp part 1 drives` | C | tests/ui_controller, tests/ui_shell | S
- `Generator checks text.common_words_are_current` | C (needs OPENWHISPER_CPP_OW_PYTHON / OW_ROOT; group with the item above) | tests/text, llm, tools/gen | S
- `llm/rewrite.cpp: the swallowed-exception branch` | C | llm | S
- `tests/ui_personalize: no direct test of _follow_saved_hotkey` | C | tests/ui_personalize | S
- `tests/dictation: test_a_whole_formatted_snippet_gets_its_leading_space` | C (+ A?: 1.2 s readiness wait in run_transform) | tests/dictation | S-M
- `tests/ui_controller: the backup gate's rule-dictation state` | C | tests/ui_controller | S
- `automation (M5.8): a judge that never answers returns` | C | tests/automation | S
- `dictation (M5.14): no test asserts the Stats target count` | C | tests/dictation | S
- `tests/ui_overlay/test_walkthrough_controls.cpp` | C | tests/ui_overlay | S
- `documents (M6.7): Qt 6 cannot swap QLocale::system()` | C (+ E: Debug run on Windows) | tests/documents | S
- `automation (M5.15): the lease-routing tests check` | C | tests/automation | S
- `ui_walkthrough (M6.12): dragging a row by its grip` | C | tests/ui_walkthrough | M
- `ui_walkthrough (M6.15): the storyboard header uses` | C (references break by date) | ui_walkthrough, tools/parity/builders | S
- `meeting_web (M7.16): no golden cases for _filter_history_page` | C (needs the OpenWhisper harness: lead decision 5) | tools/parity/cases, tests/meeting_web | S

## D: later (gain, cleanup, refactor, tooling, lint, documentation, recorded deliberate choices)

- `M8 wave 3 | List sidecar/src/pi-integration.test.ts` | D ledger | tools/parity | S
- `M10 | tools/lint/qss_unused.txt still lists 14 names` | D lint | tools/lint | S
- `Before M10.24 | Add fv_dictionary_speech_contexts` | D gain (key in frozen keys.inc: lead) | engine, speech, text, dictation, ui_personalize, settings | L
- `M10 wave 5 | Count the sweep handoffs` | D lead bookkeeping (recheck) | docs/port/audit | S
- `automation::signing::verify throws py::TypeError` | D latent (no verifier) | automation | S
- `remote_pairing_name falls back to the host for a host_name` | D (corrupt settings only) | ui_downloads | S
- `FakeProcess hands over all its scripted output at once` | D test tooling | process/testing | S-M
- `SpeechServiceEngine::accepts_phrases() is always false` | D (right while Parakeet only) | remote_host, speech | S
- `A SnapSession with no parent calls deleteLater` | D API caution | ui_snap | S
- `QtLiveDrawingPort emits its port signals inline` | D contract note | ui_snap, walkthrough | S
- `OpenCodeDriver and HeadlessDriver keep a raw LoopbackMcpServer*` | D contract caution | agents | S
- `ui_shell/shell_feature.cpp: the Upload File placeholder` | D | ui_shell | S
- `ui_widgets/speech_server_controls.h: any new construction` | D caution | ui_widgets | S
- `DownloadsPresenter's guided-setup signals` | D | ui_downloads | S
- `tests/CMakeLists.txt still holds a dead block` | D | tests | S
- `ui_overlay/visual.cpp toQColor/hueFamilies` | D refactor | ui_overlay, uimath | M
- `tests/uimath/PARITY.json counts 6 test_recording_look.py` | D ledger | tests/uimath | S
- `ui_shell/application.cpp: Linux setDesktopFileName` | D (Linux) | ui_shell | S
- `ONNX Runtime (MIT) has no entry in tools/release/notices/` | D (before publishing the component) | tools/release | S
- `Programs that a test starts as a child process` | D tooling | cmake (frozen: lead), tests | S
- `tests/dsp/test_window_splitter.cpp:94: GCC's -Wdangling-reference` | D | tests/dsp | S
- `recorder/detail/capture_ring.h: no atomic peak or RMS meter` | D | recorder | M
- `base/events.h: EventSink has no clear_replay()` | D | base | S
- `history/dictation_stats.cpp: a list or dict in stats_counts` | D (targets the app never writes) | history | S
- `uimath/particles.cpp: random_int scales random()` | D | uimath | S
- `ui_theme/font_scale.cpp reads the literal key` | D | ui_theme | S
- `hotkeys: the hook callback allocates on a claimed press` | D (as Python) | hotkeys | S
- `pyport: no \N{name} escapes` | D | pyport | M
- `Stale pending rows: 271 pending rows` | D ledger | tests/*/PARITY.json | M
- `ui_widgets/local_engine_controls.cpp: the Device and Language help texts` | D (record in the ledger) | ui_widgets, ui_record | S
- `models/component_details.cpp: the component rows carry one source URL` | D (recorded) | models | S
- `uimath/sample_voice.cpp: sample_level clamps to 0` | D | uimath, tests/golden | S
- `tools/lint/check_ownership.py passes when no owned paths` | D tooling | tools/lint | S
- `tools/release/check_release_qualification.py: OpenWhisper requires` | D (confirm against the ledger) | tools/release | S
- `ui_settings/api_keys_section.cpp: Save and Remove saved key run` | D deliberate (store refuses the GUI thread) | ui_settings | S
- `ui_downloads/downloads_page.cpp refresh(false): before the first scan` | D deliberate | ui_downloads | S
- `Omarchy (Linux look) variants not ported` | D (Linux look) | ui_history, ui_settings, ui_automation, ui_widgets | M
- `sql/connection.cpp Connection::backup_to copies 1024 pages` | D (cancel a little later) | sql | S
- `backup/database_sanitizer.cpp snapshot_database` | D deliberate | backup | S
- `update/update_copy.cpp keeps a third private copy` | D | update, download, history | S
- `src/update deliberate differences, stated in its headers` | D (record as differences) | update, docs/port/audit/a16 | S
- `tools/release/generate_notices.py:32 keeps PRODUCT` | D | tools/release | S
- `ui_controller (M3.10), deliberate: exit_for_update` | D (+ C: LaunchGateFeature untested) | ui_controller, app | S
- `ui_settings/settings_backup.cpp (M3.9), accepted` | D | ui_settings | S
- `src/text/detail/cleanup_rules.{h,cpp} is M4.1's private copy` | D | text, tests/ui_personalize, tests/llm | S
- `src/llm/detail/url.cpp is a 160-line copy` | D | llm, speech, pyport | M
- `llm/local_servers.cpp detect_running_servers waits` | D deliberate | llm | S
- `text/cleanup_profiles.cpp conflict_options passes each ""` | D | text | S
- `ui_personalize/commands_page.cpp: call CommandReadiness::setCheck` | D caution | ui_personalize | S
- `dictation/transcription_runtime.cpp decode_path treats parakeet` | D refactor | dictation, speech | S
- `net (M5.2, M7.1, M9.1), differences from uvicorn` | D (+ C: keepalive 40 s test) | net | M
- `agents (M5.6): detail/probe.cpp copies fv::run_agent_probe` | D | agents, base | S
- `automation (M5.3): detail/pyvalue.cpp error_type logs` | D | automation | S
- `agents (M5.7) agent_actions.cpp ask_headless` | D (accept or amend the spec) | agents | S
- `agentapi (M5.5): the 307 Location rebuilds the query` | D (+ README) | agentapi, tools/parity | S
- `agents (M6.2): Python writes system.md and instructions.md` | D (match or record) | agents, tools/parity | S
- `tools/parity/builders/a07.py _walkthrough sets` | D tooling | tools/parity/builders | S
- `ui_walkthrough (M6.5): deliberate leniency` | D | ui_walkthrough | S
- `walkthrough (M6.6), deliberate: set_retention takes an int` | D | walkthrough | S
- `ui_settings (M6.9), accepted` | D | ui_settings | S
- `ui_snap (M6.3, M6.8), deliberate` | D | ui_snap | S
- `meeting_state (M7.3): FakeMeetingRepository is simpler` | D | meeting_state | S
- `meeting_state (M7.7): the SQL uses literal table and column names` | D | meeting_state, db | M
- `meeting (M7.9): PlatformInfo reads an empty platform` | D (macOS stubs out of the cut) | meeting | S
- `agentapi (M5.9): the legacy-wire envelope errors` | D | agentapi | S
- `automation (M5.11): http/curl_client.cpp's idle timeout` | D | http, tests/automation | S
- `ui_remote (M9.10): the client-unpaired reference shows two found hosts` | D (accept or amend the spec) | ui_remote, tests/reference | S
- `meeting_state (M7.11), accepted` | D | meeting_state | S
- `meeting_state/content.cpp summarize_meeting_content reaches` | D refactor | meeting_state | S
- `ui_meeting (M7.10) MeetingModeTab::setFinalization` | D (corrupt input) | ui_meeting | S
- `tools/parity/README.md has no section for remote_wire.py` | D docs | tools/parity | S
- `agentapi (M5.12): the TLS half of the pinned-request test` | D (MCP Events not mounted: a19-hooks-not-started) | agentapi | M
- `agentapi (M5.16): a request body nested deeper` | D (live API not served: a19-live-api-not-served) | agentapi, tests/agentapi | S
- `The Omarchy halves of the Hooks page's` | D (Linux look) | ui_automation | S
- `dictation (M6.11), deliberate: recover_walkthrough reads` | D | dictation | S
- `meeting (M7.17): listeners run inline` | D contract | meeting | S
- `meeting_agent (M8.2): malformed state shapes` | D (corrupt input) | meeting_agent, meeting_state | S
- `src/bench (M10.4), not recorded as differences` | D | bench, docs/port/audit | S
- `On Linux (GCC 13, Qt 6.10.3, offscreen), tests that fail` | D (Linux references would be new work) | tests/reference, tools/parity | L

## E: owner or Windows

- `The onnxruntime-cpu entry lives in its own embedded catalog` | E (needs the onnxruntime-win-x64 archive's URL, size and SHA-256: owner's yes to fetch) then D wiring | models | S after the facts
- `Tests that write the real Windows Credential Manager` | E (owner decides whether they move behind [interactive]) | tests/credentials, tests/backup | S
- `Branch keep/M4.10-review-hotkeys-tests (dd2499f)` | E (the branch is not in this clone: owner's machine or jed) | tests/ui_hotkeys | S
- `tests/credentials/test_wincred.cpp: the WinCred tests` (flaky) | E (Windows to reproduce) | credentials | M
- `meeting_agent (A27-19): the built-in loop settles a pass` | E (reading pi-ai's source needs a download: owner's yes) | meeting_agent | S
- Windows halves of B items 14, 16, 20 and A item 34 need the owner's Windows machine to verify (counted under A/B).
- `documents (M6.7)` Debug run on Windows and `runtime_setup_dialog` real file pickers: Windows/owner parts of C and A items (counted there).
- `On Linux ...` font failures: no owner action; D.
- `Generator checks` and `OPENWHISPER_CPP_OW_PYTHON` checks: could be E (running OpenWhisper's Python is lead decision 5) — counted as C.
