# UI controller and application controller

Area `a03-ui-controller`. Sources (relative to `D:\coding\whisper_local`):

| File | Lines | Role |
|---|---|---|
| `services/application_controller.py` | 1501 | `ApplicationController` (QObject): owns the recorder, backends, executors and every feature runtime; the engine reload/lease state machine; startup and shutdown order; refusal gates (meeting, backup, setup); Cancel routing. Also `PreviewEvents` (preview event throttle). |
| `ui_qt/ui_controller.py` | 2930 | `UIController` (QObject): owns `MainWindow`, `WaveformOverlay`, `SystemTrayManager`, clipboard helper, Settings / guided setup / update / meeting dialogs; routes overlay state, status text, transcripts, model-download progress, meeting state, update hand-off, code-change restart prompt. |
| `ui_qt/overlay_state.py` | 17 | `OverlayState` enum (logical overlay states). |

Both classes are glue. Almost every behaviour they expose is implemented in another area
(runtimes in a04, overlays in a07, tabs in a08, Settings in a11, updater in a16, meeting in
a24-a31); this area decides *who is told what, on which thread, and when to refuse*. The
port therefore must reproduce the wiring tables and gates exactly; the leaf behaviour is
mapped in the other areas.

## What the user gets

- One controller object graph started by bootstrap (a01): main window, floating waveform
  overlay, system tray, and the feature runtimes, all wired together.
