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
| a19-actions-automation-services | 83 | 858 | 27 | 39 | 16 | 1 | 0 |
| a20-agent-api-mcp-services | 7 | 70 | 0 | 5 | 2 | 0 | 0 |
| a21-actions-automation-ui | 7 | 132 | 0 | 0 | 7 | 0 | 0 |
| a22-walkthrough-services-snap | 21 | 480 | 2 | 0 | 16 | 3 | 0 |
| a23-walkthrough-ui | 5 | 77 | 0 | 0 | 5 | 0 | 0 |
| a24-meeting-engine | 16 | 260 | 5 | 11 | 0 | 0 | 0 |
| a25-meeting-capture-asr-diarize | 18 | 304 | 7 | 6 | 0 | 2 | 3 |
| a26-meeting-agent | 14 | 227 | 2 | 11 | 0 | 0 | 1 |
| a28-meeting-state-persist-export | 12 | 234 | 4 | 8 | 0 | 0 | 0 |
| a29-meeting-intelligence | 7 | 110 | 1 | 6 | 0 | 0 | 0 |
| a30-meeting-web-server-and-dashboard-contract | 4 | 19 | 0 | 4 | 0 | 0 | 0 |
| a31-meeting-qt-ui | 13 | 194 | 0 | 0 | 12 | 1 | 0 |
| unmapped:benchmarks | 3 | 27 | 2 | 1 | 0 | 0 | 0 |
| unmapped:none | 2 | 4 | 1 | 0 | 1 | 0 | 0 |
| unmapped:scripts | 3 | 37 | 1 | 1 | 0 | 1 | 0 |
| **total** | 548 | 7766 | 111 | 154 | 210 | 36 | 37 |

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

