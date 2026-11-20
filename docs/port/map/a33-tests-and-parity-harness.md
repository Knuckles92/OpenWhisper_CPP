# a33 - OpenWhisper's tests and the parity harness

How OpenWhisper's test suite and the Python app can prove that OpenWhisper_CPP
behaves and looks the same, with the two mechanisms (golden vectors and
reference screenshots) built and run on this machine.

Reference checkout: `D:\coding\whisper_local` at commit `768b559edd93`, run
with `D:\coding\whisper_local\.venv\Scripts\python.exe` (Python 3.12.10,
PyQt6 6.11.0, which bundles Qt 6.11.0). Everything created for this map is in
`C:\coding\OpenWhisper_CPP\out\parity-scratch\` (git-ignored by `/out/`):

| File | What it is |
|---|---|
| `classify_tests.py` | Classifies every test file by area and kind (Part 1) |
| `classification.json`, `classification.md` | Its output: one row per test file; the tables below |
| `coverage_table.py` | Test density per area, from `classification.json` |
| `ow_isolation.py` | Imports OpenWhisper without touching its data (shared by the harness scripts) |
| `golden_vectors.py` | Golden vectors from curated inputs |
| `trace_plugin.py` | pytest plugin: golden vectors from the inputs OpenWhisper's own tests use |
| `golden/*.json` | 113 curated cases (4 modules) and 72 traced calls |
| `replay/replay_format_utils.cpp` | C++ replay of `services.format_utils` vectors |
| `render_reference.py` | Offscreen reference screenshots plus widget-tree, stylesheet and palette dumps |
| `screens/` | Reference PNGs and their pin sidecars (`.json`) |
| `replay/render_tree.cpp` | Rebuilds a dumped widget tree in C++ Qt 6.10.3 and diffs it pixel by pixel |

## 1. How the suite is organized and run

**Size.** `tests/` holds 548 `test_*.py` files (157,451 lines, about 7,770
`def test_` functions before parametrization) plus `conftest.py` (481 lines),
`helpers.py`, `legacy_settings_77b63c1.py` (a frozen copy of an old settings
module for migration tests), `pdf_reader.py`, and `fakes/`.

**Naming.** Files are flat, no subfolders. Many are named after a work stream
rather than a module: `test_automation_*` (86 files, Actions/hooks/MCP/API),
`test_personalize_*` (Styles, Dictionary, Snippets, Commands, the
`fix_f*`, `fix2_g*`, `fix3_h*` follow-up rounds), `test_meeting_*`,
`test_walkthrough_*`, `test_snap_*`, `test_remote_*`. The rest are named after
the module under test (`test_format_utils.py`, `test_recorder.py`).

**Runner.** pytest 9.1.1 with pytest-xdist 3.8.0 (`pyproject.toml`:
`testpaths = ["tests"]`, one marker, `qt`). CONTRIBUTING.md: run through pytest,
never bare, because the fixtures isolate data, manage Qt and handle native
teardown:

```
uv run --locked python -m pytest tests/ -n auto
uv run --locked python -m pytest tests/test_recorder.py tests/test_settings.py
```

CI (`.github/workflows/ci.yml`):

- `python` (ubuntu-22.04): ruff `--select F`, mypy on the settings layer, then
  `timeout 20m xvfb-run -a -s "-screen 0 1280x1024x24 -noreset" python -m pytest -n auto -v --durations=20 -o faulthandler_timeout=120`
  with `QT_QPA_PLATFORM=offscreen`.
- `macos` (macos-15) and `windows` (windows-2022): the whole suite, same
  command, `QT_QPA_PLATFORM=offscreen`. Windows also runs the OpenCode sidecar
  checks and builds that component.
- `desktop-core`: a fixed list of ~45 cross-platform lifecycle files
  (recorder, journal, cancellation, backup, meeting recovery, walkthrough
  storage ...) on Python 3.11 and 3.12 with source and release dependency
  pins, then `scripts/check_release_health.py` (startup timing and clean exit).
- `webui` (`npm test` + bundle freshness) and `sidecar` (`npm test`): the
  TypeScript areas a27 and a30 are tested there, not in pytest.

**Shared infrastructure.**

- `conftest.py` (details in section 3) decides per test file whether it needs
  Qt by following its imports: any reference to `ui_qt` or `PyQt6`, directly
  or through local imports, adds the `qt` marker. Qt tests get one session
  `QApplication` on the offscreen platform with `QT_QPA_FONTDIR` set to
  `%WINDIR%\Fonts` on Windows and OpenWhisper's `WidgetStyleFilter` installed,
  but no `theme.qss` (unthemed unless a test asks for `isolated_app_theme`).
  Widgets a test created are destroyed after it. `pytest_unconfigure` ends the
  process with `os._exit` so native teardown cannot turn a pass into an access
  violation.
- `helpers.py`: `write_wav`, `make_meeting`, `make_segment`,
  `patch_setting_defaults` (other platforms' defaults).
- `fakes/`: `settings.InMemorySettings`; `http_response.Response` (urllib);
  `remote.py` (a real remote host on 127.0.0.1 with a fake engine);
  `meeting_web.py` (repository and engine behind the web API);
  `scheduler.py` and `agent_core.py` (meeting agent seams);
  `personalize.py` (Settings dialog on a throwaway store);
  `fake_agent_cli.py` and `fake_acp_agent.py` (stand-ins for `claude -p`,
  `codex exec`, `opencode acp` that speak MCP/ACP on stdio).

**Style of the tests.** Mostly behavioral: build the real object with fakes at
the seams (settings, HTTP transport, clock, audio device, subprocess),
drive it, assert on outputs and on what crossed the seam. Qt tests construct
the widget, poke it (`click()`, `setText`, `QTest`), and assert on texts,
visibility, enabled state, sizes and sometimes geometry (`window_fit_*`,
`desktop_dialog_fit`). There are no image snapshots anywhere in the suite.

## 2. Classification

Generated by `classify_tests.py` (deterministic; reruns give the same JSON).

**Area rule.** Every repository module a test references is resolved to a
file: AST imports, dotted strings such as monkeypatch targets
(`"services.recorder.sd"`), and `"name.py"` literals for modules loaded by
path. Files are looked up in `docs/port/areas.json`. The primary area is the
one whose module name matches the test file name (`test_recorder_postroll` ->
`services/recorder.py`), else the area with the highest import weight, where
each module counts `1 / (number of test files importing it)` so `config.py`
and `services/settings.py`, imported almost everywhere, do not decide. Eleven
files are hand-corrected (listed in `OVERRIDES` in the script). "Also touched
by" counts files whose secondary imports reach the area.

**Kind rule** (first match wins):

| Kind | Rule | Port as |
|---|---|---|
| dropped-engine | imports a module that exists only for faster-whisper, Moonshine, Qwen, Parakeet MLX, the Unified "transcribe" engine or the isolated worker process, or its file name names one, or mentions them 8+ times | not ported; mine for behavior only where it also covers kept code |
| platform | the file name names a platform backend (`_mac`, `_win`, `x11`, `omarchy`, `wayland`, `hyprland`, `carbon`, `pynput` ...), or 5+ references to one, or a module-level platform skip, or platform skips on half its tests | per-platform C++ tests behind `if constexpr`/`#ifdef`; Windows first |
| qt-widget | references `PyQt6` or `ui_qt` | Qt Test (some are pure functions that live under `ui_qt`, see below) |
| integration | fakes, sockets, subprocess, threads, `TestClient`, `MockTransport`, a SQLite `db`/`repo`, mocks/patching | Catch2 or Qt Test with C++ fakes at the same seams |
| pure-logic | none of the above | golden vectors and direct Catch2 tests |

The kinds are conservative and file-level: a "qt-widget" or "integration" file
often contains pure tests too. Known misfiles to keep in mind:
`test_fuzzy_match`, `test_overlay_position`, `test_markdown_render`,
`test_snap_geometry`, `test_walkthrough_document` are pure functions under
`ui_qt` (counted qt-widget); `test_settings` is counted dropped-engine because
it exercises Whisper model settings heavily, but most of it is about settings
that stay; `test_local_text_servers` and `test_speech_endpoints` are cloud/LAN
text and speech endpoints (undecided, not dropped) caught by the keyword rule.

### Summary

| Area | Files | Tests | pure-logic | integration | qt-widget | platform | dropped-engine |
|---|---:|---:|---:|---:|---:|---:|---:|
| a01-app-shell | 18 | 145 | 5 | 1 | 9 | 3 | 0 |
| a02-ui-foundation | 26 | 197 | 0 | 0 | 24 | 2 | 0 |
| a03-ui-controller | 3 | 159 | 0 | 0 | 1 | 0 | 2 |
| a04-runtime-services | 17 | 414 | 3 | 6 | 5 | 1 | 2 |
| a05-recording-audio | 22 | 245 | 9 | 11 | 1 | 0 | 1 |
| a06-hotkeys-paste-focus | 40 | 431 | 8 | 7 | 13 | 12 | 0 |
| a07-overlays | 9 | 130 | 0 | 0 | 9 | 0 | 0 |
| a08-record-upload-tabs | 12 | 234 | 1 | 0 | 11 | 0 | 0 |
| a09-history-stats | 26 | 248 | 3 | 4 | 19 | 0 | 0 |
| a10-settings-services | 16 | 251 | 8 | 1 | 6 | 0 | 1 |
| a11-settings-dialog-ui | 6 | 66 | 0 | 0 | 6 | 0 | 0 |
| a12-text-ai-personalize-services | 34 | 602 | 11 | 9 | 10 | 1 | 3 |
| a13-personalize-ui | 18 | 201 | 0 | 0 | 15 | 2 | 1 |
| a14-models-components-services | 33 | 482 | 6 | 5 | 1 | 2 | 19 |
| a15-models-downloads-ui | 9 | 283 | 0 | 0 | 5 | 0 | 4 |
| a16-updater-setup-packaging | 19 | 360 | 1 | 7 | 6 | 5 | 0 |
| a17-remote-engine-services | 25 | 285 | 4 | 11 | 10 | 0 | 0 |
| a18-remote-engine-ui | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| a19-actions-automation-services | 83 | 858 | 27 | 39 | 16 | 1 | 0 |
| a20-agent-api-mcp-services | 7 | 70 | 0 | 5 | 2 | 0 | 0 |
| a21-actions-automation-ui | 7 | 132 | 0 | 0 | 7 | 0 | 0 |
| a22-walkthrough-services-snap | 21 | 480 | 2 | 0 | 16 | 3 | 0 |
| a23-walkthrough-ui | 5 | 77 | 0 | 0 | 5 | 0 | 0 |
| a24-meeting-engine | 16 | 260 | 5 | 11 | 0 | 0 | 0 |
| a25-meeting-capture-asr-diarize | 18 | 304 | 7 | 6 | 0 | 2 | 3 |
| a26-meeting-agent | 14 | 227 | 2 | 11 | 0 | 0 | 1 |
| a27-meeting-sidecars-ts | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| a28-meeting-state-persist-export | 12 | 234 | 4 | 8 | 0 | 0 | 0 |
| a29-meeting-intelligence | 7 | 110 | 1 | 6 | 0 | 0 | 0 |
| a30-meeting-web-server-and-dashboard-contract | 4 | 19 | 0 | 4 | 0 | 0 | 0 |
| a31-meeting-qt-ui | 13 | 194 | 0 | 0 | 12 | 1 | 0 |
| unmapped: benchmarks/ | 3 | 27 | 2 | 1 | 0 | 0 | 0 |
| unmapped: scripts/ | 3 | 37 | 1 | 1 | 0 | 1 | 0 |
| unmapped: no repo import | 2 | 4 | 1 | 0 | 1 | 0 | 0 |
| **total** | **548** | **7766** | **111** | **154** | **210** | **36** | **37** |

a18 (remote engine UI) has no test file of its own: its widgets are exercised
from a17's `remote_*` and `host_mode` Qt tests. a27 (TypeScript sidecars) and
the dashboard half of a30 are tested with `npm test`, outside pytest.

### File list per area

Test file names without the `test_` prefix and `.py` suffix.

<details><summary>a01-app-shell (18)</summary>

- **pure-logic** (5): asyncio_utils, automation_r3c_child_env, automation_r4b_dotenv, data_root, process_env
- **integration** (1): release_health
- **qt-widget** (9): automation_fix_pipeline_sources, bootstrap, diagnostics, loading_screen, main_entrypoint, main_window_compact, personalize_fix2_g2_styles_focus, system_tray, whats_new
- **platform** (3): agent_mcp_theme, personalize_fix2_g4_wayland_selection, ui_instance

</details>

<details><summary>a02-ui-foundation (26)</summary>

- **qt-widget** (24): automation_ui_actions_fit, deferred_screens, desktop_dialog_fit, eliding_combo, eliding_label, field_help, fuzzy_match, inactive_buttons, markdown_render, nav_rail, no_wheel_spinbox, overlay_position, past_meetings_show_in_folder, personalize_fix2_g3_chip_flow, personalize_fix3_h1_command_setup, release_notes, searchable_combo, tabbed_content, title_bar, ui_font_scale, ui_theme, update_notice, window_fit_and_title_bar, window_fits_tab
- **platform** (2): macos_dock, omarchy_ui

</details>

<details><summary>a03-ui-controller (3)</summary>

- **qt-widget** (1): meeting_recovery_ui
- **dropped-engine** (2): application_controller, local_transcription_contracts

</details>

<details><summary>a04-runtime-services (17)</summary>

- **pure-logic** (3): dictation_runtime_exception_handling, hook_refresh_waits_for_hold, personalize_fix_f1_latch_no_data
- **integration** (6): automation_actions_paste_seam, cleanup_deadline, meeting_archive_dashboard, personalize_fix_f1_paste_original_keys, preview_lazy_tail, strict_switch_reads
- **qt-widget** (5): controller_thread_hops, meeting_runtime, meeting_start_readiness_seams, remote_engine, walkthrough_shortcuts
- **platform** (1): meeting_engine
- **dropped-engine** (2): personalize_s3_controller, personalize_s9_paste_original

</details>

<details><summary>a05-recording-audio (22)</summary>

- **pure-logic** (9): audio_processor, chunking, personalize_fix_f5_final_language, personalize_fix_f5_stats_spoken, personalize_s4_incremental, personalize_s8_language, personalize_s9_stats, resample, wav_metadata
- **integration** (11): cancel_after_stop, incremental_dictation, personalize_fix_f3_legacy_mic, personalize_fix_f3_lost_mic_close, personalize_s7_recorder, personalize_s9_audio_player, recorder, recorder_postroll, recording_isolation, recording_journal, streaming_transcriber
- **qt-widget** (1): capture_delay
- **dropped-engine** (1): personalize_foundation_controller

</details>

<details><summary>a06-hotkeys-paste-focus (40)</summary>

- **pure-logic** (8): hotkey_manager, personalize_fix2_g1_paste_target_tab, personalize_fix2_g4_unknown_app_paragraphs, personalize_fix_f3_console_terminal, personalize_foundation_hotkeys, personalize_s1_catalog, personalize_s1_context_text, personalize_s3_transforms_storage
- **integration** (7): automation_r2_selection, personalize_fix2_g1_rehook_hold, personalize_fix_f2_selection_blocked, personalize_fix_f2_selection_keys_held, personalize_fix_f3_reread_exclusion, personalize_s1_service, push_hold_recording
- **qt-widget** (13): accessibility_setup, clipboard, clipboard_files, hotkey_directory, hotkey_settings_page, paste_latency, personalize_fix_f2_clipboard_no_counter, personalize_fix_f2_paste_target, personalize_fix_f5_rich_join, personalize_foundation_clipboard, personalize_s5_clipboard, personalize_s6_runtime, personalize_s6_settings
- **platform** (12): hotkey_carbon, hotkey_headless, hotkey_pynput_keypad, mouse_monitor_mac, omarchy_controls, personalize_fix2_g1_linux_terminals, personalize_fix_f4_omarchy_bar, personalize_s1_identity, personalize_s3_synthetic_keys, personalize_s6_backends, personalize_s8_omarchy, x11_key_grab

</details>

<details><summary>a07-overlays (9)</summary>

- **qt-widget** (9): agent_recording_indicator, desktop_performance, overlay_moments, personalize_fix3_h1_outcome_notices, personalize_fix_f4_leftovers, personalize_s8_language_menu, personalize_s8_overlay, recording_look, streaming_overlay_layout

</details>

<details><summary>a08-record-upload-tabs (12)</summary>

- **pure-logic** (1): batch_upload
- **qt-widget** (11): automation_fix_pipeline_scratchpad, automation_r4_scratchpad_answer, batch_relation_dialog, engine_selection_sync, live_preview_control, personalize_fix2_g3_scratchpad_quit, personalize_fix3_h2_voice_languages, personalize_foundation_ui_shell, personalize_s3_quick_record_hint, transcript_viewer_dialog, upload_file_tab

</details>

<details><summary>a09-history-stats (26)</summary>

- **pure-logic** (3): format_utils, history_export, personalize_coordinator
- **integration** (4): database, history_calendar, personalize_foundation_history, recording_retention
- **qt-widget** (19): export_dialog_base, history_calendar_dialog, history_calendar_wiring, history_entry_dialog, history_export_dialog, history_scalability, history_sidebar_delete, history_sidebar_show_in_folder, mcp_desktop_changes, personalize_fix2_g3_playback_fetch, personalize_fix2_g3_stats_chart, personalize_fix2_g3_stats_dialog, personalize_fix3_h3_audio_fetch, personalize_fix_f1_tray_copy_original, personalize_s3_command_delivery, personalize_s9_history_edit, personalize_s9_history_ui, personalize_s9_stats_dialog, stats_features

</details>

<details><summary>a10-settings-services (16)</summary>

- **pure-logic** (8): automation_fix_pipeline_backup, credentials, meeting_speech_follows_dictation, personalize_fix2_g1_held_scan_code, personalize_fix3_h3_enable_disable_hold, personalize_fix_f1_hotkey_repeat, personalize_foundation_backup, setting_defaults
- **integration** (1): backup
- **qt-widget** (6): backup_manager, backup_startup, code_change_restart, settings_general_layout, settings_retention_commit, settings_unified
- **dropped-engine** (1): settings

</details>

<details><summary>a11-settings-dialog-ui (6)</summary>

- **qt-widget** (6): api_keys_settings_page, meeting_settings_dialog, personalize_fix2_g2_mic_focus, personalize_foundation_ui_settings, settings_backup, settings_binder

</details>

<details><summary>a12-text-ai-personalize-services (34)</summary>

- **pure-logic** (11): personalize_fix2_g4_fold_speed, personalize_foundation_pipeline, personalize_foundation_settings, personalize_s1_styles, personalize_s2_benchmark, personalize_s2_prompts, personalize_s4_learning, personalize_s5_snippets, thinking_levels, typesafe_meeting_runtime, typesafe_service
- **integration** (9): http_tls, meeting_direct_deadline, network_reuse, personalize_fix_f2_command_refusals, personalize_s5_pipeline, text_generation, text_judge, text_llm, transcript_cleanup
- **qt-widget** (10): cleanup_agent_offer, cleanup_profiles, dictionary_cleanup_revert, personalize_fix2_g2_dictionary_focus, personalize_fix_f2_scratchpad_transform, personalize_fix_f4_unsaved_drafts, personalize_foundation_runtime, personalize_s3_command_runtime, personalize_s3_text_rewrite, typesafe_settings_messaging
- **platform** (1): personalize_s8_scratchpad
- **dropped-engine** (3): local_text_servers, openai_lazy_import, speech_endpoints

</details>

<details><summary>a13-personalize-ui (18)</summary>

- **qt-widget** (15): cleanup_rule_dialog, luna_cleanup_picker, personalize_fix2_g2_enter_key, personalize_fix3_h1_basic_tone_copy, personalize_fix3_h2_dictionary_honesty, personalize_fix_f4_styles_gate, personalize_s1_settings_page, personalize_s2_settings, personalize_s3_commands_page, personalize_s4_settings_page, personalize_s5_settings_page, personalize_s7_settings, rule_composer, speech_server_controls, text_endpoint_dialog
- **platform** (2): personalize_fix_f4_command_field, personalize_fix_f4_frozen_modules
- **dropped-engine** (1): speech_backend_picker

</details>

<details><summary>a14-models-components-services (33)</summary>

- **pure-logic** (6): build_component, component_catalog, component_runtime, gpu_info, model_catalog, whisper_av_compat
- **integration** (5): component_download, components, hf_access, openai_backend, openai_retirement
- **qt-widget** (1): setting_key_references
- **platform** (2): linux_gpu_component, native_libraries
- **dropped-engine** (19): api_upload_warmup, engine_warmup, isolated_cancellation, local_asr, local_asr_languages, local_asr_package_isolation, local_backend_fallback, macos_runtimes, model_download_safety, parakeet_mlx, parakeet_mlx_hardware, parakeet_unified, personalize_s4_openai, personalize_s4_recognition, qwen_batch, qwen_batch_worker, qwen_gpu_support, qwen_silence, use_gpu_offer

</details>

<details><summary>a15-models-downloads-ui (9)</summary>

- **qt-widget** (5): engine_status, hf_consent_dialog, personalize_s8_voice_languages, remote_dependencies, remote_link_ui
- **dropped-engine** (4): custom_whisper, remote_runtime, settings_downloads, settings_models

</details>

<details><summary>a16-updater-setup-packaging (19)</summary>

- **pure-logic** (1): app_update_rate_limit
- **integration** (7): app_update, app_update_apply, app_update_download_fallback, app_update_release_track, code_identity, frozen_updater, update_startup_gate
- **qt-widget** (6): app_update_dialog, app_update_handoff, guided_setup, multiprocessing_startup, update_resilience, updater_helper
- **platform** (5): app_update_macos, linux_deps, linux_installer, macos_installer, windows_installer

</details>

<details><summary>a17-remote-engine-services (25)</summary>

- **pure-logic** (4): lan_address, personalize_fix_f5_sync_edit_race, personalize_s4_remote, remote_token_scope
- **integration** (11): backup_record_sync, client_history, host_backup_recovery, meeting_web_server_address, personalize_s9_remote_records, remote_mcp_control, remote_mcp_link, remote_records, remote_registry, remote_tailscale_cli, segment_cursor_sources
- **qt-widget** (10): host_mode, personalize_fix_f1_paste_original_host, personalize_s9_retitle_sync, remote_engine_push, remote_keychain_recovery, remote_model_management, remote_pairing_ux, remote_records_ui, remote_rename, settings_polish

</details>

<details><summary>a19-actions-automation-services (83)</summary>

- **pure-logic** (27): automation_agents_handoff, automation_api_meeting_events, automation_fix_actions_agents, automation_fix_channel_gap, automation_fix_channel_link, automation_fix_channel_origin, automation_fix_hooks_filter_surrogate, automation_fix_hooks_webhook_secret, automation_fix_mcp_events_rollback, automation_fix_pipeline_shaping, automation_fix_routing_failure, automation_fix_routing_gate, automation_fix_routing_status, automation_fix_ui_hooks_saved, automation_foundation_models, automation_foundation_signing, automation_hooks_pipeline, automation_r2_caret_text, automation_r2_program_words_pipeline, automation_r3_busy_and_state, automation_r3_cancel_window, automation_r3_lease_routing, automation_r3_program_answers, automation_r4_cancel_uploads, automation_routing_decide, automation_routing_gate, automation_routing_status
- **integration** (39): automation_agents_ask, automation_agents_launch, automation_api_routes, automation_api_sse, automation_channel_protocol, automation_fix_actions_launch, automation_fix_actions_programs, automation_fix_api_agent_prompts, automation_fix_api_remote_filters, automation_fix_api_sse_order, automation_fix_api_unicode, automation_fix_events_gap_reason, automation_fix_hooks_filter_cancel, automation_fix_hooks_webhook_lanes, automation_fix_mcp_agent_prompts, automation_fix_mcp_events_gone, automation_fix_mcp_events_registry, automation_fix_mcp_events_remote, automation_fix_mcp_since, automation_fix_mcp_unicode, automation_fix_pipeline_control, automation_fix_pipeline_events, automation_fix_routing_timeout, automation_foundation_events, automation_foundation_pipeline, automation_hooks_filters, automation_hooks_programs, automation_hooks_webhooks, automation_integration_fixes, automation_mcp_events, automation_mcp_tools, automation_r2_program_words, automation_r2c_launch, automation_r2c_programs, automation_r3_gap_per_type, automation_r3c_programs, automation_r4_cancel_outcome, automation_r4_cancel_runtime, automation_r4b_launch
- **qt-widget** (16): automation_actions_pipeline, automation_actions_relay, automation_actions_runtime, automation_api_app_control, automation_api_end_to_end, automation_channel_e2e, automation_fix2_cancel, automation_fix2_pipeline, automation_fix_actions_runtime, automation_fix_pipeline_lifecycle, automation_fix_ui_hooks_answers, automation_fix_ui_hooks_results, automation_fix_ui_hooks_tab_order, automation_r3_cancel_finished, automation_ui_api_page, automation_ui_hooks_page
- **platform** (1): automation_r3c_launch

</details>

<details><summary>a20-agent-api-mcp-services (7)</summary>

- **integration** (5): agent_mcp, agent_process, automation_fix_api_not_found, automation_fix_mcp_events_logging, installed_agents
- **qt-widget** (2): agent_api, personalize_s2_mcp

</details>

<details><summary>a21-actions-automation-ui (7)</summary>

- **qt-widget** (7): agent_picker, automation_fix_ui_actions_page, automation_ui_actions_copy, automation_ui_actions_page, mcp_host_tabs, mcp_settings_page, walkthrough_write_up_notice

</details>

<details><summary>a22-walkthrough-services-snap (21)</summary>

- **pure-logic** (2): walkthrough_sections, walkthrough_storage
- **qt-widget** (16): snap_annotations, snap_click_screens, snap_geometry, snap_live, snap_overlay, walkthrough_buttons, walkthrough_document, walkthrough_document_threads, walkthrough_library, walkthrough_paste, walkthrough_runtime, walkthrough_session, walkthrough_storyboard, walkthrough_styles, walkthrough_word, walkthrough_write_up
- **platform** (3): snap_macos_capture, snap_macos_focus, snap_session

</details>

<details><summary>a23-walkthrough-ui (5)</summary>

- **qt-widget** (5): drag_to_move, past_walkthroughs, walkthrough_copy_tips, walkthrough_library_ui, walkthrough_panel

</details>

<details><summary>a24-meeting-engine (16)</summary>

- **pure-logic** (5): meeting_clock, meeting_export, meeting_export_bulk, meeting_polish_batches, meeting_time_utils
- **integration** (11): meeting_custom_report, meeting_dev_fixture, meeting_export_features, meeting_finalization, meeting_product_eval, meeting_scheduler, meeting_spool, meeting_title_consistency, meeting_web_auth, meeting_web_history, meeting_ws

</details>

<details><summary>a25-meeting-capture-asr-diarize (18)</summary>

- **pure-logic** (7): meeting_capture_health, meeting_data_lifecycle, meeting_diarize, meeting_hallucination, meeting_sd_stream, meeting_soundcard_stream, personalize_s7_audio_devices
- **integration** (6): meeting_capture_devices, meeting_cloud_speakers, meeting_failure_logging, meeting_recovery, meeting_voice_preview, remote_host_admission
- **platform** (2): meeting_linux_capture, meeting_macos_capture
- **dropped-engine** (3): meeting_asr, meeting_refinalize, meeting_remote

</details>

<details><summary>a26-meeting-agent (14)</summary>

- **pure-logic** (2): meeting_agent_factory, meeting_repair
- **integration** (11): agent_cleanup, installed_agent_core, installed_agent_drivers, meeting_agent_rejection_repair, meeting_corrections, meeting_live_agent_workflow, meeting_polish_agent, meeting_sidecar, meeting_spotlight_context, opencode_sidecar, scheduler_semantic_topic_shift
- **dropped-engine** (1): benchmark_contracts

</details>

<details><summary>a28-meeting-state-persist-export (12)</summary>

- **pure-logic** (4): meeting_content, meeting_intent, meeting_state_copy_on_write, personalize_s4_dictionary
- **integration** (8): meeting_delivery_cursor, meeting_insight_review, meeting_notes_agent, meeting_repository, meeting_revise_segment_text, meeting_scoped_corrections, meeting_state, meeting_stored

</details>

<details><summary>a29-meeting-intelligence (7)</summary>

- **pure-logic** (1): meeting_takeaways
- **integration** (6): live_signals_lifecycle, meeting_context_folder, meeting_fast_features, meeting_recall, meeting_reinsight, voice_commands

</details>

<details><summary>a30-meeting-web-server-and-dashboard-contract (4)</summary>

- **integration** (4): mcp_controls, meeting_note_requests, meeting_web_server_logging, meeting_web_server_stop

</details>

<details><summary>a31-meeting-qt-ui (13)</summary>

- **qt-widget** (12): ai_insights_agent_copy, meeting_audio_consent_dialog, meeting_delete_dialog, meeting_export_dialog, meeting_intro_dialog, meeting_mode_tab, meeting_setup, meeting_unsupported_platform, past_meetings_delete, past_meetings_panel, startup_visibility, typed_settings
- **platform** (1): meeting_linux_audio_dialog

</details>

<details><summary>unmapped:benchmarks (3)</summary>

- **pure-logic** (2): live_preview_benchmark, meeting_benchmark
- **integration** (1): audio_dsp_benchmark

</details>

<details><summary>unmapped:none (2)</summary>

- **pure-logic** (1): webui_accessibility
- **qt-widget** (1): qt_test_isolation

</details>

<details><summary>unmapped:scripts (3)</summary>

- **pure-logic** (1): release_qualification
- **integration** (1): testclient_compat
- **platform** (1): windows_signing

</details>

## 3. Data isolation

OpenWhisper resolves every writable path through `config.data_root()`:

1. `OPENWHISPER_DATA_DIR`, when set, wins.
2. Otherwise a source run uses the working directory or the checkout when one
   already has `openwhisper_settings.json`, or the checkout when it is a git
   checkout. **So a plain `import config` from `D:\coding\whisper_local` points
   at the owner's live settings, database and recordings in the checkout.**
3. Frozen builds use `%LOCALAPPDATA%\OpenWhisper`. `components_root()` is
   always `%LOCALAPPDATA%\OpenWhisper\components`.

`AppConfig` resolves `SETTINGS_FILE`, `DATABASE_FILE`, `LOG_FILE`,
`RECORDINGS_FOLDER` ... once, at import, so the variable has to be set before
`config` is first imported.

What `tests/conftest.py` does:

- `pytest_configure` (trylast, before collection imports anything) sets
  `OPENWHISPER_DATA_DIR` to a pytest temp folder and removes proxy variables.
- `_session_data_root` asserts `config.SETTINGS_FILE` is under that root
  ("config was imported before pytest_configure chose the test data root").
- `_isolated_data_root` (autouse) moves the root to each test's `tmp_path`,
  re-points every `config` path, `credentials.env_file_path` (the checkout's
  own `.env` is the lookup fallback and holds real keys) and the settings
  manager, resets the `db` and `history_manager` singletons, and closes SQLite
  afterwards.
- `_isolated_credential_store` (autouse): in-memory credential store instead
  of Windows Credential Manager / Keychain.
- `_null_focus_capture`: no foreground-window or caret-text reads.
- `_no_real_network_discovery`: discovery on port 0, no LAN broadcast, no
  PowerShell network-profile reads.
- `_no_real_local_text_servers`: Ollama/LM Studio/vLLM probes get a refusing
  `httpx.MockTransport`.
- `_no_installed_agent_probes` (Qt tests): no running `claude`/`codex`/`opencode`.
- `_before_openai_transcription_shutdown`: pins the OpenAI retirement clock to
  2026-09-26.

Two gaps the suite does not close and the harness has to: Python writes
`__pycache__` next to the sources it imports (conftest even forces
`sys.dont_write_bytecode = False`), and pytest writes `.pytest_cache` in the
rootdir. Both would land in the checkout.

`ow_isolation.py` repeats the conftest mechanism for scripts that import
OpenWhisper outside pytest: sets `OPENWHISPER_DATA_DIR` to
`out\parity-scratch\ow-data` and `LOCALAPPDATA`/`APPDATA`/`XDG_DATA_HOME` to
`out\parity-scratch\ow-appdata` before anything imports `config` (and refuses
to run if `config` is already imported), sets `HF_HUB_OFFLINE=1`, drops
proxies, disables bytecode, then after import asserts that `data_root()`,
`SETTINGS_FILE`, `DATABASE_FILE` and `components_root()` are inside the
scratch folder and installs the in-memory credential store, a scratch
`env_file_path` and the null focus service. Scripts run with `-I -B`.

For pytest runs: `PYTHONPYCACHEPREFIX` moves bytecode into scratch,
`-p no:cacheprovider` stops `.pytest_cache`, `--basetemp` keeps temp folders in
scratch, and conftest does the rest.

**Checked.** After each run, every file and folder under
`D:\coding\whisper_local` (venvs included) and under
`%LOCALAPPDATA%\OpenWhisper` was listed by write and creation time. None of
the harness runs wrote there. The only recent changes in either place
(`openwhisper.db-wal/-shm` in the checkout at 01:21, a recording and settings
in `%LOCALAPPDATA%\OpenWhisper` at 01:20 and 02:03) predate the first harness
run (after 02:27) and come from the owner's own use of OpenWhisper; their
contents were not read. `ow-data` and `ow-appdata` stayed empty: nothing in the
modules exercised writes data at import.

What was **not** run: the full suite, any test that starts the app, records
audio, or opens the real database. Running other test files through the same
command is isolated by conftest for data, but a test file that spawns
`fake_agent_cli.py` or a remote host binds loopback ports and starts child
processes; that is expected and still inside scratch, but was not exercised.

## 4. Harness (a): golden vectors

**Design.** A Python script imports OpenWhisper from the checkout path under
`ow_isolation`, calls pure functions on a list of inputs and writes one JSON
file per module:

```json
{"module": "services.format_utils", "source_commit": "768b559e...",
 "pins": {"python": "3.12.10", "platform": "win32", "timezone_note": "..."},
 "functions": {"format_audio_duration": [
   {"args": [62.5], "result": "1m 2s"},
   {"args": ["x"], "result": "0.0s"}]}}
```

Raised exceptions are recorded as `{"args": [...], "raises": "ValueError"}`.
Floats are written with `repr()` (exact round trip); NaN/inf and non-JSON
values are tagged (`{"$float": "nan"}`, `{"$repr": "..."}`).

Two sources of inputs:

1. **Curated** (`golden_vectors.py`, the `CASES` table): ordinary values plus
   the edges where a literal C++ translation goes wrong.
2. **Traced** (`trace_plugin.py`): runs OpenWhisper's own test files with a
   pytest plugin that wraps the chosen functions in `pytest_sessionstart`
   (after conftest has moved the data root, before collection) and records
   every distinct call. This harvests exactly the inputs the Python tests
   already consider important. It misses calls through references bound
   before session start (decorators, default arguments).

The C++ side reads the same files. The Qt-free core uses nlohmann-json (add
`nlohmann-json` to `vcpkg.json`) in Catch2 tests, one `TEST_CASE` per
function with a `DYNAMIC_SECTION` per case; the proof below used Qt's
`QJsonDocument` only because Qt was already installed.

**Commands that worked** (PowerShell, cwd `C:\coding\OpenWhisper_CPP\out\parity-scratch`):

```powershell
# curated vectors -> golden\*.json
D:\coding\whisper_local\.venv\Scripts\python.exe -I -B golden_vectors.py D:\coding\whisper_local C:\coding\OpenWhisper_CPP\out\parity-scratch

# traced vectors from OpenWhisper's own tests -> golden\traced.json
$sc = 'C:\coding\OpenWhisper_CPP\out\parity-scratch'
$env:PYTHONPATH = $sc; $env:PYTHONPYCACHEPREFIX = "$sc\pycache"
$env:LOCALAPPDATA = "$sc\ow-appdata"; $env:APPDATA = "$sc\ow-appdata"
$env:QT_QPA_PLATFORM = 'offscreen'; $env:HF_HUB_OFFLINE = '1'
D:\coding\whisper_local\.venv\Scripts\python.exe -m pytest -q -p trace_plugin -p no:cacheprovider `
  --basetemp "$sc\pytest-tmp" `
  --trace-golden "services.format_utils:format_audio_duration,format_timestamp" `
  --trace-golden "ui_qt.utils.fuzzy_match:fuzzy_score,fuzzy_match" `
  --trace-out "$sc\golden\traced.json" `
  D:\coding\whisper_local\tests\test_format_utils.py D:\coding\whisper_local\tests\test_fuzzy_match.py

# C++ replay
. C:\coding\OpenWhisper_CPP\scripts\vsenv.ps1
$q = 'D:\Qt\6.10.3\msvc2022_64'; cd replay
cl /nologo /std:c++20 /EHsc /O2 /MD /Zc:__cplusplus /permissive- /utf-8 /I "$q\include" /I "$q\include\QtCore" replay_format_utils.cpp /link "$q\lib\Qt6Core.lib"
$env:PATH = "$q\bin;$env:PATH"; .\replay_format_utils.exe ..\golden\services.format_utils.json
```

**What they produced.**

- `golden/services.format_utils.json` (67 cases over 6 functions),
  `ui_qt.utils.loudness.json` (11), `services.titles.json` (16),
  `ui_qt.utils.fuzzy_match.json` (19).
- `golden/traced.json`: 72 distinct calls (5 + 5 from `test_format_utils`,
  33 + 29 from `test_fuzzy_match`); the 16 tests passed with the plugin loaded.
- The replay ran two C++ ports of `format_audio_duration` against the 20
  vectors:

  ```
    duration (std::lround)     [62.5]       expected 1m 2s      got 1m 3s
  duration (std::lround)     20 cases, 1 mismatches
  duration (half to even)    20 cases, 0 mismatches
  format_size_bytes          13 cases, 0 mismatches
  ```

  Python's `round()` rounds half to even; `std::lround` rounds half away from
  zero. That is exactly the kind of divergence the vectors exist to catch.

**Python behaviors the vectors must pin** (found while choosing inputs):

- `round()` is half-to-even (`62.5 -> 62`, `63.5 -> 64`); f-string `:.0f`
  and `:.1f` round the exact binary value (`format_size_bytes(2500) == "2 KB"`,
  `1500 -> "2 KB"`). Use `std::nearbyint` under `FE_TONEAREST` and
  `std::format`, never `std::round`/`lround`.
- `len()` counts code points, not UTF-8 bytes (`title_error("é" * 200)` is
  valid). Count with a UTF-8 decoder, or keep such strings in `QString` and
  count `QStringView` code points, not UTF-16 units either.
- `str.casefold()` folds `ß` to `ss` (`fuzzy_score("straße", "STRASSE") == 10000`),
  and `re`'s `\w` is Unicode. `QString::toCaseFolded()` does not expand `ß`;
  a C++ port needs full case folding (ICU `u_strFoldCase`, or a table).
- `datetime.astimezone()` uses the machine's zone. Only naive timestamps are in
  the vectors; aware ones need a pinned zone on both sides (on Windows the
  `TZ` variable does not affect Python), so pass the zone in explicitly in the
  C++ API and test with it.
- `float("12.5")` accepts strings: several formatters take loosely typed input
  from settings and JSON. The vectors keep those cases.

## 5. Harness (b): reference screenshots

**Design.** `render_reference.py` constructs OpenWhisper widgets or dialogs
in a process configured the way `ui_qt/app.py` `QtApplication` configures the
app, grabs them, and writes per render:

- `screens/<widget>-<theme>-<style>-dpr<n>-s<scale>.png`, the reference image;
- the same name `.json`: every pin (Qt version, platform, base style, theme,
  font scale, app font, resolved family, font dir, stylesheet SHA-1, pixel
  SHA-1) and a **widget-tree dump**: for each widget its Python class, first
  Qt base class, `objectName`, dynamic properties (`tone=neutral`), geometry,
  visibility, focus, text, checked/default state, alignment, word wrap,
  widget-local stylesheet and font;
- `screens/app-<theme>-s<scale>.qss`: the application stylesheet after token
  and font-scale resolution (142,683 bytes for dark at 100%);
- `screens/palette-<theme>.json`: the active `QPalette` the theme manager set.

Steps inside, in order: isolate data (section 3); set
`QT_QPA_PLATFORM=offscreen`, `QT_QPA_FONTDIR=%WINDIR%\Fonts`,
`QT_ENABLE_HIGHDPI_SCALING=1`, `QT_SCALE_FACTOR=<dpr>`, `QT_FONT_DPI=96`,
clear `QT_SCREEN_SCALE_FACTORS` and `QT_STYLE_OVERRIDE`; create
`QApplication`; `app.setStyle(<style>)`; `ThemeManager(<theme>)`, install
`WidgetStyleFilter`, `apply_ui_theme`, `apply_ui_font_scale(<percent>)`; build
the widget; `setFixedSize`; `show()`; one `processEvents()`; `grab()`.

`replay/render_tree.cpp` is the C++ half for checking rendering alone: it
reads the tree dump, rebuilds the same widgets as plain Qt classes at the
dumped geometry with the same names, properties, texts, focus and fonts,
applies the dumped stylesheet and palette, grabs offscreen and diffs against
the reference (count of differing pixels, max channel delta, a magenta diff
image). A real port is then checked in two steps: its own widget tree against
the dumped tree (layout: geometry, texts, properties, without pixels), then
its own grab against the reference PNG (rendering).

**Commands that worked** (cwd `out\parity-scratch`):

```powershell
$py = 'D:\coding\whisper_local\.venv\Scripts\python.exe'; $ow = 'D:\coding\whisper_local'; $sc = 'C:\coding\OpenWhisper_CPP\out\parity-scratch'
& $py -I -B render_reference.py $ow $sc                       # dark, fusion, dpr 1, 100 %
& $py -I -B render_reference.py $ow $sc --theme light
& $py -I -B render_reference.py $ow $sc --dpr 2
& $py -I -B render_reference.py $ow $sc --style windows11

. C:\coding\OpenWhisper_CPP\scripts\vsenv.ps1
$q = 'D:\Qt\6.10.3\msvc2022_64'; cd replay
cl /nologo /std:c++20 /EHsc /O2 /MD /Zc:__cplusplus /permissive- /utf-8 /I "$q\include" /I "$q\include\QtCore" /I "$q\include\QtGui" /I "$q\include\QtWidgets" render_tree.cpp /link "$q\lib\Qt6Core.lib" "$q\lib\Qt6Gui.lib" "$q\lib\Qt6Widgets.lib"
$env:PATH = "$q\bin;$env:PATH"; $env:QT_PLUGIN_PATH = "$q\plugins"
$env:QT_QPA_PLATFORM = 'offscreen'; $env:QT_QPA_FONTDIR = "$env:WINDIR\Fonts"
$env:QT_SCALE_FACTOR = '1'; $env:QT_FONT_DPI = '96'; $env:QT_ENABLE_HIGHDPI_SCALING = '1'
$s = '..\screens'; $n = 'meeting_delete_dialog'
.\render_tree.exe "$s\$n-dark-fusion-dpr1-s100.json" "$s\app-dark-s100.qss" "$s\palette-dark.json" "$s\$n-dark-fusion-dpr1-s100.png" "cpp-$n-dark.png"
```

**What they produced.** Two widgets: `EmptyState` (Settings' empty list box,
with an "Add snippet" button) at 420x150, and `MeetingDeleteDialog`
(`has_audio=True`) at 420x230, in dark and light, at DPR 1, 1.25 and 2, font
scale 100 and 125, and with Fusion, windows11 and windowsvista base styles.
The PNGs show the themed widgets as the app draws them (dark `#1c1c1e`
background, red Delete, blue-ringed default Cancel).

C++ (Qt 6.10.3, MSVC) rebuilding the dumped trees against the Python (PyQt6
6.11.0) references:

| Comparison | Result |
|---|---|
| dialog, dark, DPR 1 (focus pinned) | 0 of 96,600 pixels differ |
| empty state, dark, DPR 1 | 0 of 63,000 |
| dialog and empty state, light | 0 differ |
| dialog, dark, DPR 2 (`QT_SCALE_FACTOR=2` both sides) | 0 of 386,400 |
| C++ base style windows11 vs Python Fusion | 0 differ |
| first C++ attempt, focus not pinned | 321 differ (focus ring on the first checkbox, Cancel not ringed) |
| C++ `QT_FONT_DPI=120` | size differs: 525x288 vs 420x230 |
| C++ DPR 1 vs DPR 2 reference | size differs: 420x230 vs 840x460 |
| C++ without `QT_QPA_FONTDIR` | 6,905 differ, max delta 217 (no fonts found: boxes for text) |

Python against itself: two separate runs of each render give identical pixel
hashes, so the reference is deterministic.

### What must be pinned for two renders to match

| Pin | Why, and what was observed |
|---|---|
| Platform plugin `offscreen` on both sides | It rasterizes text with FreeType from `QT_QPA_FONTDIR`, not DirectWrite, so it is deterministic and the same in PyQt6 and C++. On-screen renders on Windows use DirectWrite and will not match an offscreen reference. |
| `QT_QPA_FONTDIR` | Offscreen does not find Windows fonts by itself; without it text renders as boxes (6,905 pixels off). Same folder, same font files, on both sides. |
| Fonts present | The app font is `Segoe UI` (`Noto Sans` under Omarchy), set by `apply_ui_font_scale` to 14 px at 100 %. Segoe UI is not redistributable: references made on Windows can only be compared on Windows. A Linux CI would need a pinned free font on both sides, which no longer shows the real look. |
| Device pixel ratio | `QT_SCALE_FACTOR` (and `QT_ENABLE_HIGHDPI_SCALING=1`, no `QT_SCREEN_SCALE_FACTORS`) on both sides; DPR 2 doubles the image. |
| Logical DPI | `QT_FONT_DPI=96`. On offscreen it scales the whole render (120 gave 1.25x). |
| Theme and stylesheet | Apply the dumped, resolved `.qss` verbatim in the C++ test, or check that the port's own resolver produces the same text (compare SHA-1s: the sidecar has `stylesheet_sha1`). Theme `dark`/`light` and font scale percent are inputs to that text. |
| Palette | `ThemeManager.apply_platform_palette` sets a `QPalette`. It made no difference to these two QSS-styled widgets, but unstyled parts (scroll areas, item views, tooltips) read it. Set it from the dump. |
| Base style | The offscreen default is Fusion; the app on Windows runs windows11 (wrapped by `SnappyTooltipStyle`). With `theme.qss` applied these widgets render identically under Fusion, windows11 and windowsvista, but controls the QSS does not fully style can differ: pin the same base style name both sides. Set it after `QApplication` exists: setting windows11 before construction crashes the offscreen process (access violation, Qt 6.11.0). |
| Focus and default button | Pin which widget has focus (`hasFocus` in the dump) and the default button; otherwise the first focusable widget takes focus on show (321 pixels differed). Hover/pressed states likewise: never move the mouse. |
| Size | Fixed logical size for the grab. A port's natural size is checked separately against the tree dump's geometry. |
| Animations and timers | No event loop after one `processEvents()`, so `QPropertyAnimation`/`QVariantAnimation` (collapse animations, agent picker hover/lift, action field arrival) stay at their start value and no `QTimer` fires (`SegmentedBar` reflow timer). OpenWhisper has no global "reduce motion" switch; for widgets whose first frame depends on an animation, drive the animation to its end value explicitly on both sides. |
| Time and randomness | Widgets that show dates (history, meetings, stats, backups) need a pinned clock and time zone, injected on both sides (OpenWhisper's tests patch module-level `_today`/`now` functions). Particle and overlay looks (`particle_style.py`) use `animation_time` and `random`: render at a fixed time with a fixed seed. |
| Qt version | References are PyQt6 6.11.0; OpenWhisper_CPP builds against Qt 6.10.3. These renders matched exactly across the two versions, but stylesheet and text rendering do change between Qt minors. Keep both on one version (OpenWhisper_CPP moving to 6.11.x is the cheap direction), and record the version in every sidecar (done). |
| Image format | Compare after converting both to ARGB32; compare at the device pixel size. |
| Class names | `theme.qss` selects only on Qt class names, `#objectName` and `[property=value]` (no selector names an OpenWhisper class), so C++ widgets can live in namespace `fv` as long as they derive from the same Qt class and set the same object names and dynamic properties (`tone`, `buttonId`, ...). Widget-local stylesheets need the same check before relying on it. |

## 6. Coverage strength by area

Test functions per 1,000 source lines, from `coverage_table.py`. "Primary" is
the file's own area; "also touched by" counts files that reach the area
through secondary imports.

| Area | Src lines | Test files (primary) | Also touched by | Test functions | Tests per 1k src lines | Pure-logic files | Pure-logic tests |
|---|---:|---:|---:|---:|---:|---:|---:|
| a01-app-shell | 5896 | 18 | 249 | 145 | 25 | 5 | 28 |
| a02-ui-foundation | 13058 | 26 | 74 | 197 | 15 | 0 | 0 |
| a03-ui-controller | 4448 | 3 | 52 | 159 | 36 | 0 | 0 |
| a04-runtime-services | 9596 | 17 | 42 | 414 | 43 | 3 | 16 |
| a05-recording-audio | 6078 | 22 | 71 | 245 | 40 | 9 | 87 |
| a06-hotkeys-paste-focus | 7782 | 40 | 39 | 431 | 55 | 8 | 53 |
| a07-overlays | 4370 | 9 | 7 | 130 | 30 | 0 | 0 |
| a08-record-upload-tabs | 4941 | 12 | 15 | 234 | 47 | 1 | 14 |
| a09-history-stats | 10374 | 26 | 49 | 248 | 24 | 3 | 17 |
| a10-settings-services | 5137 | 16 | 245 | 251 | 49 | 8 | 72 |
| a11-settings-dialog-ui | 8178 | 6 | 61 | 66 | 8 | 0 | 0 |
| a12-text-ai-personalize-services | 6678 | 34 | 56 | 602 | 90 | 11 | 181 |
| a13-personalize-ui | 6245 | 18 | 6 | 201 | 32 | 0 | 0 |
| a14-models-components-services | 13430 | 33 | 81 | 482 | 36 | 6 | 42 |
| a15-models-downloads-ui | 7745 | 9 | 22 | 283 | 37 | 0 | 0 |
| a16-updater-setup-packaging | 8688 | 19 | 4 | 360 | 41 | 1 | 12 |
| a17-remote-engine-services | 9628 | 25 | 25 | 285 | 30 | 4 | 30 |
| a18-remote-engine-ui | 3872 | 0 | 7 | 0 | 0 | 0 | 0 |
| a19-actions-automation-services | 9376 | 83 | 11 | 858 | 92 | 27 | 206 |
| a20-agent-api-mcp-services | 5706 | 7 | 48 | 70 | 12 | 0 | 0 |
| a21-actions-automation-ui | 8337 | 7 | 7 | 132 | 16 | 0 | 0 |
| a22-walkthrough-services-snap | 8219 | 21 | 6 | 480 | 58 | 2 | 22 |
| a23-walkthrough-ui | 5880 | 5 | 4 | 77 | 13 | 0 | 0 |
| a24-meeting-engine | 6540 | 16 | 67 | 260 | 40 | 5 | 32 |
| a25-meeting-capture-asr-diarize | 8932 | 18 | 10 | 304 | 34 | 7 | 79 |
| a26-meeting-agent | 8058 | 14 | 13 | 227 | 28 | 2 | 35 |
| a27-meeting-sidecars-ts | 7218 | 0 | 0 | 0 | 0 | 0 | 0 |
| a28-meeting-state-persist-export | 6761 | 12 | 39 | 234 | 35 | 4 | 59 |
| a29-meeting-intelligence | 3975 | 7 | 7 | 110 | 28 | 1 | 4 |
| a30-meeting-web-server-and-dashboard-contract | 5223 | 4 | 11 | 19 | 4 | 0 | 0 |
| a31-meeting-qt-ui | 6535 | 13 | 18 | 194 | 30 | 0 | 0 |

**Strong pure-logic coverage** (best golden-vector sources):

- **a19 actions/automation**: 27 pure files, 206 tests: routing decisions
  (`routing_decide`, `routing_gate`, `routing_status`), event shaping and gap
  handling, filter surrogates, webhook signing and secrets, program words.
- **a12 text/personalize services**: 11 pure files, 181 tests: style and
  cleanup prompts, snippet expansion, learning, thinking levels, typesafe
  payloads, transforms.
- **a05 recording/audio**: 9 pure files, 87 tests: `audio_processor`,
  `chunking`, `resample`, `wav_metadata`, language and stats folding,
  incremental dictation windows. Numeric, so vectors need a tolerance policy.
- **a25 capture/diarize**: 7 pure files, 79 tests: hallucination filter,
  diarization clustering/assignment, capture health, stream framing.
- **a10 settings**: 8 pure files, 72 tests: defaults, credentials, backup
  manifest, hotkey normalization. `services/setting_types.py` and the defaults
  table are ideal to dump wholesale (every key, type, default, migration).
- **a28 meeting state**: 4 pure files, 59 tests: content model, copy-on-write
  state, intents, dictionary.
- **a06 hotkeys/focus**: 8 pure files, 53 tests: focus catalog, context text,
  paste target, terminal detection.
- Pure modules living under `ui_qt` that count as qt-widget above but are
  golden-vector material: `fuzzy_match`, `overlay_position`,
  `markdown_render`, `list_reconcile`, `loudness`, `palette` token resolution,
  `font_scale.scale_qss_fonts`, `snap/geometry`, walkthrough `document`/`sections`.

**Thinly tested** (port these with extra care, and lean on screenshots and
manual comparison):

- **a18 remote engine UI** (3,872 lines): no test file of its own.
- **a11 settings dialog UI** (8,178 lines, 8 tests per 1k lines): the shell,
  binder, search and home pages; most Settings pages are tested from a13/a15/a17
  instead. The single biggest visual surface, so a screenshot set per page is
  the main guard.
- **a20 agent API/MCP** (12 per 1k), **a30 web server** (4 per 1k, the
  dashboard itself is tested in TS), **a23 walkthrough UI** (13), **a02 UI
  foundation** (15 per 1k for 13k lines of shared widgets; no pure tests), **a21
  actions UI** (16).
- **a27** TypeScript sidecars: no pytest coverage; they stay TypeScript
  processes or are dropped, so their own `npm test` remains the reference.
- Dropped-engine weight sits in **a14** (19 of 33 files) and **a15** (4 of 9):
  what is left for the Parakeet/NeMo path there is component catalog,
  component runtime, downloads, GPU info and model catalog.

## 7. Recommended test strategy for OpenWhisper_CPP

1. **Catch2 for the Qt-free core** (`src/engine` and any future
   `src/core`-style libraries). Port pure-logic and integration tests as direct
   C++ tests, keeping OpenWhisper's test names in the Catch2 test names
   (`"[format_utils] format_audio_duration rounds half to even"`) and a
   `// from tests/test_format_utils.py::test_...` comment so coverage can be
   tracked file by file against `classification.json`. Reproduce the
   fakes as C++ seams: settings store, clock, HTTP transport, audio device,
   process launcher, credential store. Add `nlohmann-json` to `vcpkg.json` for
   vector replay.
2. **Golden vectors** for every pure module before its port starts:
   generate with `golden_vectors.py` (curated edges) plus `trace_plugin.py`
   over the module's own test files (real inputs), commit the JSON under
   `tests/golden/<module>.json` with the source commit inside, and replay with
   one generic Catch2 driver per function signature. Regenerate when the
   OpenWhisper reference commit moves; a diff in a vector file is a behavior
   change to review. For numeric DSP (resample, loudness, diarization), store
   arrays and compare with a stated tolerance (absolute 1e-6 for float32
   pipelines unless the test says otherwise).
3. **Qt Test for widgets** (`tests/app`, run through `ctest` with the
   offscreen plugin as today). Port the 210 qt-widget files' assertions
   (texts, enabled/visible state, signals, size and fit checks) directly; most
   do not depend on PyQt specifics. Keep the same object names and dynamic
   properties as OpenWhisper so tests and `theme.qss` both find widgets the same
   way. Put `theme.qss` and the token/scale resolver in the C++ app and
   golden-test the resolver against `app-<theme>-s<scale>.qss` dumps.
4. **Screenshot comparison** as the "looks the same" gate: for each dialog,
   page and overlay state, a reference PNG plus tree dump from
   `render_reference.py` (extend `build_widgets()` with a builder per widget
   and a fixed fake data set), stored under `tests/app/reference/`. The C++
   Qt Test builds its own widget with the same fake data, first compares its
   widget tree to the dump (geometry, texts, properties: readable failures),
   then compares the grab pixel-for-pixel, writing a diff image on failure.
   Run on Windows with the pins in section 5; allow a small per-test tolerance
   only where a known difference is accepted and documented.
5. **Platform tests**: Windows first (hotkeys, paste, focus, updater). macOS
   and Linux backends are mapped but only tested when ported.
6. **Dropped-engine tests** (37 files): not ported. Before deleting an idea,
   read the file for behavior that also applies to Parakeet (cancellation,
   warmup, language lists, fallback messages).
7. **Track parity** with a generated report: for each OpenWhisper test file,
   `ported` / `replaced by golden vectors` / `not applicable (dropped)` /
   `pending`, keyed by `classification.json`.