- Recording start/stop/cancel from window, tray, hotkeys, D-Bus shell button, with
  refusals spoken in the status line ("Meeting Mode is active — end the meeting to use
  dictation", "Speech engine is still loading...", etc.) and the window's recording chrome
  rolled back when a start is refused.
- A single Cancel that cancels the recording/transcription, else an AI walkthrough
  write-up, else running actions, else says "Nothing to cancel".
- Engine reload after settings changes: debounced 400 ms, background thread, busy state
  greys engine combos, warm-up decode after load, status lines ("Reloading speech
  engine...", "Loading speech engine...", the engine's device info).
- Engine lease: the dictation engine is released for Meeting Mode and reloaded after.
- Overlay routing: recording / streaming preview / processing / transcribing / cleaning /
  rewriting / cancel animation / STT on-off moments / copied / notices / captions; upload
  jobs show progress inline in the Upload tab instead of the overlay.
- Transcript delivery to the Quick Record or Upload tab, with stats, errors and batch
  progress routed to whichever tab started the job.
- Settings window built in the background 2.5 s after launch, piece by piece; opened on a
  named destination from many places.
- Guided setup offer 750 ms after first paint for new installs.
- Model download progress dialog when Settings is not visible.
- Meeting Mode wiring: start readiness (platform ack, setup dialog, system-audio
  permission, Linux audio remediation), end confirmation, state to tab and tray, dashboard
  URLs, recovery dialogs, consent dialog.
- App update flow: notice in title bar and tray, update dialog, busy confirmation, hand-off
  to native helper / setup installer / macOS helper, hard-exit watchdog.
- "Code changed on disk" restart prompt polled every 10 s (dev/self-updating installs).
- About box, quit confirmation while busy, restart on backup restore.
- Clipboard services for paste (stage, restore, commit, selection capture, files).
- Walkthrough and screenshot glue (panel, drawing, snap, paste, library).

## Feature inventory

Decision column: **port**, **drop** (engine-specific), **undecided** (backend not decided:
Nemotron, cloud API, remote engine), **other-platform** (macOS/Linux only).

| ID | Feature | Exact user-visible behaviour | Source | Decision |
|---|---|---|---|---|
| A03-01 | Controller construction order | `seed_new_install_settings` first (before anything writes settings), recorder from settings, executors, runtimes (Hotkey, Streaming, Transcription, Command, Action, Walkthrough, PreviewEvents, Meeting, Remote, Models, Components, Updater), backends, UI callbacks, hotkeys, audio-level callback, signals, hook watchdog. Streaming setup deferred to `notify_main_ui_ready`. | application_controller.py:`ApplicationController.__init__` (165-275) | port |
| A03-02 | Executors | shared pool 2 workers (transcription, reloads); `database-startup` 1; `history-save` 1 (drained on quit, never cancelled); `component` 1 (installs run minutes); `walkthrough-library` 1. | application_controller.py:179-200 | port |
| A03-03 | Backend table and selection | Keys `local_whisper`, `api`, every `services.local_asr.catalog.BACKENDS` key, `remote`. Selected = `settings_manager.load_model_selection()` (key `selected_model`), fallback `local_whisper`. Logs "Using transcription backend: X". | `_setup_transcription_backends` (277-327) | port (parakeet only); local_whisper/others drop; api/remote/nemotron undecided |
| A03-04 | UI callback wiring | ~75 `ui_controller.on_*` / `get_*` assignments (record, profiles, upload, models, components, updates, meeting x16, settings, try action). Port as a table of `std::function` or signal connections. | `_setup_ui_callbacks` (329-430) | port |
| A03-05 | Settings-changed dispatch | kind `transforms`/`hotkeys`/`actions` -> refresh profile hotkeys; `webhooks`/`programs`/`filters`/`mcp_events` -> `automation_service.reload(kind)`; `updates` -> `updater.schedule_check()`. UI side: kind `updates` also refreshes update notice first. | `on_settings_changed` (440-452); ui_controller.py:`_on_settings_page_changed` (1415) | port |
| A03-06 | Debounced engine reload | Any thread may request. Refused with status "End the meeting before changing the engine" (meeting) or "Finish recording before changing the engine" (recording/transcribing), each also emits busy=False. Otherwise `_reload_pending=True` and a queued signal arms a single-shot 400 ms (`WHISPER_RELOAD_DEBOUNCE_MS`) timer on the Qt thread. On fire: re-arm if recording/transcribing/host update active/reload in flight; cancel with meeting status if meeting; else in-flight=True, busy=True, status = reload note or "Reloading speech engine...", submit worker. | `reload_whisper_model`, `_setup_reload_timer`, `_arm_reload_timer`, `_do_reload_whisper_model` (454-509) | port |
| A03-07 | Reload worker + warm-up | Under engine lock: abort if shutting down/meeting/recording/transcribing; release every unselected engine (`cleanup`); reload selected optional engine: `reload_model()`; if selection changed meanwhile, clean up and return; announce (device_info_update + status device_info, then `switch_error`, then `cpu_fallback_note`); missing model -> `ensure_local_model_available`; loaded and in `SPEECH_WARMUP_BACKENDS` (`parakeet`,`nemotron`,`parakeet_mlx`,`parakeet_unified`) -> warm-up decode outside the lock, then finish. Exceptions: status "Engine load failed: {exc}", device info = exc, ready False. | `_reload_worker`, `_reload_selected_engine`, `_reload_speech_engine`, `_announce_speech_engine` (519-562, 1438-1478) | port (parakeet path) |
| A03-08 | Reload finish | Clear in-flight under handoff lock, take `_restore_after_reload`. If restore owed and the selected engine got unloaded by a lease and no meeting: resubmit reload. Else busy=False; if unloaded, not model-missing, not leased, not remote -> `models.runtime_consent_requested(model_name)` (missing-runtime offer); remote -> force preview re-setup + `remote.remote_settled("reload")`; flush pending streaming setup. | `_finish_speech_reload` (564-591) | port (remote branch undecided) |
| A03-09 | Local Whisper reload worker | Status "Whisper engine ready" / "Model '{name}' is not downloaded" / "Engine reload failed" / "Ready"; GPU fallback signal. | `_reload_whisper_worker` (593-633) | drop |
| A03-10 | Initial engine load after first paint | `notify_main_ui_ready`: optional engine (Parakeet) -> pending streaming setup, in-flight, busy, status "Loading speech engine...", submit reload worker. local_whisper deferred -> same; else consent + GPU fallback + streaming setup via `QTimer.singleShot(0)`. API/other -> streaming setup next tick. | `notify_main_ui_ready` (830-894), `_start_initial_whisper_load` (642-649) | port |
| A03-11 | Startup background jobs | After first paint: DB init on `database-startup` executor (emits `startup_database_ready(error)`); credential-store warm (`credentials.store().status`), `updater.prune_leftovers`, `component_coordinator.list_components` on component executor; `updater.schedule_check(launch=True)`; remote host `apply_host_settings` on daemon thread `remote-host-start`; OpenAI SDK warm-up; `automation_service.start(self)` (logged on failure "Could not start automation"). | 863-894 | port (credential, updater, components, automation); remote undecided; SDK warm drop |
| A03-12 | DB-ready dependents | On error: `update_readiness_error = error`, status shows it, nothing else starts. On success: `_startup_database_ready=True`; submit `recover_recordings`; `meeting_runtime.setup()`; `record_sync.start()`; `remote_engine.start_client_history()`; `backup_coordinator.start()`. Error text default "Could not initialize the database". | `_initialize_startup_database`, `_on_startup_database_ready` (896-919) | port (record_sync/remote undecided) |
| A03-13 | Engine lease for Meeting Mode | `release_local_engine`: cleanup every backend with a loaded `model` except remote; clean preview, mark pending streaming setup; device info "Released while Meeting Mode runs" (ready False) if anything released; returns bool. `restore_local_engine`: no-op unless released; if reload in flight set `_restore_after_reload`, else status "Reloading speech engine...", busy, submit worker. | 651-694 | port |
| A03-14 | Readiness message before capture | None if backend available. No backend: "No transcription engine is selected". Optional engine: reload in flight -> "Speech engine is still loading..."; remote -> see A03-15; model missing -> trigger download consent and return engine `device_info`; closed by cancel or no last_error -> request reload and return "Reloading speech engine..."; else engine `device_info`. Whisper: "Whisper engine is still loading..." / "Whisper model '{m}' is not downloaded — finish the model download before recording". API: "The selected API transcription engine needs a valid API key". Else "The selected transcription engine is unavailable". | `transcription_readiness_message`, `_speech_engine_readiness_message`, `local_whisper_loading_message` (696-732, 1481-1491) | port (parakeet branch); whisper drop; api undecided |
| A03-15 | Remote readiness | `remote.retry_now()`; "Connecting to {host or 'the remote engine'}..." while restoring or no error; else "{last_error} Trying again...". | `_remote_engine_readiness_message` (1494-1501) | undecided |
| A03-16 | Start/stop/toggle recording | `start_recording(source)`: refuse gates (A03-17), readiness message -> status + False; else runtime start. Profile start skips readiness check (runtime handles). `toggle_recording` hotkey path same gates. Action / profile toggles: stop if recording, else start for that id. `toggle_route_recording`: stop, or check `routing.status(...)`, status its message if not ready, else start with `route="hotkey"`. Sources: `"hotkey"`, `"ui"`, `"dbus"`. | 996-1080 | port (dbus: Linux) |
| A03-17 | Dictation refusal gates | In order: backup in progress -> "Wait for backup or restore preparation to finish"; meeting setup dialog open -> "Close Meeting Mode setup before using Quick Record."; meeting active -> "Meeting Mode is active — end the meeting to use dictation". Used by start, profile, action, route, retranscribe, upload, upload batch, model change. | `_refuse_dictation_during_meeting` (1096-1109) | port |
| A03-18 | Engine-load refusal in meeting | Status "End the meeting before changing the engine"; used by model download workers. | `_refuse_engine_load_during_meeting` (1111-1127) | port |
| A03-19 | Cancel routing | `runtime.cancel()` else `runtime.cancel_write_up()` else emit `actions_cancel_requested` (queued to Qt) -> `action_runtime.cancel()` else status + notice "Nothing to cancel". | `cancel`, `_cancel_actions` (1129-1148), `NOTHING_TO_CANCEL` (46) | port |
| A03-20 | Rule-dictation clip transcription | Synchronous, worker thread. Raises: "Wait for backup or restore preparation to finish", "OpenWhisper is preparing an update. Try again shortly." (host update), "Meeting Mode is active — end the meeting to use dictation", "No transcription engine is available", "Transcription engine is busy". | `transcribe_clip` (1163-1195) | port (rename app) |
| A03-21 | Model change from UI | Refuse while recording/transcribing: "Finish recording or transcription before changing engines"; meeting gates; then `models.forget_runtime_prompts()` + `transcription_runtime.on_model_changed`. Local-engine settings changes ignored when Remote selected. | `on_model_changed` (1213-1219), `_on_local_engine_settings_changed` (953-958) | port |
| A03-22 | Audio device change | Refuse while recording/job: status "Wait for the recording to finish before changing device". Else cleanup recorder, rebuild from settings, rewire error/device-switch callbacks and level callback, status "Audio device changed". | `change_audio_device` (969-985) | port |
| A03-23 | Backup/restore reservation | `_acquire_backup` returns error text or None: "Wait for OpenWhisper to finish starting before backing up or restoring." / "Wait for recording recovery to finish before backing up or restoring." / "Wait for the walkthrough to finish saving before backing up or restoring." / "Finish recording or transcription before backing up or restoring." / "Finish the meeting and its processing before backing up or restoring." / "Finish dictating the cleanup rule before backing up or restoring." Takes capture, meeting and job locks together. Disables the main window central widget (remembers prior enabled state) until release. Backup worker context pauses record sync and remote host records. Restore staged without error -> `QApplication.exit(RESTART_EXIT_CODE)`. | 741-790, 822-828 | port (pausing remote/sync undecided) |
| A03-24 | Walkthrough library reservation | Errors: "Wait for OpenWhisper to finish starting." / "Wait for recording recovery to finish." / "Wait for the backup or restore to finish." / "Wait for the other walkthrough change to finish." / "Wait for the AI write-up to finish." / "Wait for the walkthrough being transcribed to finish." Dictation stays free. | `_acquire_walkthrough_library` (792-820) | port |
| A03-25 | Preview event throttle | `transcription.preview` events at most 4/s (`INTERVAL_S = 0.25`); too-early text held (only newest) and flushed when the quarter second is up; `final` sent immediately and drops held text. Timer interval `max(1, int(wait*1000)+1)` ms. | `PreviewEvents` (75-116), `_publish_preview` (1274) | port |
| A03-26 | Signals bus | `ApplicationController` signals: `transcription_completed(str, object raw, object cleanup_info)`, `transcription_failed(str)`, `walkthrough_written_up(obj)`, `recording_capture_failed(str)`, `history_persisted(obj)`, `startup_database_ready(str)`, `batch_progress(int,int,str)`, `batch_item_finished(int,bool,str)`, `batch_completed(obj)`, `large_file_detected(float)`, `status_update(str)`, `recording_state_changed(bool)`, `recording_destination_changed(str)`, `actions_cancel_requested()`, `overlay_state_update(obj)`, `transcription_retry_progress(str)`, `minimize_to_tray_requested()`, `device_info_update(str,bool)`, `engine_busy_changed(bool)`, `reload_debounce_requested()`, `notice_requested(str,bool)`, `recording_device_switched(str,str)`, `dictionary_term_learned(str)`. All emitted from any thread, delivered queued on Qt thread. | 122-163, `_connect_signals` (1224-1272) | port |
| A03-27 | Minimize-to-tray hotkey | Hotkey thread emits; Qt thread calls `main_window.toggle_tray_visibility`. | `minimize_to_tray` (1150) | port |
| A03-28 | Recording state to UI | On true: `ui_controller.on_dictation_started(profile)` (sets Quick Record tab profile). Always sets `is_recording` on both controllers and calls `main_window._update_recording_state()`. | `_on_recording_state_changed` (1278-1283) | port |
| A03-29 | Shutdown sequence | See Modules: exact order of ~20 steps, each wrapped in `_attempt` (log "{failure}: {exc}" and continue); history queue drained before DB close; records `shutdown_s` metric. | `cleanup` (1298-1357) | port (worker-process close drop) |
| A03-30 | OverlayState enum | values `none, recording, processing, transcribing, cleaning, canceling, stt_enabled, stt_disabled, command_listening, rewriting`. | overlay_state.py | port |
| A03-31 | Overlay state routing | Upload job active -> stage goes to Upload tab card (tray recording off, overlay hidden) unless the state has no stage; NONE still passed to card and runs teardown. Otherwise Quick Record tab `set_activity_state(state)` then: CANCELING -> tray off, dismiss walkthrough panel, cancel animation; NONE -> tray off, hide overlay, clear streaming, flow inactive; RECORDING/COMMAND_LISTENING -> tray on, listening overlay; PROCESSING/TRANSCRIBING/CLEANING/REWRITING -> tray off, (PROCESSING clears processing label), clear streaming text, show/set waveform state; STT_ENABLED/DISABLED -> show/set moment state. | ui_controller.py:`set_overlay_state` (875-914), `_OVERLAY_WORK_STATES`, `_OVERLAY_MOMENT_STATES` (117-127), `_route_upload_progress` (942-959) | port |
| A03-32 | Listening overlay | COMMAND_LISTENING -> `STATE_COMMAND_LISTENING`. Streaming flow active -> `STATE_STREAMING` (show at cursor if hidden; set only if not already). Else `STATE_RECORDING`. | `_show_listening_overlay` (923-932) | port |
| A03-33 | Cancel animation | Stop timer, flow inactive, clear streaming text, show/set `STATE_CANCELING`, single-shot timer 1200 + 200 = 1400 ms (`CANCELLATION_ANIMATION_DURATION_MS + CANCELLATION_GRACE_MS`). On fire: only if overlay state is CANCELING or IDLE -> hide overlay, Quick Record activity NONE (a new recording started meanwhile is left alone). | `_start_cancel_animation`, `_on_cancel_animation_finished` (1450-1471) | port |
| A03-34 | Streaming preview overlay | show: flow active, clear text, show at cursor `STATE_STREAMING`. update: `overlay.update_streaming_text(text, final)`. hide: flow inactive, clear text (does not hide overlay). | 1436-1448 | port |
| A03-35 | Transcription retry on overlay | `show_transcription_retry(label)` -> processing state + label; failure -> overlay notice. | 916-921 | port |
| A03-36 | Large file splitting | Upload job -> tab `set_large_file_stage(mb)`; else overlay `set_large_file_info(mb)` + show at cursor `STATE_LARGE_FILE_SPLITTING`. | `show_large_file_state` (961-970) | undecided (only API splits) |
| A03-37 | Transcript display | Routed by `_transcription_source_tab` (Quick Record default; Upload when the upload tab started it): `set_transcript(text, raw)` then `expand_transcription()`, hide overlay. Error: Upload -> `show_job_error(msg)`; QR -> `clear_transcription()` (transcript box stays empty so Copy can't copy an error); hide overlay; dismiss walkthrough panel. | `_display_transcript`, `_display_transcription_error` (686-704) | port |
| A03-38 | Status routing | If QR tab `engine_loading`: main status + upload tab engine message. Upload job source -> upload tab status. Else main window status. Ambient status (failed update check) -> `quick_record_tab.set_ambient_message`. | `_apply_status_to_main_window` (706-713), `set_ambient_status` (871) | port |
| A03-39 | Audio levels | `overlay.update_audio_levels(levels)`; walkthrough panel `hear(max(levels))`. | 715-718 | port |
| A03-40 | UI-side recording start | Source tab = QR; profile id -> `on_profile_record_start(id, source)` else `on_record_start(source)`; False -> revert (is_recording False on both, `_update_recording_state`, log "Recording start refused; reverting recording state"). Without callbacks emits `record_started` (shows overlay). | `start_recording`, `_revert_refused_record_start` (720-744) | port |
| A03-41 | UI-side cancel | `cancel_recording`: is_recording False, window state updated, `on_record_cancel()`, clear QR transcript. Upload cancel only when upload job active; uses `on_upload_cancel` or `on_record_cancel` without touching recording state. | 757-770, 2466-2476 | port |
| A03-42 | Tray actions | Exit -> `main_window.request_quit()` (not `QApplication.quit`, which minimize-to-tray vetoes). Toggle recording (source "ui"; D-Bus shell "dbus"). Meeting toggle (end with confirmation or start with readiness, cloud=None). Dashboard, copy last original, walkthrough("tray"), screenshot("tray"), scratchpad, stats, update (restore window + show found update), language menu (populated once at start too because some platforms won't open an empty submenu). | `_setup_connections` (480-496), 651-670, 2206-2214 | port |
| A03-43 | Main-window signal wiring | 35+ `main_window.*` signals connected (see Modules). Lazily created screens (`screen_created(key, widget)`) for `meeting` and `walkthrough` get connected when created. | `_setup_connections` (420-511), `_on_screen_created` | port |
| A03-44 | Agent/MCP change notifications | `agent_data_changed(kind, changes)`: non-settings -> refresh history, `record_sync.record_saved/record_edited` with kind dictation (for "transcription") or meeting. settings -> re-read saved values (newest wins over queued), apply: theme, font scale, streaming enabled, overlay font size, overlay layout, recording look (look/dot/text/clock), cleanup enabled/level -> refresh controls, trigger mode; refresh open Settings. | `_apply_agent_changes` (576-616) | port (record_sync undecided) |
| A03-45 | Engine busy UI | QR, Upload and Host screens `set_engine_busy(busy)`; on idle refresh model views (Downloads delete lock). | `set_engine_busy` (827-841) | port (host undecided) |
| A03-46 | Device info + remote models | `main_window.set_device_info(info, ready)`; then refresh remote models (`get_remote_models`) and `dictation_language.note_remote_runtime`. | 779-795 | port; remote part undecided |
| A03-47 | Remote engine hooks | remote/host model selection (host error "Switching models isn't available in this window." -> host dashboard `show_engine_error`), runtime selection, retry link, `set_remote_link`, `set_remote_clients`, pair-request prompter created when `remote_engine` set, host dashboard bind. | 398-418, 797-825 | undecided |
| A03-48 | Transcription stats | Routed by source tab: `(transcription_time, audio_duration, file_size, cleanup_time, remote=RemoteTiming)`. Clear: main + upload (only if upload screen already built; a recording must not build it). | 843-866 | port |
| A03-49 | Notices near pointer | `show_notice(text, done)` skipped when main window `isActiveWindow()` (status line already says it). | 992-999 | port |
| A03-50 | Copied animation | overlay show at cursor `STATE_COPIED`; used by upload copy, past-meeting copy, guest link copy, main window callback. | 989 | port |
| A03-51 | Clipboard service | `copy_to_clipboard(text, html)`, `prefetch_clipboard_snapshot`, `discard_clipboard_prefetch`, `stage_transcript_for_paste(text, html, restore)`, `capture_selection(callback, timeout_ms=700 COMMAND_SELECTION_TIMEOUT_MS)` via `synthetic_keys.send_copy`, `schedule_clipboard_restore(stage)` with 250 ms (`AUTO_PASTE_CLIPBOARD_RESTORE_DELAY_MS`), `commit_transcript_clipboard`, `copy_files_to_clipboard(paths, text, image)`. Restore failure status "Ready (Pasted; clipboard restore failed)". | 1001-1065 | port |
| A03-52 | Copy buttons | QR copy -> `quick_record_tab.show_copy_result(ok)`. Upload copy -> tab status "Copied to clipboard" + copied animation, or "Copy failed". | 2497-2507 | port |
| A03-53 | Walkthrough glue | tray/window/overlay finish -> `on_walkthrough("tray"/"ui")`; snap -> `on_screenshot`; `snap_support`, `request_snap_access`, `start_walkthrough_drawing(mark_shortcuts)`, `stop_walkthrough_drawing`, `start_snap(hint, take="region", front, click_at, screen)` with keep-out windows (overlay if top-level, panel, drawing windows) and float-above overlay; `keep_overlay_out_of_screenshots`; `set_walkthrough_active` (tray, window, overlay controls, panel; minimize to tray if `walkthrough_minimize_to_tray`); panel stand-in for overlay unless AI write-up running; `show_walkthrough_ready`; paste latest ("Couldn't open the latest walkthrough: {exc}", "No completed walkthrough to paste yet"); `paste_walkthrough` ("Walkthrough saved; another walkthrough paste is in progress", "Pasting PDF, then instructions…", warnings "Write-up failed: …", "{n} screenshot(s) couldn't be saved" joined by ". "); copy again ("Walkthrough copied" / "Couldn't copy the walkthrough" / "Couldn't open the walkthrough: {exc}"); library dialog singleton refreshed only when not busy and not dirty. | 1067-1332 | port |
| A03-54 | Scratchpad, language, history actions | `toggle_scratchpad`, `insert_into_scratchpad(text)->bool`, `show_in_scratchpad`, `cycle_dictation_language` (overlay signal), `populate_language_menu`, `paste_last_original`, `copy_last_original`, `set_hands_free`. | 1334-1367, 1410 | port |
| A03-55 | Mic switched notice | status "Microphone disconnected — continuing on {name}" and overlay caption "Switched to {name}", name default "the system default microphone". | `on_recording_device_switched` (1369-1372) | port |
| A03-56 | Dictionary learned notice | protected entry -> 'AI cleanup will keep "{term}" as you said it', else 'Added "{term}" to your dictionary'; refresh Settings DICTIONARY page. | 1374-1383 | port |
| A03-57 | Stats feature links | key `meetings`/`files`/`walkthroughs` -> `main_window.show_past_items(tab)`; `actions`/`commands`/`snippets`/`dictionary` -> Settings destination. | `open_stats_feature` (1390-1408) | port |
| A03-58 | Try action plumbing | Settings "Try it": `on_try_action(action, sample, done)`; missing -> `done(ActionResult(False, "Actions aren't ready yet."))`; cancel try runs. | 1422-1434 | port |
| A03-59 | Guided setup offer | If `should_invite()`: start 750 ms single-shot; on fire, re-check; if a modal is active, main window hidden or minimized -> retry in 1000 ms; else show on welcome. Menu "Guided setup…" opens first step. Singleton dialog; on first creation connects `transcription_received` / `transcription_error_received` to the dialog's dictation try box. | 1476-1529 | port |
| A03-60 | Settings opening API | `open_settings_dialog(focus_hf_policy)` (Home or HF policy), `open_engine_help_destination(dest)` (`api_keys` -> focus selected endpoint key env; `remote_models` -> RemoteModelsDialog per host or Settings `remote_engine`; `ondemand`/`downloads`/`remote_engine`), `open_hotkey_settings`, `open_cleanup_profiles`, `open_settings_destination(name=OVERVIEW)` (`engine_downloads` -> Downloads focusing missing runtime; `text` -> focus cleanup model; legacy names `ondemand`,`text`,`meeting`,`runtime`,`downloads`), `open_downloads(component_id)` (skipped when guided setup shows that install). Always `refresh()` then show/raise/activate. | 1536-1601, 1823-1865 | port |
| A03-61 | Settings dialog binding | `_prepare_settings_dialog` assigns ~30 hooks every open (font scale, theme, overlay font/layout/look, streaming, hotkeys, profiles, trigger mode, api keys, developer mode -> meeting screen, cleanup, try, remote section, models pages, downloads). | 1605-1648 | port |
| A03-62 | Staged Settings build | `build_settings_when_idle`: start 2500 ms after launch; each tick builds one piece (`build_step()`), gap 60 ms; recording, meeting, active modal or popup -> retry in 1000 ms; exception -> log "Could not build Settings ahead of its first opening", discard. Opening mid-build calls `finish_build()` on the same dialog. Adoption connects Downloads component install/cancel/remove signals. | 1650-1731 (`SETTINGS_BUILD_DELAY_MS`, `_GAP_MS`, `_RETRY_MS`) | port |
| A03-63 | Theme and font scale | `apply_ui_font_scale(percent)` then redraw rendered Markdown (QR transcript, upload transcript, upload viewer typography); theme only redraws when `apply_ui_theme` reports a change. | 1755-1772 | port |
| A03-64 | Model download UI fan-out | started/progress/finished/batch planned/batch finished/deleted to QR tab, Upload screen, Settings Downloads and model pages, guided setup. Fallback progress dialog when neither Settings nor the guided-setup download view is visible. | 1898-2011 | port (Parakeet model download) |
| A03-65 | Download progress dialog | `QProgressDialog` label `Downloading "{model}"…`, title "Downloading model", min duration 0, no cancel button, no auto close/reset, min width 360. Total<=0 -> busy range (0,0); else 0-100 and label `Downloading "{model}"… {done} of {total}` via `format_size_bytes`. Closed + deleteLater on finish. | 1963-2005 | port |
| A03-66 | Component install fan-out | install/cancel/remove requests; progress `(id, phase, done, total)`; state changed; finished `(id, ok, message)` to Downloads, model pages, guided setup. | 2013-2048 | port (NeMo runtime CPU/GPU component) |
| A03-67 | Consent / runtime dialogs | `show_hf_consent_dialog(model, policy, env_blocked)->ConsentAction`; `show_required_runtime_dialog(model, component)->bool`; `show_gpu_runtime_dialog(model, gpu, cpu)->choice|None`; `show_use_gpu_dialog(plan)->bool`. All modal `exec()` on Qt thread. | 1781-1821 | port (HF consent for Parakeet .nemo download; GPU runtime offer) |
| A03-68 | Meeting readiness | Platform ack (unsupported OS dialog), unlock meeting tab; if setup available and setup open -> None; setup incomplete -> open setup (starting=True), non-"auto" policy returned; system-audio permission (macOS); Linux implementation ready -> Linux audio dialog: Cancel -> None, microphone only -> "disabled", else "required"; default "auto". Demo start uses only the platform ack and policy "disabled". | `ensure_meeting_platform_ack`, `ensure_meeting_start_readiness`, `_on_meeting_demo_requested` (2050-2154) | port |
| A03-69 | Meeting setup dialog guard | Refused when already open or meeting active; when recording: status "Finish Quick Record before checking meeting audio."; sets `_meeting_setup_open` (which also blocks dictation, A03-17) for the modal's life. | `_open_meeting_setup` (2119-2135) | port |
| A03-70 | End meeting confirmation | Question box title "End meeting?", text "End this meeting now? Audio capture will stop immediately. OpenWhisper will keep working in the background to finish the transcript and report.", Yes/No default No. | `_on_meeting_end_requested` (2216-2230) | port (rename) |
| A03-71 | Meeting state payload | `dashboard_available False` clears URLs; tab gets payload with `dashboard_available` default; active -> clear sidebar past-meeting selection; else `meeting_id` selects it; inactive with status in {ended, failed, needs_recovery, canceled} -> refresh past meetings; `active` key -> tray meeting active + switch to Meeting tab + lock tabs (`set_recording_state(True, TAB_MEETING_MODE)`) or unlock (`False, -1`); else if dashboard -> tray dashboard available. | `on_meeting_state_changed` (2232-2284) | port |
| A03-72 | Meeting status / errors / server | status to meeting screen + main status; error -> status, then restart offer if code changed, else warning "Meeting Error" (default text "Meeting error"); server started -> store URLs, tray + tab dashboard availability, auto-open dashboard. Guest link copy -> clipboard, status "Guest link copied", copied animation. Past transcript copy -> "Transcript copied". | 2286-2327, 2422-2433, 2188-2194 | port |
| A03-73 | Meeting consent / recovery dialogs | Consent dialog with destination/remote from meeting LLM profile; recovery dialog with finalize/discard callbacks; non-modal scan-error box: title "Meeting Recovery", warning icon, informative "Saved meeting audio remains on this computer. Retry the scan to find interrupted meetings.", buttons "Retry scan" (ActionRole), "Later" (RejectRole); reused if visible (text updated, raised); status "Meeting recovery scan failed". | 2328-2420 | port |
| A03-74 | Hotkey display | Labels for record_toggle, cancel, enable_disable, minimize_tray (defaults from `config.DEFAULT_HOTKEYS`: Win/Linux `kp *`, `kp -`, `ctrl+alt+kp *`, `ctrl+alt+m`; macOS `ctrl+alt+r`, `ctrl+alt+escape`, `ctrl+alt+shift+r`, `ctrl+alt+m`), walkthrough, screenshot (unbound default) -> `main_window.update_hotkeys(...)` and overlay control hints. Settings hotkey change goes to `on_hotkeys_changed` or saves directly. | `update_hotkey_display` (2518-2537), 2435-2448 | port |
| A03-75 | Upload job entry | single file `(path, duration_s)` / batch request; sets source tab Upload; batch progress/item finished only while upload job active; batch result to tab. | 2450-2495 | port |
| A03-76 | Tab switching | `switch_to_tab(i)` leaves Host Mode first; `switch_to_meeting_mode`. | 2509-2516 | port |
| A03-77 | Update check results | Dialog open: manual check only raises it. Else keep last non-None result (offline failure keeps release found earlier), refresh notice (title bar + tray version when `shows_update_notice(status, settings)`), manual -> dialog. | `on_update_check_finished`, `_refresh_update_notice`, `show_found_update` (2544-2586) | port (if updater kept; see a16) |
| A03-78 | Update dialog | Modal `AppUpdateDialog`; primary action with release but cannot apply -> open release html_url; no result -> open `RELEASES_PAGE_URL`. After close, notify settings kind "updates". | `show_app_update_dialog` (2588-2614) | port |
| A03-79 | Busy confirmation | Busy reasons in order: "a recording", "a meeting", "transcription", "a component install". Quit: title "Quit OpenWhisper?", "OpenWhisper is busy with {busy}. Quitting now stops it before it finishes." Update: title "Install update?", "OpenWhisper is busy with {busy}. Install the update anyway? Unsaved work in that task may be lost." Both Yes/No default No. Declined update start -> dialog error "The update was not started." | 2616-2671 | port |
| A03-80 | Update hand-off | Download finished: error -> dialog error (offer setup if native result with verified setup asset) / status "Update canceled" if canceled and closed / warning box "Update failed"; canceled -> abandon "The update was canceled."; busy declined -> abandon "The update was not installed. Your work is still open."; macOS DMG -> helper or open disk image ("The Mac update disk image could not be opened."); native transaction -> journal helper exe/argv (dialog phase RESTARTING) or error (offer setup); else setup installer `/SP- /SILENT /NORESTART /OPENWHISPERUPDATE=1 /ALLUSERS|/CURRENTUSER /DIR=<app> /LOG=<updates>/setup.log` after releasing app mutex. Launch failure: "The updater could not be started." / "The installer could not be started." (reacquire mutex for setup). Success -> `exit_for_update`. | 133-239, 2655-2714 | port (Windows installer flavour decided in a16) |
| A03-81 | macOS update helper | start helper; poll every 100 ms until `helper_ready` or process exit / `READY_WAIT_S` deadline -> "The updater could not be started.". | `_start_macos_update` (2716-2748) | other-platform |
| A03-82 | Exit for update + watchdog | Mark dialog handed off, `_force_quit`, arm 10 s (`HANDOFF_EXIT_GRACE_S`) daemon timer that logs "Still running %.0fs after handing off to the updater; exiting hard" and `os._exit(0)`; `QApplication.exit(0)` (cannot be vetoed). | `exit_for_update`, `arm_handoff_watchdog`, `_hard_exit` (64-97, 2750-2767) | port |
| A03-83 | Code-changed restart prompt | Timer every 10 000 ms (`CODE_CHANGE_POLL_MS`). Setting `update_restart_prompt_enabled` false -> stop. If `changed_since_launch()` and not busy -> offer (stops timer: asked once per run). Box title "Restart OpenWhisper"; info text "OpenWhisper was updated while it was running." + "Restart to finish the update. Until you do, parts of the app that haven't loaded yet may not work." + checkbox "Don't show this again" (saves setting false); failure variant warning "OpenWhisper was updated while it was running, which likely caused this error." + "Restart to finish the update, then try again." + details = error, no checkbox. Buttons "Restart now" (Accept, default) / "Later". Restart confirms busy work, saves geometry, `_force_quit`, `exit(RESTART_EXIT_CODE)`. | 2769-2830 | port only if OpenWhisper_CPP has a source-run/self-update case; otherwise drop (see open questions) |
| A03-84 | Leftover apply error | On launch, `consume_apply_error()` -> warning "Update failed". | 2832-2837 | port (with a16) |
| A03-85 | About box | `QMessageBox.about` title "About OpenWhisper"; HTML with bold product line, "Version {VERSION}<br>Install: {channel}", optional "Update status: Update available: X / Development build, newer than X / Development build / Up to date", description, five bullets, website link `https://openwhisper.fiorilabs.tech/`. | `show_about_dialog` (2839-2879) | port with OpenWhisper_CPP wording |
| A03-86 | Retranscribe | history panel -> `on_retranscribe(path)` (meeting/backup gates). | 2884-2890 | port |
| A03-87 | UI cleanup | cancel walkthrough paste; clipboard cleanup (restore); stop settings-build, code-change, cancel-animation timers; stop overlay timer, close overlay; hide tray and unparent; `_force_quit` then close main window. | `UIController.cleanup` (2892-2930) | port |
| A03-88 | Overlay parent on Wayland | Overlay parented to main window on Wayland (top-level elsewhere). | 264-266 | other-platform |
| A03-89 | macOS dock icon sync | Installed when tray is available on darwin. | 269-273 | other-platform |
| A03-90 | OpenAI SDK / PyAV warm-up | Import SDK/PyAV in background threads to hide first-use latency. | 48-72, 921-949 | drop (Python import cost) |

### Added by check

Small wiring behaviours the table above did not name. Each is a one-step forward unless
stated; a port that drops one silently breaks a control.

| ID | Feature | Exact behaviour | Source | Decision |
|---|---|---|---|---|
| A03-91 | Engine-change entry points from the UI | `main_window.model_changed` -> `_on_model_changed` (logs "Model changed to: {name}") -> `on_model_changed`. `whisper_engine_changed` (local model/device/quant combo in the main window) -> `on_whisper_settings_changed` (the same debounced reload hook as Settings, A03-06). Settings model pages `runtime_changed` -> same hook; `_on_manager_set_active(name)` saves `whisper_model` first, then the hook. `select_transcription_backend(display_name)` picks a backend via the first existing transcription tab's engine bar (`choose_backend`), no-op when no tab is built. | ui_controller.py:`_on_model_changed`, `_on_whisper_engine_changed`, `_on_manager_runtime_changed`, `_on_manager_set_active`, `select_transcription_backend` (624-637, 1867-1896) | port (Parakeet model key replaces `whisper_model`; `select_transcription_backend` only if more than one engine stays) |
| A03-92 | Live preview toggle paths | Tab footer toggle -> `on_streaming_settings_changed` directly (logs "Live preview toggled via main GUI"). Settings toggle -> `refresh_live_preview_controls()` (tabs mirror it) first, then the same hook; the controller then runs `reconfigure_streaming`. | `_on_live_preview_changed`, `_on_settings_streaming_changed`, `refresh_live_preview_controls` (639-649, 1752) | port |
| A03-93 | Cleanup settings fan-out | Settings cleanup change -> `refresh_cleanup_controls()` (Quick Record `load_cleanup_setting`; Upload tab only if built) then `CleanupAgentOffer.maybe_offer()`. Offer accepted -> refresh Settings and guided setup dialogs if they exist. Cleanup profiles changed -> Quick Record `refresh_cleanup_profiles()` then `on_cleanup_profiles_changed` (profile hotkeys). Profile hotkey capture start/stop -> `on_profile_hotkey_capture(suspended)` so global hotkeys pause while a key is being recorded. | `_on_settings_cleanup_changed`, `_on_cleanup_agent_accepted`, `refresh_cleanup_controls`, `_on_cleanup_profiles_changed`, `_on_profile_hotkey_capture` (1596-1603, 1737-1752) | port |
| A03-94 | API-key change | Key saved or removed -> `refresh_model_views()` (re-read model assignments and downloads, only when Settings is visible). No reload; clients resolve the key on next connect. | `_on_api_keys_changed`, `refresh_model_views` (1774-1779, 1898-1902) | undecided (cloud API) |
| A03-95 | Model batch download controls | `request_model_batch_stop` -> `on_model_batch_stop` (the Downloads page "Stop" for a batch). Batch planned/finished go to Settings Downloads only (`begin_batch(list)` / `finish_batch(completed, planned)`), nothing when Settings was never built. `_download_model_for_setup(name)` -> `on_model_download_requested` for the guided-setup download button. | 1949-1961, 1527-1529 | port (Parakeet model; batch only if more than one downloadable model) |
| A03-96 | Controller-side completion forwards | `ApplicationController._on_transcription_complete`, `_on_transcription_error`, `_on_batch_complete` are the slots of `transcription_completed`, `transcription_failed`, `batch_completed`; each just calls the matching `transcription_runtime.on_*`. `update_status_with_auto_hide(status)` forwards to `hotkey_runtime` (status that clears itself). `setup_hook_watchdog()` of the hotkey runtime is called once at the end of construction (a04/a06). | application_controller.py:1221-1222, 1285-1296, 275 | port |
| A03-97 | Meeting tab one-line forwards | `start_requested(cloud, intent)` -> readiness (A03-68) then `on_meeting_start(cloud, system_audio_policy=policy, intent=intent)`; `demo_requested` -> `on_meeting_start_demo(cloud, system_audio_policy="disabled")`; `cloud_toggled` -> `on_meeting_toggle_cloud`; open dashboard / report, retry insights / speakers / step(id), defer insights -> `on_meeting_*`; past meeting open / copy transcript / delete(id, delete_recordings=True) / clear(delete_recordings) -> `on_meeting_open_past`, `on_meeting_copy_transcript` (returns text; empty text copies nothing), `on_meeting_delete_past`, `on_meeting_clear_past`. | 2137-2204 | port |
| A03-98 | Update dialog requests | Download and "setup installer" requests both confirm busy work first (A03-79; declined -> dialog `set_error("The update was not started.")`), clear `_update_canceled`, then `on_update_download(result)` or `on_update_download(result, True)` (the second flag forces the installer path). Cancel -> `_update_canceled = True` then `on_update_cancel`. Progress `(phase, done, total)` -> dialog `set_progress`. Manual check button -> `on_check_for_updates` ("always hits GitHub"). | 2539-2543, 2655-2682 | port (with a16) |
| A03-99 | Tray / window helpers | `show_main_window` = `main_window.restore_from_tray()`; tray update item restores then `show_found_update`. History panel refresh = `refresh_history`. `show_overlay_caption(text)` shows a caption on the overlay (used for "Switched to {name}" and walkthrough captions). `on_dictation_started(profile)` selects the profile in the Quick Record tab. | 651-653, 746, 1211, 1473, 2881 | port |
| A03-100 | Restore staging and backup lifecycle | `_on_restore_staged(path, error)`: no error -> `QApplication.exit(RESTART_EXIT_CODE)`; `_release_backup` re-enables the central widget. `_submit_restore_reload` is the lease-restore reload submit (A03-13). `setup_backup()` builds the `BackupCoordinator` with this controller's acquire/release/record-context hooks and is called by bootstrap before `notify_main_ui_ready`. | application_controller.py:690-695, 741-755, 785-790, 822-828 | port |

## Modules

### services/application_controller.py

**Responsibility.** Composition root for non-UI logic plus a few cross-cutting state
machines (engine reload/lease, startup DB gate, backup and walkthrough-library
reservations, Cancel routing, shutdown order). Feature runtimes receive `self` and reach
back into its attributes (`recorder`, `current_backend`, `executor`, signals,
`ui_controller`, locks), so the controller is effectively a shared context object.

**Public surface.**
- Signals: A03-26 list.
- Constants: `NOTHING_TO_CANCEL = "Nothing to cancel"` (46).
- `PreviewEvents(publish)`, `offer(job, text, final)`; `INTERVAL_S = 0.25`.
- Methods called from runtimes / UI / hotkeys: `start_recording(source)`,
  `start_profile_recording(id, source)`, `toggle_profile_recording(id)`,
  `start_action_recording(id, source)`, `toggle_action_recording(id)`,
  `toggle_route_recording()`, `stop_recording()`, `toggle_recording()->bool`, `cancel()`,
  `minimize_to_tray()`, `toggle_meeting_mode()`, `command_key_pressed(at)` /
  `command_key_released(at)`, `transcribe_clip(path)->str`, `retranscribe_audio`,
  `upload_audio_file(path, duration_seconds)`, `upload_audio_files(request)`,
  `on_model_changed(name)`, `update_hotkeys(dict)`, `update_recording_trigger_mode(mode)`,
  `reconfigure_streaming()`, `change_audio_device()`, `on_settings_changed(kind)`,
  `reload_whisper_model()`, `release_local_engine()->bool`, `restore_local_engine()`,
  `transcription_readiness_message()->Optional[str]`, `is_transcribing()`,
  `is_meeting_active()`, `host_update_active()`, `_engine_settled()`,
  `ensure_local_model_available()`, `setup_backup()`, `notify_main_ui_ready()`,
  `update_status_with_auto_hide(status)`, `cleanup()`.
- Attributes read by runtimes: `recorder`, `executor`, `persistence_executor`,
  `component_executor`, `walkthrough_executor`, `transcription_backends`,
  `current_backend`, `_current_model_name`, `_streaming_backend`, `streaming_transcriber`,
  `_pending_*` job fields (audio path, duration, file size, source name, streaming text,
  start time, elapsed, remote timing), `_shutdown_cancel` (Event), `_shutting_down`,
  `meeting_active`, `update_readiness_error`, `remote_engine`, `record_sync`,
  `_engine_lock` (RLock), `_reload_handoff_lock`, `_backup_in_progress`.

**Threading model.**
- Constructed and signals delivered on the Qt thread. All signals cross threads as queued
  connections (PyQt auto connection from worker threads).
- Reload: request any thread -> `reload_debounce_requested` signal -> Qt-thread
  `QTimer` (a QTimer started off its thread never fires, 481-483) -> worker on shared
  executor. `_reload_pending` lets other threads see a reload coming before the timer is
  armed (`_engine_settled`).
- `_engine_lock` (RLock) serializes load/release; warm-up runs outside it so a meeting
  lease never waits (530-534).
- `_reload_handoff_lock` guards `_reload_in_flight` against `_restore_after_reload`.
- Backup acquire takes `runtime._capture_lock`, `meeting._lock`, `runtime._job_lock`
  together (764) — lock order matters.
- Hotkey dispatcher thread calls `toggle_*`, `cancel`, `command_key_*`; notices go via
  `notice_requested` signal.

**State machine: engine reload.**
States: Settled -> Pending (`_reload_pending`, timer armed) -> InFlight
(`_reload_in_flight`, busy=True) -> [Warming] -> Finish -> Settled. Re-arms the 400 ms
timer while recording, transcribing, host update or another reload is running. Meeting
cancels the pending reload. Lease adds `_engine_released_for_lease` and
`_restore_after_reload` (lease returned during warm-up -> resubmit reload,
`_finish_speech_reload` 571-575). A port should model this as an explicit enum with
transitions tested by golden traces (test_engine_warmup.py pins every edge).

**Shutdown order (`cleanup`, 1298-1357)** — keep exactly:
1. `_shutting_down = True`; backup coordinator shutdown; startup executor shutdown
   (wait, cancel futures); set `_shutdown_cancel`.
2. Cancel model batch download; stop Auto Mode (walkthrough runtime); transcription
   runtime `begin_shutdown`; UI `cancel_walkthrough_library`.
3. Command runtime cleanup; stop automation (action runtime close + automation service
   stop); stop audio playback + focus-context service (COM objects); cancel active
   transcription; stop timers (`_watchdog_timer`, `_reload_timer`,
   `_periodic_refresh_timer`, remote timers, updater).
4. Interrupt backends (cancel + cleanup each, then close worker processes) **before**
   joining executors, because an executor may be inside model construction or a network
   request.
5. Meeting runtime cleanup; hotkey runtime and manager cleanup; recorder cleanup;
   streaming cleanup; cancel all component installs (download loop checks cancel once per
   MiB); record sync stop (2 s timeout); remote engine shutdown + remote backend cleanup.
6. Shutdown `executor`, `component_executor`, `walkthrough_executor` (wait, cancel
   futures).
7. Drain `persistence_executor` without cancelling (accepted transcripts survive Quit).
8. Backends cleanup (clear table, `current_backend=None`); UI controller cleanup; DB close.
9. Record metric `shutdown_s`.

**Edge cases to keep.**
- `seed_new_install_settings` must run before anything writes settings (168-170).
- Recorder callbacks rewired after every rebuild (980-982).
- `start_profile_recording` does not call the readiness check (1016-1019) — deliberate or
  not, tests pin profile behaviour through the runtime; keep identical.
- `toggle_recording` checks meeting gates only when not recording, so stop always works
  (1072).
- Readiness check off the Qt thread really reloads (test_controller_thread_hops.py:55).
- Reload of an engine that was switched away during load cleans it up and is not warmed
  (1451-1453).
- Released-by-lease engine never triggers the missing-runtime offer (578-582).
- `_finish_speech_reload` and `_reload_worker` return early when shutting down.
- DB init error is surfaced and blocks dependents; shutdown during init does not start
  them (903, 907).
- `_acquire_backup` fails if Settings' cleanup-rule dictation is not idle (774-777).
- Restore success exits with `RESTART_EXIT_CODE`; bootstrap relaunches after closing
  writers.

### ui_qt/overlay_state.py

`OverlayState` enum, string values listed in A03-30. Used by runtimes (emit through
`overlay_state_update`), Quick Record tab `set_activity_state`, Upload tab
`set_progress_state` and `widgets/transcription_progress.stage_for_overlay_state`.
C++: `enum class OverlayState` in a Qt-free header plus `toString/fromString` if any wire
or log uses the value strings (check a20 MCP status).

### ui_qt/ui_controller.py

**Responsibility.** Owns the top-level UI objects and is the only object the
application controller and runtimes talk to for UI effects. Exposes a callback-slot
surface (`on_*` attributes set by the app controller, read at call time) and a method
surface (called by runtimes on the Qt thread or through its own signals).

**Owned objects (constructor, 251-396).**
- `TemporaryClipboard(QApplication.clipboard(), self)`; `restore_failed` -> status.
- `MainWindow()`; `WaveformOverlay(parent=main_window if Wayland else None)`;
  `SystemTrayManager(main_window)`; `main_window.set_tray_available(tray.available)`.
- macOS dock icon sync.
- `CleanupAgentOffer(main_window, self)` with `on_accepted`.
- Timers: `_code_change_timer` (10 s repeating, started in ctor), `_settings_build_timer`
  (single shot), `_guided_setup_offer_timer` (single shot), `cancel_animation_timer`
  (single shot, no parent).
- State: `is_recording`, `streaming_flow_active`, `_transcription_source_tab` (default
  `TAB_QUICK_RECORD`), `_meeting_active`, `_meeting_urls`, `_meeting_setup_open`,
  `_settings_dialog`, `_settings_build`, `_guided_setup_dialog`, `_update_dialog`,
  `_last_update_result`, `_update_canceled`, `_download_progress_dialog`,
  `_walkthrough_*`, `_remote_models_dialog`, `_pair_prompter`,
  `_meeting_recovery_scan_dialog`, `_macos_update_timer`.

**Own signals.** `record_started()`, `record_stopped()`,
`transcription_received(str, object raw)`, `transcription_error_received(str)`,
`status_changed(str)`, `audio_levels_updated(list)`, `agent_data_changed(str, dict)`.
These exist so `set_transcript`, `show_transcription_error`, `set_status`,
`update_audio_levels` and MCP notifications are callable from any thread and land on the
Qt thread.

**Signal wiring (`_setup_connections`, 420-511).**
MainWindow -> handler: `record_toggled`->`_on_record_toggled` (uses QR selected cleanup
profile id); `quick_record_tab.profiles_requested`->`open_cleanup_profiles`;
`quick_record_tab.settings_requested`->`open_settings_destination`;
`record_canceled`->`cancel_recording`; `model_changed`; `whisper_engine_changed`;
`remote_model_selected`; `host_model_selected`; `remote_runtime_selected`;
`remote_retry_requested`; `live_preview_changed`; `cleanup_toggled`->
`CleanupAgentOffer.maybe_offer`; `settings_requested`; `guided_setup_requested`;
`engine_help_requested`; `settings_destination_requested`; `hotkeys_requested`;
`about_requested`; `check_for_updates_requested`; `update_notice_requested`->
`show_found_update`; `retranscribe_requested`; `upload_file_requested`;
`upload_files_requested`; `upload_cancel_requested`; `upload_copy_requested`;
`quick_record_copy_requested`; `meeting_dashboard_requested`; `past_meeting_requested`;
`past_meeting_copy_requested`; `past_meeting_delete_requested`;
`past_meetings_clear_requested`; `scratchpad_requested`; `stats_requested`;
`screen_created`; `walkthrough_requested`; `walkthrough_library_requested`;
`walkthroughs_changed`. Callbacks: `main_window.on_show_copied_animation`,
`main_window.confirm_quit = _confirm_quit_while_busy`.
Tray: `exit_requested`, `toggle_recording`, `meeting_toggle_requested`,
`meeting_dashboard_requested`, `copy_last_original_requested`, `walkthrough_requested`,
`screenshot_requested`, `scratchpad_requested`, `stats_requested`, `update_requested`,
`language_menu_requested`.
Overlay: `language_cycle_requested`, `walkthrough_snap_requested`,
`walkthrough_finish_requested`.
Meeting tab (`_connect_meeting_tab`, 535-567): `start_requested(cloud, intent)`,
`setup_requested`, `demo_requested(cloud)`, `pause_requested`, `resume_requested`,
`end_requested`, `open_dashboard_requested`, `open_report_requested`,
`copy_guest_link_requested`, `cloud_toggled(bool)`, `ai_settings_requested` ->
Settings `MEETING_INTELLIGENCE`, `retry_insights_requested`, `retry_speakers_requested`,
`retry_step_requested(step_id)`, `background_requested`, `defer_insights_requested`.
Walkthrough tab: `review.bind(copy_files, recover, acquire, release, submit)` with
lambdas that read the callbacks at use time (the app controller sets them later).

**Deferred screens.** `main_window._screen_widgets` dict holds built screens; `upload`,
`meeting`, `host`, `walkthrough` are built on first visit. `_update_screen(key, method,
*args)` forwards through `main_window.update_screen` which queues values for unbuilt
screens (test_deferred_screens.py). Code paths that must not build a screen check
`_screen_widgets.get("upload")` (864, 1766). A fallback branch handles a legacy window
without the dict (573-574) — stand-in only; the port needs only the deferred path.

**State machines.**
- Overlay routing (A03-31..34): states from `OverlayState` plus
  `streaming_flow_active` and the "is upload job" predicate
  (`_upload_job_active`: source tab is Upload, upload screen built, and
  `upload_file_tab.is_transcribing`).
- Cancel animation timer (A03-33).
- Settings staged build (A03-62): Idle -> Delayed(2500) -> Building(step every 60 ms,
  retry 1000 ms when busy) -> Adopted; or Discarded on error; or Finished by an opening.
- Guided-setup offer (A03-59).
- Update: `_update_dialog` present/absent, `_update_canceled`, `_last_update_result`;
  hand-off flow (A03-80..82).
- Meeting: `_meeting_active`, `_meeting_urls`, `_meeting_setup_open`.

**Constants.** `HANDOFF_EXIT_GRACE_S = 10.0`, `CODE_CHANGE_POLL_MS = 10_000`,
`SETTINGS_BUILD_DELAY_MS = 2500`, `SETTINGS_BUILD_GAP_MS = 60`,
`SETTINGS_BUILD_RETRY_MS = 1000`, guided setup 750 / 1000 ms, mac helper poll 100 ms,
config `CANCELLATION_ANIMATION_DURATION_MS = 1200`, `CANCELLATION_GRACE_MS = 200`,
`AUTO_PASTE_CLIPBOARD_RESTORE_DELAY_MS = 250`, `COMMAND_SELECTION_TIMEOUT_MS = 700`.

**UI owned directly by this file** (other widgets are mapped in a07/a08/a11/a16/a31):
- Download progress `QProgressDialog` (A03-65): parent main window; title "Downloading
  model"; label `Downloading "{name}"…` / `… {done} of {total}`; range (0,0) busy or
  0..100; no cancel button; min width 360; min duration 0; no auto close/reset. No
  objectName or stylesheet properties set (inherits app stylesheet).
- Message boxes (all parented to main window, app stylesheet): "Update failed" warning;
  "End meeting?" question; "Meeting Error" warning; "Meeting Recovery" non-modal warning
  with "Retry scan"/"Later"; "Quit OpenWhisper?" and "Install update?" questions (Yes/No,
  default No); "Restart OpenWhisper" info/warning with optional "Don't show this again"
  checkbox and detailed text, buttons "Restart now" (default) / "Later"; "About
  OpenWhisper" rich-text about box.
- No objectNames, dynamic properties, animations, shortcuts or tooltips are defined in
  this area; animations belong to the overlay (a07).

**Edge cases to keep (with lines).**
- Tray Exit must go through `main_window.request_quit()`; a bare quit is vetoed by
  minimize-to-tray closeEvent (657-659).
- Language submenu populated once at start because some platforms won't open an empty
  submenu (495-496).
- `_apply_agent_changes` re-reads saved settings, not the queued values (587-590).
- `set_engine_busy(False)` refreshes model views so the Downloads delete lock follows the
  newly loaded model (840-841).
- Error display never puts the error in the transcript box (697-702).
- `clear_transcription_stats` must not build the upload screen (863-866).
- `show_notice` suppressed when main window active (998).
- Cancel animation end leaves a newer state alone (1465-1469).
- `start_snap` keeps overlay out only if it is a top-level window (Wayland parented
  overlay is not) (1113).
- Walkthrough panel stands in for the overlay only while it shows the walkthrough's own
  recording and no AI write-up is running (1173-1181).
- Library dialog refresh only when not busy and not dirty (1235, 1310, 1328).
- `on_update_check_finished` never replaces the result shown by an open dialog (2555-2560);
  a failed check keeps the previous release (2561-2562).
- `show_app_update_dialog` clears `_update_dialog` only if it is still the same dialog
  (2611).
- Cancel after the dialog closed does not pop "Update failed" (163-167).
- `exit_for_update` uses `exit(0)` not `quit()` (windows veto quit) and arms the 10 s
  hard-exit watchdog (2750-2767).
- Setup-installer path releases the single-instance mutex before launching setup and
  reacquires it on launch failure (225, 233).
- Restart prompt asked once per run; after Later only a failure asks again (2786).
- `_on_meeting_end_requested` no-op unless meeting active and callback set (2218).
- Recovery scan error dialog reused while visible (2392-2396).
- `_open_meeting_setup` always resets `_meeting_setup_open` in `finally` and
  `deleteLater`s the dialog.

## Data and formats

Settings keys read or written directly by this area (file `openwhisper_settings.json`,
via `services.settings.settings_manager`; OpenWhisper_CPP must read the same names if it is to
import an existing install):

| Key | Type | Default | Where |
|---|---|---|---|
| `selected_model` | choice | `config.DEFAULT_BACKEND` (`parakeet_mlx` on Apple MLX, `parakeet` where supported, else `local_whisper`) | `load_model_selection` in `_setup_transcription_backends` |
| `whisper_model` | str | `"auto"` | written by `_on_manager_set_active` (1894) |
| `ui_theme` | choice dark/light/system | `"dark"` | `_apply_agent_changes` |
| `ui_font_scale` | int choice | 100 | `_apply_agent_changes` |
| `streaming_enabled` | bool | False | `_apply_agent_changes` |
| `streaming_overlay_font_size` | int 10-48 | 16 | `_apply_agent_changes` |
| `streaming_overlay_layout` | choice | `"header"` | `_apply_agent_changes` |
| `recording_overlay_look` | choice | `"bars"` | `_apply_agent_changes` |
| `recording_overlay_dot` / `_text` / `_clock` | bool | True | `_apply_agent_changes` |
| `transcript_cleanup_enabled` | bool | False | `_apply_agent_changes` |
| `transcript_cleanup_level` | choice | `"medium"` | `_apply_agent_changes` |
| `recording_trigger_mode` | choice | `"toggle"` | `_apply_agent_changes`, `_on_settings_recording_trigger_mode_changed` (save fallback) |
| hotkeys (`record_toggle`, `cancel`, `enable_disable`, `minimize_tray`, `walkthrough`, `screenshot`, ...) | dict | `config.DEFAULT_HOTKEYS` | `update_hotkey_display`; `save_hotkey_settings` fallback |
| `update_restart_prompt_enabled` | bool | True | `_on_code_change_poll`, written False by checkbox |
| `walkthrough_minimize_to_tray` | bool | False | `set_walkthrough_active` |
| update notify settings | various | — | `shows_update_notice(status, settings)` (a16) |
| meeting LLM profile | — | — | `resolve_meeting_llm_profile` for consent copy |

Database: no direct table access; `db.ensure_initialized()` on startup and `db.close()` at
shutdown (`openwhisper.db`, a09/a28).

Files / process contracts:
- Exit codes: `0` normal; `RESTART_EXIT_CODE` (services/backup_startup.py) -> main.py
  relaunches after bootstrap closes data writers (used by restore and restart prompt).
- Updater: `updates_dir()/setup.log`; Inno Setup flags listed in A03-80; native helper
  argv from the update journal (a16). The helper waits for this exact PID to exit.
- Application single-instance mutex released/reacquired around setup launch.
- Metrics: `services.diagnostics.record_metrics(shutdown_s=...)`.
- Events: `transcription.preview` (via `TranscriptionRuntime.publish_preview`) throttled
  to 4/s — consumed by automation/MCP (a19/a20).

## External dependencies

| Python | Used for | C++ / Qt 6 |
|---|---|---|
| PyQt6 QtCore (`QObject`, `pyqtSignal`, `QTimer`, `QUrl`) | signals, timers | QtCore, same classes; queued connections across threads |
| PyQt6 QtGui `QDesktopServices` | open URLs / disk image | `QDesktopServices::openUrl` |
| PyQt6 QtWidgets (`QApplication`, `QMessageBox`, `QProgressDialog`, `QCheckBox`) | dialogs, exit | same |
| `concurrent.futures.ThreadPoolExecutor` | 5 executors | `QThreadPool` instances with `setMaxThreadCount`, or a small `fv::SerialExecutor` (std::thread + queue) where shutdown semantics (cancel queued, wait, drain-without-cancel) must be exact |
| `threading` (RLock, Lock, Event, Thread, Timer) | locks, daemon threads, watchdog | `std::recursive_mutex`, `std::mutex`, `std::atomic_bool`/`std::condition_variable`, `std::thread` detached; watchdog = detached thread sleeping 10 s then `std::_Exit(0)` / `TerminateProcess` |
| `os._exit` | hard exit | `std::_Exit(0)` |
| `time.monotonic`, `perf_counter` | throttle, metrics | `std::chrono::steady_clock`, `QElapsedTimer` |
| `contextlib.contextmanager` | backup record context | RAII guard |
| `logging` | logs | project logger (a01) |
| `openai`, `av` (PyAV) imports | warm-up only | none (drop) |

## Platform-specific code

- **Windows**: setup installer fallback (Inno flags, HKLM vs HKCU scope via
  `discover_install_registration`), app mutex release/reacquire; focus-context COM
  shutdown in `_stop_playback_and_focus_capture`; default hotkeys `kp *`, `kp -`,
  `ctrl+alt+kp *`, `ctrl+alt+m`.
- **macOS**: dock icon sync (269-273); DMG / staged helper update (181-192, 2716-2748);
  system-audio permission in meeting readiness; snap access request (Screen Recording);
  default hotkeys `ctrl+alt+r`, `ctrl+alt+escape`, `ctrl+alt+shift+r`, `ctrl+alt+m`.
- **Linux**: overlay parented to main window on Wayland (`is_wayland()`, 264-266), which
  also changes `start_snap` keep-out; D-Bus shell record toggle source `"dbus"`
  (`_on_shell_toggle_recording`, Omarchy); Linux meeting system-audio remediation dialog
  (2110-2116); walkthrough panel is None on Wayland (1194).

## Engine-specific code

Drop (non-Parakeet or Python-process specific):
- `LocalWhisperBackend` construction and the whole `_reload_whisper_worker`,
  `local_whisper_loading_message`, Whisper branches of `transcription_readiness_message`
  and `notify_main_ui_ready`, `gpu_fallback_detected` from Whisper load
  (593-633, 696-706, 719-729, 851-860).
- `_release_unselected_engines` over many engines collapses to: release remote client
  and/or anything not selected (only Parakeet, maybe Nemotron, remote).
- `close_all_workers()` (worker processes, 1390-1391), `_preload_recording_reader`
  (PyAV), `_warm_openai_sdk` / `_import_openai_sdk`.
- `SPEECH_WARMUP_BACKENDS` entries other than `parakeet` (and `nemotron` if kept).
- `_on_manager_set_active` writes `whisper_model` — replace with the Parakeet model key.

Must adapt to in-process Parakeet:
- Reload worker calls `Recognizer` load on `EngineWorker`'s thread, not on the shared
  executor: OpenWhisper_CPP's rule is that the engine is reached only through queued signals to
  `EngineWorker`. The reload state machine stays in the controller; the actual load,
  warm-up and transcribe are queued requests to the worker, completion comes back as
  signals. `_engine_lock` becomes "only the engine thread touches the Recognizer".
- `release_local_engine` must free the Parakeet model (the runtime library stays loaded:
  never `FreeLibrary`).
- Warm-up: one short non-silent decode after load (test_engine_warmup.py pins: skipped if
  unloaded, non-fatal, never queued behind another decode, ends quietly on cleanup).
- Readiness "closed by cancel / never loaded -> reload" branch maps to Recognizer state.
- `cpu_fallback_note` / `switch_error` / `device_info` strings come from the engine wrapper
  (a04/a14 own the wording).
- `models.runtime_consent_requested` -> offer to install the NeMo-Speech.cpp CPU/GPU
  runtime component.
- Live preview shares the loaded Parakeet engine (windows decoded with `transcribe`);
  setup must follow load completion (`_pending_streaming_setup`).

Undecided backends (map kept, port behind a feature flag): API (`api` backend,
`on_progress`, large-file split notice A03-36, API-key readiness message), Remote engine
(RemoteSpeechBackend, RemoteEngineService host, record sync, pairing prompter, host
dashboard, remote models dialog, A03-15/47, remote branches of reload finish), Nemotron
(same runtime; would join the warm-up list and native-stream preview).

## Dependencies on other areas

Calls out (imports):
- a01 app shell / bootstrap: constructs `UIController` then `ApplicationController(ui,
  local_backend)`, calls `setup_backup`, `notify_main_ui_ready`, `offer_guided_setup`,
  `build_settings_when_idle`, `show_apply_error_if_any`, `cleanup`; relaunch on
  `RESTART_EXIT_CODE`.
- a02 ui-foundation: `apply_ui_font_scale`, `apply_ui_theme`, `TabbedContentWidget` tab
  constants, `ui_qt.utils.desktop.is_wayland`, macOS dock.
- a04 runtime services: `HotkeyRuntime`, `StreamingRuntime`, `TranscriptionRuntime`
  (job model, `_job_lock`, `_capture_lock`, `writing_up`, `publish_preview`, cancel),
  `CommandRuntime`, `ModelRuntime`, `ComponentRuntime`, `UpdaterRuntime`, `RemoteRuntime`,
  `dictation_pipeline.JobMode`.
- a05 recording-audio: `AudioRecorder.from_settings`, `error_callback`,
  `device_switch_callback`, `audio_player`.
- a06 hotkeys-paste-focus: `format_hotkey_display`, `send_paste`, `synthetic_keys.send_copy`,
  `focus_context`, `ui_qt.clipboard.TemporaryClipboard`/`ClipboardStageResult`.
- a07 overlays: `WaveformOverlay` (STATE_* constants, `show_at_cursor`, `set_state`,
  `current_state`, notices, captions, streaming text, walkthrough controls, stand-in,
  refresh look/font/layout, `timer`).
- a08 record/upload tabs: `MainWindow`, Quick Record and Upload tabs,
  `stage_for_overlay_state`.
- a09 history/stats: `refresh_history`, `history_actions`, `stats_dialog`, `db`.
- a10 settings services: `settings_manager`, `SettingsKey`, resolvers,
  `seed_new_install_settings`, `credentials`, `dictionary`.
- a11 settings dialog UI: `SettingsDialog` + destinations, `settings_dictionary`,
  `settings_destinations`, `hf_consent_dialog`, `required_runtime_dialog`,
  `use_gpu_dialog`, `guided_setup`.
- a12/a13 text AI / personalize: `CleanupAgentOffer`, `cleanup_profiles.cleanup_may_run`,
  scratchpad, language menu, `dictation_language`.
- a14/a15 models/components: `component_coordinator`, downloads pages.
- a16 updater: `app_update`, `app_update_apply`, `app_update_macos`, `update_contract`,
  `code_identity.changed_since_launch`, `AppUpdateDialog`.
- a17/a18 remote engine: `RemoteSpeechBackend`, `RemoteEngineService`, `record_sync`,
  `PairRequestPrompter`, `RemoteModelsDialog`, host dashboard.
- a19/a21 actions/automation: `ActionRuntime`, `automation.service`, `actions.routing`.
- a20 agent API / MCP: emits `agent_data_changed`.
- a22/a23 walkthrough: `WalkthroughRuntime`, snap, live annotations, walkthrough panel,
  ready popup, paste, library dialog, `services.walkthrough.library`.
- a24-a31 meeting: `MeetingRuntime`, meeting dialogs (unsupported, setup, system audio,
  Linux audio, consent, recovery), `meeting.platform`, `services.meeting_setup`.
- Backup: `services.backup_manager.BackupCoordinator`, `backup_startup`.
- Diagnostics: `services.diagnostics.record_metrics`.

Called in by: bootstrap (a01), every runtime in a04 (through `controller.ui_controller`
and the controller signals), hotkey manager (a06), MCP/automation (a19/a20), meeting
runtime (a24), walkthrough runtime (a22), D-Bus/Omarchy shell (a06/a01).

## Tests

| Test file | What it pins for this area |
|---|---|
| tests/test_application_controller.py | Broad: backup/walkthrough-library reservations and their messages, restore restart, model switch and reload in background, refusals while recording/meeting, minimize hotkey on main thread, streaming preview sharing the Parakeet engine, upload/large-file routing, start-refused stays idle, transcribe_clip delegation/errors, meeting refusals and rollbacks, engine release/restore idempotence, `cleanup` safe with partial state, startup DB deferral and failure, readiness messages before capture, optional-engine switch releases others, history drained on quit. Many tests use stand-in controllers (`__new__` + bound methods), which is why helpers live outside the class (1433-1435). |
| tests/test_controller_thread_hops.py | readiness check off Qt thread really reloads and `_engine_settled` is false before the timer arms; Qt-thread request arms at once; worker signals delivered on Qt thread; busy notice shown on Qt thread. |
| tests/test_engine_warmup.py | Reload -> warm -> busy cleared -> preview setup ordering; failed warm-up/load/missing model/runtime; engine switched during load; lease during warm-up; lease returned during warm-up reloads; lease held asks for no runtime. |
| tests/test_app_update_handoff.py | `exit_for_update` exits instead of asking; failed launch keeps app; cancel after close no failure box; tray exit through window; quit mid-task asks; setup fallback flags; mutex reacquire; watchdog forces stalled exit; normal exit leaves no holder; mac DMG. |
| tests/test_update_notice.py | notice from automatic check, manual check dialog, failed check keeps release, up-to-date withdraws, notify setting, dialog-open check leaves result alone. |
| tests/test_update_resilience.py | (mostly a16) `test_stale_attempt_cannot_change_current_dialog`, `test_ui_cancel_after_success_is_queued_keeps_app_open`, `test_new_work_at_download_completion_can_decline_restart`. |
| tests/test_code_change_restart.py | poll asks once, don't-show-again mutes, error offer has no checkbox, restart confirms busy work, restart exit code, meeting error after change offers restart. |
| tests/test_meeting_start_readiness_seams.py | readiness forwarded once, cancel starts nothing, mic-only -> "disabled", demo uses only platform ack, tray end requires confirmation, Linux path after setup. |
| tests/test_meeting_setup.py | `test_first_start_runs_shared_setup` (controller opens setup on first start). |
| tests/test_settings_unified.py | staged Settings build: piece per tick, waits for recording/meeting, opening mid-build finishes same window, failing piece discards; destination aliases (`text`, `engine_downloads`, legacy names). |
| tests/test_deferred_screens.py | controller startup keeps inactive screens unbuilt; host dashboard gets queued values. |
| tests/test_mcp_desktop_changes.py | `_apply_agent_changes` on GUI thread with latest values; theme resolution; preview/cleanup/trigger refresh. |
| tests/test_live_preview_control.py | tab toggle reconfigures streaming; Settings toggle refreshes tabs then reconfigures. |
| tests/test_personalize_fix3_h1_outcome_notices.py | `show_notice` suppressed with main window active; notices during recording/job. |
| tests/test_overlay_moments.py | STT on/off and copied moments, cancel animation from hidden. |
| tests/test_cancel_after_stop.py | cancel semantics through the controller (`cancel` routing). |
| tests/test_automation_*cancel*.py, test_automation_fix*_pipeline*.py | Cancel falls through to actions, `NOTHING_TO_CANCEL`, `transcription.preview` throttling via `PreviewEvents`. |
| tests/test_guided_setup.py | controller offer opens on welcome, menu opens first step, singleton, offer waits for startup modal and visible main window. |
| tests/test_walkthrough_*.py, test_personalize_*.py, test_remote_*.py, test_host_mode.py, test_stats_features.py | import the controllers as fixtures; their pinned behaviour is mostly in other areas. |

Golden-vector candidates (pure logic, replayable):
- `PreviewEvents` timing: sequence of `(t, text, final)` offers -> sequence of sends with
  timestamps (inject clock).
- `transcription_readiness_message` decision table: (backend kind, available,
  reload_in_flight, model_missing, should_cancel, last_error, remote restoring, host
  name) -> message + side effects (reload requested / download consent).
- Reload state machine traces: events (request, timer fire, recording on/off, meeting
  on/off, lease release/restore, load done, warm done) -> emitted signals sequence.
- `set_overlay_state` routing table: (state, upload job active, has stage, streaming flow,
  overlay visible, current state) -> overlay call(s), tray recording flag, tab calls.
- `_acquire_backup` / `_acquire_walkthrough_library` gate tables -> message.
- `_refuse_dictation_during_meeting` gate order.
- `_update_work_is_busy` order and `on_meeting_state_changed` payload -> effects.
- `_setup_installer_command` argv given (registration hive, app dir, updates dir).

## Porting notes

**Decomposition.**
- Qt-free core (`src/engine`-style, or a new `src/core` with no Qt):
  `OverlayState` enum; `PreviewThrottle` (clock-injected); `EngineReloadMachine` (pure
  state machine emitting commands: arm timer, start load, warm, finish, ask runtime,
  set busy, status text); readiness-message function; gate functions (dictation refusal,
  backup acquire, library acquire, busy reason) over a plain `AppState` snapshot;
  installer argv builder. All golden-vector testable.
- Qt layer (`src/app`): `ApplicationController` (QObject; owns executors, runtimes,
  `EngineWorker`, signals, shutdown order); `UiController` (QObject; owns MainWindow,
  overlay, tray, clipboard, dialogs; overlay router; settings opener; download fan-out;
  meeting glue; update hand-off). Replace the ~75 assignable `on_*` callbacks with either
  Qt signals on `UiController` connected by `ApplicationController`, or a struct of
  `std::function` filled once — keep "read at call time" semantics for those set after
  construction (remote engine, walkthrough library, settings changed).
- Split `UiController` into helpers to keep files reviewable: `OverlayRouter`,
  `SettingsOpener` (staged build + destinations + bindings), `DownloadProgressPresenter`,
  `MeetingUiGlue`, `UpdateHandoff`, `WalkthroughUiGlue`, `ClipboardService` wrapper.

**Order of work.**
1. `OverlayState`, gate functions, readiness message, preview throttle (core + tests).
2. `ApplicationController` skeleton: executors, signals, Parakeet backend only, reload
   state machine driving `EngineWorker`, start/stop/cancel, startup and shutdown order.
3. `UiController` core: main window, overlay routing, status/transcript routing, tray
   wiring, clipboard service, notices.
4. Settings opener + staged build, guided setup offer, model download/component fan-out.
5. Meeting glue, walkthrough glue, scratchpad/language/history hooks (as those areas land).
6. Update hand-off + watchdog, about box, quit confirmation, restore restart.
7. Undecided backends behind flags.

**Risks.**
- Thread model mismatch: Python runs loads on executor threads; OpenWhisper_CPP mandates the
  engine thread. The reload/lease/warm-up interleavings (test_engine_warmup.py) must be
  re-derived for queued requests, especially "lease returned during warm-up".
- Lock order in `_acquire_backup` across three runtime locks; C++ needs `std::scoped_lock`
  of all three to avoid deadlock.
- Runtimes reaching into controller privates (`_job_lock`, `_capture_lock`,
  `_active_job`, `meeting._background_engines`, etc.): needs explicit interfaces.
- Very wide fan-out surface; a missed connection silently breaks a feature. Use this map's
  wiring lists as a checklist and add a test that every MainWindow/tray/overlay signal is
  connected.
- Shutdown ordering and "drain history without cancel" must hold with QThreadPool
  (which cannot cancel running tasks) — prefer custom serial executors.
- Hard exit after hand-off must not run static destructors that touch the NeMo runtime
  (never unload; `std::_Exit`).

**Rough C++ size.** ~3,500 lines (application controller ~1,200, UI controller ~1,900,
core logic + overlay enum ~400), excluding dropped engine paths; plus ~800 lines of tests.

**Native gains.**
- No SDK/PyAV warm-up threads; startup work (credential store, component listing) is
  cheap in C++ and can mostly run inline after first paint.
- Settings staged build exists only because PyQt construction is ~10x slower than show;
  in C++ Qt the full build may be fast enough to drop staging (measure; keep the API).
- Code-change restart prompt exists for a Python app updated in place; a compiled binary
  only needs it if the updater replaces files while running.
- Shutdown can be faster: no interpreter teardown, no worker processes; the 10 s hand-off
  watchdog stays as a safety net.
- Reload debounce and warm-up remain useful (Parakeet first decode cost is real).

## Open questions for the owner

- Keep the "code changed on disk" restart prompt (A03-83) in a compiled app, or drop it?
- Keep the About text and website, or new OpenWhisper_CPP wording/URL; and the install-channel
  line?
- Updater: keep native helper + Inno setup fallback flow as-is (a16), or simpler?
- Remote engine / API / Nemotron: needed for the controller's backend table at all?
- Should OpenWhisper_CPP read/write the same settings keys and exit codes so an OpenWhisper
  install can be migrated in place?
