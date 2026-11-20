# Meeting engine: lifecycle, finalization, recovery, refinalize

Area `a24-meeting-engine`. Source root `D:\coding\whisper_local`. Files (6540 lines):
`meeting/__init__.py` (8), `meeting/clock.py` (101), `meeting/time_utils.py` (122),
`meeting/platform.py` (152), `meeting/interfaces.py` (347), `meeting/dev_fixture.py` (293),
`meeting/end_run.py` (438), `meeting/finalization.py` (383), `meeting/recovery.py` (744),
`meeting/refinalize.py` (1183), `meeting/engine.py` (2769).

No UI lives in this area (the package is deliberately Qt-free, `meeting/__init__.py:3`).
The look-related contract here is strings: finalization step names, details, headlines and
terminal messages that the desktop finalization card and the web dashboard render verbatim.

## What the user gets

- **One meeting at a time, end to end.** Start creates a meeting row, spool folder, host
  and guest links, a "Me" participant, starts the pausable meeting clock, a 10 s heartbeat,
  dual-channel capture (mic + system audio), the local web dashboard, then loads ASR,
  speaker separation and (when AI insights are on) the agent in the background so capture
  never waits on a model.
- **Pause / resume** freezes meeting time (timestamps exclude paused spans).
- **AI insights toggle mid-meeting** starts or tears down the agent; turning back on never
  re-sends transcript spoken while it was off.
- **Backup agent**: when the chosen agent fails (at start or after repeated checkpoint
  failures) insights move once to a user-chosen installed agent.
- **End** returns immediately; the dashboard flips to "ending", capture stops, ASR drains,
  the meeting is marked `ended` (or `needs_recovery`), listeners get `ended`, then an
  ordered, visible pipeline runs: Audio Re-transcription -> Speaker Identification ->
  Transcript Cleanup -> Summary & Action Items -> State Finalization. Each step shows a
  status, detail and a headline; the outcome card says what was found or which steps
  failed ("... failed. The recording and transcript were kept.").
- **Crash recovery**: on startup, meetings whose app session died (stale heartbeat or dead
  pid) are offered for Finalize or Discard. Finalize transcribes leftover audio chunks
  headless (including audio salvaged from orphaned WAVs and raw session PCM tails) and
  marks the meeting ended (or `failed` when capture dropped audio).
- **Retry / re-run** any post-meeting step later (desktop Past Meetings card, dashboard
  `reinsights` / `respeakers` routes, and the follow-up after recovery), one run per
  meeting at a time.
- **Demo meeting** (developer mode): a canned 25-turn, 3-speaker transcript with cards so
  End can be exercised without audio.

## Feature inventory

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A24-01 | Meeting start | Creates `meeting_sessions` row (status `active`, tokens `secrets.token_urlsafe(32)` x2, `app_pid`, `app_heartbeat_at`=started_at), spool dir `<spool_root or "meeting_audio">/<meeting_id[:12]>`, MeetingState with clamped intent (`MAX_INTENT_LEN`), finalization default (`pending` if cloud else `disabled` "AI insights are off for this meeting."), upserts participant "Me" (kind `me`). Returns `{meeting_id,url,host_url,guest_url}`. Raises "Meeting engine is already active or finished" on reuse. | engine.py:MeetingEngine.start (535-678) | port |
| A24-02 | Required vs optional startup | Dashboard server and >=1 capture source required; server failure raises "Meeting dashboard could not start: {exc}". ASR (local), diarizer, intelligence start on `meeting-processing-start` thread; diarizer on its own `meeting-diarizer-start` thread so a slow speaker-model download never delays ASR. Remote ASR route is started before capture (auth/validate). | engine.py:_start_processing, _processing_worker, _start_diarizer_background, _start_server | port (remote ASR: undecided backend) |
| A24-03 | Aborted start cleanup | Failed start tears everything down; if the meeting has no chunks and no segments the row and spool are deleted (no "Untitled · Ended" history row); otherwise status `failed`, finalization `unavailable` "Meeting failed to start before final insights could run." Inspection failure keeps the row. | engine.py:_abort_start, _aborted_meeting_has_artifacts, _discard_empty_meeting, _mark_aborted_failed | port |
| A24-04 | Pause / resume | Clock pause + state status `paused`; on persist failure the clock is rolled back and "Could not persist paused meeting state" / "Could not persist resumed meeting state" raised. No-ops once End claimed. | engine.py:pause, resume; clock.py | port |
| A24-05 | Meeting clock | Monotonic, pausable "meeting seconds"; timestamps taken while paused resolve to the pause instant; `resume_from_recovery(elapsed)` re-anchors; `paused_total_s`. | clock.py:MeetingClock | port |
| A24-06 | Heartbeat | Every 10 s `repository.heartbeat` (writes pid + UTC now); also retries fast-feature start and live-highlights status. Failures warn once, then debug. | engine.py:_heartbeat_loop, HEARTBEAT_INTERVAL_S | port |
| A24-07 | Status fan-out | `status` event + WS `{"type":"status", status, intelligence_online, diarization_available, diarization_reason, capture, finalization, live_highlights_status, speech}`; listener copy may carry `note`. | engine.py:_emit_status | port |
| A24-08 | Listener events | Kinds: `status`, `segments`, `server_started`, `error` (codes `processing_unavailable`, `asr_unavailable`, `asr_chunk_failed`, `end_failed`, `capture_incomplete` from capture), `ended`, `intelligence`, `asr_released`, `background_ready`, `voice_feedback`, `voice_command`. Listener exceptions are logged and swallowed. | engine.py:add_listener, _emit | port |
| A24-09 | ASR wiring | `MeetingAsrEngine(model, meeting_id, repo, language=None if "auto", term_rules=, on_backlog_status=, on_chunk_failure=, defer_load=)`; deferred load publishes handle before native load so End can cancel; unavailable local model -> error "The transcription model is not available; audio is recorded and can be transcribed later." (chunks stay pending). Ordered replay of pending SQLite chunks under `_chunk_lock` before live delivery. | engine.py:_start_asr, _load_deferred_asr, _adopt_asr, _on_chunk, _report_unavailable_asr | port (adapt to Parakeet) |
| A24-10 | Chunk result commit | Mic segments -> "Me" (`speaker_source="channel"`); loopback -> diarizer per chunk WAV; atomic `commit_chunk_transcription`; then final `speech_preview` frontier broadcast, scheduler notify, `segments` event/WS, voice-command observe, live signals. | engine.py:_on_chunk_result, _assign_speakers, _label_mic_segments | port |
| A24-11 | ASR status surfaces | Backlog: `speech.backlog`, `speech.message`. Chunk failure: `speech.chunk_error` and error text + " After fixing the audio file or speech engine, restart the app and retry this meeting from Recovery." Remote: `speech.{source:"remote",host,model,connected,message}`, "Transcribing on {host_name}". | engine.py:_on_asr_backlog_status, _on_asr_chunk_failure, _on_asr_connection_status, _record_remote_route | port (remote: undecided) |
| A24-12 | Live speech preview | Broadcast `speech_preview` only while active, not paused, and `end_s` beyond the committed frontier per channel. | engine.py:_on_speech_preview | port |
| A24-13 | Diarization lifecycle | Reasons: "Speaker identification is off in Settings." / "No speaker model is installed." / "The speaker model could not load on this computer." / "Speaker separation stopped working during the meeting." (latched once) / "The demo meeting uses channel labels."; resolver exception text wins. Relabel ops applied as actor `system`/`diarizer`; human pins forwarded to `diarizer.pin` after commit. | engine.py:_start_diarizer, _check_diarizer_degraded, _on_diarizer_relabel, _apply_diarizer_pins | port (diarizer itself is another area) |
| A24-14 | Intelligence start | Generation-guarded agent core creation/initialize; unhealthy core -> "The meeting-intelligence backend is unavailable (no API key or backend); continuing transcript-only."; exception -> finalization `unavailable` "Meeting intelligence failed to start: {exc}". Online -> finalization `pending`, `intelligence {online:true}`. | engine.py:_start_intelligence, _adopt_agent_core, _report_intelligence_unusable, _unwind_intelligence_start | undecided backend (cloud LLM) |
| A24-15 | Scheduler health | Offline after repeated failures: "Checkpoints failed repeatedly; continuing transcript-only. {reason}" where reason is "The AI provider rejected the API key (401/403). Check it in Settings → Meeting Mode → Intelligence." or "Last error: {first 200 chars}". | engine.py:_on_intelligence_health, _agent_failure_text | undecided backend |
| A24-16 | Backup agent | Once per meeting, not when equal to primary, not after End. On success: finalization `pending` "Your backup agent took over AI insights. {failure}"; on failure `unavailable` "{failure} The backup agent could not take over. {backup_error}". Repository row updated to the backup's provider/model. | engine.py:_fall_back_to_backup_agent, _switch_to_backup_agent | undecided backend |
| A24-17 | Cloud toggle mid-meeting | Only while active/paused, else "AI insights can only be changed while the meeting is active or paused."; applies `set_cloud_enabled` op as host; off -> stop scheduler, cancel, shut core, finalization `disabled`; on -> restart with send cursor seeded (forward-only). | engine.py:set_cloud_enabled, _stop_intelligence, _seed_scheduler_watermark | undecided backend |
| A24-18 | Agent activity ticks | Ring of 50, throttled 1 s per (kind, tool), host-only WS `agent_activity`; `recent_agent_activity()` for `hello`. | engine.py:_on_agent_activity, recent_agent_activity | undecided backend |
| A24-19 | Agent write gate | `allow/revoke_agent_writes`; tool-host writes only while current generation, same core, allowed, and cloud on. | engine.py:allow_agent_writes, _adopt_agent_core (writes_allowed lambda) | undecided backend |
| A24-20 | End orchestration | `end()` async on `meeting-end` thread; waits up to 120 s for an in-flight start; double End reuses the worker. Sequence: status `ending` -> scheduler `prepare_for_end` -> capture stop/flush -> cancel ASR if not ready -> drain (300 s; shutdown 30 s) -> count unfinished -> live-draft repair -> clock pause -> persist `ended`/`needs_recovery` + ended_at + paused_total_s -> stop heartbeat -> plan/announce -> WS `meeting_ended` -> `status` -> `ended` event -> redecode -> stop ASR (`asr_released`, then `background_ready`) -> speaker id -> insights -> wrap-up (final highlights if complete, scheduler stop, core shutdown, optional insight review). | engine.py:end, _begin_end, _await_start, _end_worker, _drain_end_capture | port |
| A24-21 | Completeness rule | `complete = drained and unfinished == 0 and not capture.integrity_error`; count failure counts as 1 unfinished. Incomplete -> `needs_recovery`, cloud passes refused with "Final insights are unavailable until transcription recovery finishes." | engine.py:_end_worker 914-938, _unfinished_chunks_after_drain; end_run.py:RECOVERY_PENDING_MESSAGE | port |
| A24-22 | Finalization step plan | Steps in order, each only when applicable: redecode (end_redecode and ASR has `transcribe_offline_session`), speaker_id (backend `openai` and not demo), polish (run_cloud and end_polish), consolidation (run_cloud and end_report), finalize (always). `run_cloud = cloud_enabled and complete and scheduler and core healthy and (polish or report)`. | end_run.py:EndRun.plan | port |
| A24-23 | Step display strings | Names: "Audio Re-transcription", "Speaker Identification", "Transcript Cleanup", "Summary & Action Items", "State Finalization". Default details: "High-accuracy full session Whisper decode", "OpenAI labels on the system-audio recording", "AI grammar, punctuation, and speaker formatting", "Synthesizing executive summary, key points, decisions, and action items", "Saving final transcript and consolidating meeting state". Headlines: "Re-transcribing meeting…", "Identifying speakers…", "Cleaning transcript…", "Preparing final report…", "Finalizing meeting state…"; progress: "Re-transcribing meeting (window {curr}/{total})…", "Identifying speakers (window {curr}/{total})…", "Cleaning transcript (block {curr}/{total})…". | finalization.py:STEP_NAMES, STEP_DETAILS; end_run.py:_STEP_RUNNING_MESSAGES | port (FioriVoce: "Whisper" wording should become Parakeet; decide) |
| A24-24 | Finalization announce/terminal states | Announce before `ended`: first step running, or `disabled` "AI insights are off for this meeting.", or `unavailable` recovery message, or offline message "Meeting intelligence is offline; final cloud insights could not run. {failure}". Terminal outcomes: completed with "Final insights ready — N segments[, K key points][, A action items][, D decisions]." (only non-zero), "Transcript cleanup is ready." (report off), "Post-meeting cleanup and report are off.", speaker-only outcomes, failed "{Names} failed. The recording and transcript were kept. {Name: detail ...}", exception "Final insights failed: {exc}". | end_run.py:announce, _run_cloud_passes, _complete, _report_without_cloud, _finish_after_speakers; finalization.py:insights_ready_message, failed_steps_message, saved_state_detail | port |
| A24-25 | Finalization persistence | `_set_finalization` coerces through `FinalizationState`, preserves `card_deferred`, persists via `store.update_runtime_fields`; on persist failure of a terminal state emits an ephemeral status (unlock UI without claiming success). Any terminal state adopts the topic as title for untitled meetings. | engine.py:_set_finalization, _emit_ephemeral_finalization, _adopt_untitled_title_from_topic, publish_finalization | port |
| A24-26 | Offline re-decode at End | Same `rerun_redecode` as retry, fed by the live ASR's `transcribe_offline_session` and the live diarizer; on success broadcasts `segments {items, removed_ids}`. Failure detail "Re-decoding failed; kept live transcript" or the sparse message. | engine.py:_run_offline_final_pass; end_run.py:redecode | port (adapt: Parakeet full-session decode) |
| A24-27 | Coverage guard | Optional; refuses a re-decode with < 80% of draft words: "Re-transcription produced {new} words versus {old} in the live transcript (below the 80% coverage threshold). Kept the live transcript to avoid losing speech." Empty pass: "Re-decoding produced no transcript." | refinalize.py:_coverage_refusal; finalization.py:sparse_redecode_detail | port |
| A24-28 | Re-decode commit | `replace_final_transcript` (remaps evidence/pins), `mark_chunks_done`, reload store, strip unevidenced unprotected `proposed` cards (except `user_notes`, `live_notes`), `repair_meeting_state` on final rows. Mic -> Me; loopback diarized from whole-session audio. | refinalize.py:_store_redecoded, reload_store, strip_unevidenced_proposed, assign_session_speakers | port |
| A24-29 | Cloud speaker pass gate | Order: backend must be `openai` ("Speaker identification is not set to OpenAI."), not retired (date >= 2027-02-26: "OpenAI has retired its speaker-identification model. On-device speaker labels were kept."), consent ("Audio-upload consent has not been given.", key not read), key ("No OpenAI API key is configured."). Refusal = skipped (step `completed`), never failed. Key hidden from repr. | finalization.py:speaker_pass_gate, SpeakerPassGate | undecided backend (OpenAI) |
| A24-30 | Speaker pass run | Upload announce "Uploading system audio for speaker labels…"; result "Updated N speaker label(s)" / skip reason / "Speaker identification failed." Speaker-only End outcomes: "Speaker identification finished." (+ " AI insights are off for this meeting."), skip text, failure text. | refinalize.py:run_speaker_pass, speaker_step_outcome; end_run.py:identify_speakers, speaker_outcome | undecided backend |
| A24-31 | Transcript polish | Blocks of <=80 segments and <=8000 chars, overlap min(8, len/4), oversized segment kept whole; 180 s per block; stop at first failed block: "Transcript cleanup timed out after {t:g}s on block {i}/{n}. Request ID: {id}." or "{error} (block {i}/{n}; request ID: {id})". Progress "Cleaning transcript formatting and grammar (block {i}/{n}, {k} segments)...". | finalization.py:polish_blocks, polish_transcript, POLISH_* | port algorithm; LLM undecided |
| A24-32 | Bounded agent call | Daemon thread, join timeout; on timeout run `on_timeout` (revoke writes) then `cancel`, wait 5 s grace, return None; raise -> failed result with message. | finalization.py:run_agent_call, CANCEL_GRACE_S | port (if LLM kept) |
| A24-33 | Summary stats | `{segments, words, key_points, action_items, decisions, risks, questions, duration_s}`; excludes items with status `removed` and questions `dismissed`; duration live = clock.now_s, retry = wall minus pauses. Finalize detail "Saved {segments} segments ({words} words)". | finalization.py:summary_stats; engine.py:_finalization_summary_stats; refinalize.py:_collect_summary_stats | port |
| A24-34 | Failed End | Any exception still emits `ended` (desktop leaves exclusive meeting mode only on it), stops ASR, revokes writes, keeps an already-committed terminal status (else `needs_recovery`), finalization `failed` "Meeting end failed before final insights could finish: {exc}", error `end_failed`. | engine.py:_finish_failed_end, _record_failed_end_state | port |
| A24-35 | Shutdown | Ends active meeting with 30 s drain, joins End up to 30+180+900+60 s, then marks interrupted finalization: cloud off -> `disabled`; `running` -> `failed` "Final insights were interrupted by application shutdown."; `pending` -> `unavailable` "Final insights did not run before shutdown."; stops server, agent, joins processing/diarizer threads 5 s. | engine.py:shutdown, _interrupt_finalization_on_shutdown | port |
| A24-36 | Crash detection | Session dead when heartbeat missing/unparseable/>= 60 s old, or fresh but pid not alive. Windows pid probe: `OpenProcess(0x1000)`; ACCESS_DENIED (5) = alive; `GetExitCodeProcess == 259`. POSIX `kill(pid,0)`. | recovery.py:is_session_dead, _pid_alive | port |
| A24-37 | Recovery scan | First reconciles audio of dead/terminal meetings (strict), then `find_interrupted_meetings` (status in active/paused/ending/needs_recovery OR any chunk not done), keeps dead sessions, skips `card_deferred`. Scan failures raise `RecoveryScanError` (UI must show, not treat as none). | recovery.py:find_recoverable_meetings, reconcile_startup_audio | port |
| A24-38 | Orphan WAV reconcile | For each `*.recovery.json` sidecar in spool: matching meeting, channel, valid mono 16-bit WAV; restore `.orphan` -> canonical via `os.replace`; register once by (channel, seq) using WAV header duration; delete sidecar. Invalid artifacts left untouched. | recovery.py:_recover_stranded_wavs, _restore_orphan, _read_wav_duration | port |
| A24-39 | PCM tail salvage | When no finalized `{ch}_session.wav`, read `{ch}_session.json` meta; spans: `{ch}_session.16k.pcm` prefix at `origin_s` (16 kHz) and raw `{ch}_session.pcm` at `pcm_origin_s` (if `pcm_active`, legacy rules); emit `{ch}_{seq:05d}.recovered.wav` chunks of <= 20 s past the last registered coverage, each protected by its own sidecar. Stops a channel on any failure. | recovery.py:_pcm_artifacts, _recover_pcm_tails, RECOVERED_CHUNK_MAX_S | port |
| A24-40 | Headless finalize | Reconcile, `reset_unfinished_chunks(force=True)`, build ASR (model lease when local), enqueue/requeue pending, drain 600 s, commit per chunk, progress `(done,total)`. Success: status `ended` (keeps existing ended_at) or `failed` when `capture.audio_incomplete`; finalization normalized via `normalize_historical`; raw state_json patched preserving unknown keys. Unfinished left -> `needs_recovery`, False. App messages: "Interrupted meeting finalized" / "Meeting transcription finished; captured audio remains incomplete. Review the partial meeting in History." / "Could not finalize the meeting — transcription may still be pending". | recovery.py:finalize_meeting, _complete_finalize, _mark_ended; services/runtime/meeting.py:_finalize_recovered_worker | port |
| A24-41 | Recovery follow-up | After finalize, runs retry from `redecode` (spool + end_redecode), else `polish`/`consolidation` when cloud and enabled. | services/runtime/meeting.py:_recovered_follow_up_step | port (caller area) |
| A24-42 | Retry pipeline | `rerun_finalization(from_step="failed"|id)`: steps from stored finalization (or synthesized: redecode if audio, speaker_id if asked, polish+consolidation if cloud, finalize); `failed` starts at first failed/skipped step else consolidation; runs that step and every later one; overall `failed`/`disabled`/`completed` "Final insights are ready." or ready message. Returns `{ok,state,applied,error,finalization}`; progress callback per persisted snapshot. | refinalize.py:rerun_finalization, _steps_from_state, _run_ids, _RetryRun, _retry_*, _complete_retry, _overall_from_steps | port |
| A24-43 | One run per meeting | Process-wide claim; second concurrent retry raises `FinalizationBusyError("post-meeting steps are already running for this meeting")`; a borrowed `_claim` keeps exclusivity until the caller publishes; `is_current` detects superseded claims. | refinalize.py:FinalizationClaim, _one_run_per_meeting, is_running | port |
| A24-44 | Re-run speakers | Dashboard/desktop speaker-only rerun returns pass result + state snapshot. | refinalize.py:rerun_speakers | undecided backend |
| A24-45 | Re-decode retry model load | Loads meeting's backend (remote route, catalog local backend, or LocalWhisperBackend), model lease around it, always releases; errors "The Whisper model is not available yet. Approve the download and retry." / backend last_error / "The speech model failed to load." | refinalize.py:_RedecodeBackend | adapt (Parakeet only) |
| A24-46 | Model lease | `(acquire, release)` pair so only one speech model is resident; acquire/release never raise. | refinalize.py:acquire_model_lease, release_model_lease | port |
| A24-47 | Demo meeting | Title "Demo Planning Sync", note "Demo meeting loaded — no audio is being captured.", 25 turns (me/jordan/sam, 4.0-376.0 s), participants Jordan, Sam (`others_cluster`), cards: topic, rolling summary, key point, decision (confirmed + pinned), action item, high-severity risk, timeline beat, question; clock re-anchored at last end_s. | dev_fixture.py; engine.py:_seed_demo_meeting | port (dev mode) |
| A24-48 | Platform policy | Supported: Windows always; macOS >= 13 (unknown version fails closed); Linux public support False but setup available on x86_64/aarch64 preview. OS names "macOS"/"Linux"/"this platform". macOS Screen Recording preflight/request and deep link. | platform.py | port; macOS/Linux other-platform |
| A24-49 | Time formatting | Stored UTC ISO with `Z`; legacy naive = local wall time; started-at "%b %d, %Y · %I:%M %p"; duration "{s} sec" / "{m} min" / "{h}h {m}m"; identity "{started} · {duration}"; unknown date `UNKNOWN_DATE`. | time_utils.py | port |
| A24-50 | Guests and tokens | `add_guest` clamps to 80 chars, default "Guest"; `regenerate_tokens` rotates links, re-emits `server_started`, invalidates sockets. | engine.py:add_guest, regenerate_tokens | port (web area) |
| A24-51 | Client actions | `apply_client_action`, `undo`, `request_note_adjustment` ("Turn on AI insights in an active meeting first."); guidance notify when `user_notes` or intent changes. | engine.py:apply_client_action, undo, _notify_human_guidance | port |
| A24-52 | Fast features (TypeSafe) | Live highlights status `off`/`unavailable`/`needs_insights`/`on`; LiveSignals + CitationVerifier workers; topic-shift judge; final highlights after End; consent = local judge or cloud on. | engine.py:_refresh_live_highlights_status, _start_fast_features, _finalize_highlights, _typesafe_* | undecided backend |
| A24-53 | Voice commands | Listener on previews/rows; "fix_transcript" correction op with feedback strings ('Correction not applied. Say “note taker, replace X with Y” using a term in the recent transcript.', "Transcript correction applied. Remove the spoken correction note to undo it.", "The transcript correction could not be saved."); recap request ("Preparing a recap…", "Recap added to the live notes.", "The recap produced no saved notes. Try again from the notes request box.", "The recap could not finish. Try again from the notes request box.", "Recap is unavailable while the note agent is offline."). | engine.py:_apply_spoken_action, _voice_feedback, _get_voice_command_listener | undecided backend |
| A24-54 | Term rules to ASR | Live `user_notes` corrections fed to the decoder prompt per chunk; recovery uses `repository_term_rules`. | engine.py:_active_term_rules; recovery.py:finalize_meeting | adapt (Parakeet has no text prompt; see Engine-specific) |
| A24-55 | Transcript delivery cursors | `get_transcript`, delivery page/cursor/high-water pass-throughs for consumers. | engine.py:get_transcript* | port |
| A24-56 | Insight review start | After End, if `insight_review.enabled`, `start_review(store, repo)`. | engine.py:_wrap_up_end | undecided backend |

### Added by check

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A24-57 | `ended` event payload | Normal End: `{meeting_id, canceled:false, status, unfinished_chunks, capture_error}`; failed End: `{meeting_id, canceled:false, status, error:str(exc)}`. `meeting_ended` WS message `{type, status}` is sent before the `status` and `ended` emits; on a failed End it is sent only when `ended` was not already emitted. The desktop runtime keys exclusive meeting mode off `ended`. | engine.py:_end_worker 953-961, _finish_failed_end 1173-1222 | port |
| A24-58 | End is a no-op when nothing to end | `_begin_end` returns None when not active and no End thread; `end()` after an End claim returns the existing thread; `wait_for_end` joins it. Shutdown on a never-started or already-ended engine skips the End branch. | engine.py:_begin_end, end, wait_for_end, shutdown | port |
| A24-59 | Failed-End cleanup order | `_stop_asr("failed end")` -> revoke writes -> shut agent core -> clock pause -> stop heartbeat -> persist `needs_recovery` + ended_at only if no terminal status was committed -> finalization `failed` -> status emit. Each step best-effort, never raises. | engine.py:_finish_failed_end, _record_failed_end_state | port |
| A24-60 | Wrap-up details | Final highlights only when `complete`; scheduler `stop()` then cleared; agent core shut down; insight review started only if state `insight_review.enabled`. Runs after the cloud passes, so `ended` and `background_ready` are already out. | engine.py:_wrap_up_end | port (review/highlights undecided) |
| A24-61 | Mark "ending" first | `_mark_ending` persists status `ending` and emits a status before anything slow runs, so the dashboard leaves "Live" immediately; persistence failure is logged and End continues. Scheduler `prepare_for_end` failures are logged, not fatal. | engine.py:_mark_ending, _prepare_scheduler_for_end | port |
| A24-62 | Terminal status persist is fatal | `store.update_runtime_fields(status=)` returning False raises "Could not persist terminal meeting state"; a repository failure is logged and re-raised, driving the failed-End path. Repository row gets `status`, `ended_at=now`, `paused_total_s`. | engine.py:_persist_terminal_status | port |
| A24-63 | Live-draft repair at End | `repair_meeting_state(store, get_transcript())` runs on the live transcript right after capture drains (before the offline re-decode may replace it); failure only logged "Immediate end-state repair failed". | engine.py:_repair_live_draft | port |
| A24-64 | Demo seeding side effects | Demo start sets capture `{mic_available:false, loopback_available:false, message:DEMO_NOTE}`, `diarization_available=false`, `diarization_reason=DIARIZATION_DEMO`, skips capture/ASR/diarizer; title falls back to "Demo Planning Sync" when none given. The note is passed as `note` on the first status emit. | engine.py:_seed_demo_meeting, start | port (dev mode) |
| A24-65 | Capture note on start | `capture.start(meeting_id, spool_dir)` returns a note (e.g. one source unavailable) that is delivered in the first `status` listener payload only (not on the WS copy). | engine.py:start, _emit_status | port |
| A24-66 | Token rotation contract | `regenerate_tokens` raises "No active meeting" before start; persists via `repository.replace_tokens`; re-emits `server_started {url, host_url, guest_url}`; returns `{host_url, guest_url}`; drops old sockets. `add_guest` raises "No active meeting" / ValueError(op reason or "guest_rejected"). | engine.py:regenerate_tokens, add_guest | port (web area) |
| A24-67 | Host-only broadcast fallback | A transport without `host_only` support drops host-only messages (never leaks to guests); other broadcast errors are logged and swallowed. | engine.py:_broadcast | port |
| A24-68 | Cloud-toggle ordering | On: `_start_fast_features`, allow writes, finalization `pending` (not emitted), start intelligence; off: `_stop_intelligence`. Then a status emit only if live-highlights status changed. Raises "AI insights can only be changed while the meeting is active or paused." both when End is claimed and when state status is not active/paused; store rejection raises its reason or "cloud state update failed". | engine.py:set_cloud_enabled | undecided backend |
| A24-69 | Retry parameters | `rerun_finalization` takes provider, model, endpoint, agent_core_kind, sidecar_payload_dir, store, timeout_s (consolidation, default `DEFAULT_TIMEOUT_S`), asr_model_name, language, transcribe_fn, speaker_id_backend, speaker_audio_consent, speaker_api_key, speaker_transcribe_fn, progress_cb, model_lease, redecode_coverage_guard. Raises ValueError("unknown meeting"). `have_audio` = meeting row has `spool_dir`. Speaker gate key comes from the caller, never looked up here. | refinalize.py:rerun_finalization | port |
| A24-70 | Pin forwarding to diarizer | Only results that are ok, entity `segment_speaker`, `pinned`, source `human` and with a participant id are forwarded to `diarizer.pin`; failures logged. | engine.py:_apply_diarizer_pins | port |

## Modules

### meeting/clock.py
- `MeetingClock`: `start()`, `resume_from_recovery(elapsed_s)`, `pause()`, `resume()` (both
  idempotent), `is_paused`, `is_running`, `now_s()`, `meeting_time(t_mono)`,
  `paused_total_s()`, `started_at_iso`. All under a mutex; used from audio threads.
- Edge cases: 0.0 before start (clock.py:90); paused timestamps clamp to pause instant
  (92); never negative (93); `resume_from_recovery` zeros pause credit (51).
- C++: `std::chrono::steady_clock` + `std::mutex`; inject a clock source for tests.

### meeting/time_utils.py
- `utc_now_iso()` ("...Z"), `parse_meeting_time`, `as_local_time`, `elapsed_seconds` (mixed
  naive/aware pair treated as local, 47-54), `seconds_since`, `format_meeting_started_at`,
  `meeting_duration_s(meeting, running=)` (wall minus `paused_total_s`, >= 0),
  `format_meeting_duration`, `format_meeting_identity_meta`.
- Python `isoformat()` emits microseconds (`2026-11-14T10:00:00.123456Z`); readers must
  accept both with and without fractional seconds and without zone (legacy naive).
- C++: `QDateTime::fromString(Qt::ISODateWithMs)` in the Qt layer, or a small hand parser
  in a Qt-free core; `%b %d, %Y · %I:%M %p` is English-locale, fixed (use `QLocale::c()`).

### meeting/platform.py
- `normalize_linux_machine`, `linux_meeting_implementation_ready`,
  `meeting_mode_supported(platform, machine)`, `meeting_setup_available`,
  `meeting_unsupported_os_name`, `system_audio_permission_required`,
  `system_audio_permission_granted` (Quartz `CGPreflightScreenCaptureAccess`),
  `request_system_audio_permission` (`CGRequestScreenCaptureAccess`),
  `MIN_MACOS_MAJOR=13`, `SCREEN_RECORDING_SETTINGS_URL`.
- Consumers: ui_qt meeting tab, setup dialog, unsupported dialog, system-audio dialog,
  ui_controller, capture/linux_audio.

### meeting/interfaces.py
- Constants `CHANNEL_MIC="mic"`, `CHANNEL_LOOPBACK="loopback"`.
- Dataclasses `CaptureBlock`, `SpooledChunk`, `TranscriptSegment` (speaker_source
  channel|diarizer|human, pinned, embedding bytes), `OpResult` (ok, op, reason, target_id,
  effect, inverse, current_revision, seq), `AgentConfig`, `CheckpointPayload`
  (is_consolidation/is_polish/is_notes), `AgentResult`.
- Protocols: `CaptureSource`, `Diarizer`, `AgentToolHost`, `AgentCore`,
  `MeetingRepository` (full method list at interfaces.py:281-347; the engine also calls
  `get_segments_delivery_page`, `get_delivery_cursor`, `advance_delivery_cursor`,
  `get_delivery_high_water`, `replace_final_transcript`, `mark_chunks_done`,
  `register_chunk_if_missing` which are not in the protocol).
- C++: plain structs in `src/meeting/types.h`; protocols become abstract interfaces
  (`IRepository`, `IDiarizer`, `IAgentCore`, `ICaptureSource`) so tests can fake them,
  as Python tests do (`tests/fakes/*`).

### meeting/dev_fixture.py
- `seed_demo_meeting(meeting_id, me_participant_id, store, repository, clock, title)` ->
  `{segment_count, last_end_s, participant_ids}`; raises RuntimeError when a participant
  op fails or no host. Ops are applied as actor `system`/`demo`. The decision card is
  confirmed and pinned so it survives proposed-card cleanup. Data table at 23-86 must be
  copied verbatim (it is also golden input for End tests).

### meeting/end_run.py
- `EndRun`: holds no engine state; all engine effects are injected callables
  (`set_finalization`, `redecode`, `redecode_failure`, `identify_speakers`,
  `allow/revoke_agent_writes`, `core_is_healthy`, `offline_message`, `summary_stats`).
- Methods in call order: `plan()`, `announce()`, `redecode()`, `identify_speakers()`,
  `insights()` -> `_run_cloud_passes()` (`_polish`, `_consolidate`, `_complete`) or
  `_report_without_cloud()` (`_finish_after_speakers`).
- `update_step` logs at WARNING for failed; `current_step` defaults to 1 when the id is
  unknown (119-126).
- Edge cases: incomplete meeting with redecode marks finalize completed "Saved meeting
  state after re-transcription." and finishes `unavailable` with steps so the card never
  shows redecode as running (392-411); `_polish` reads outcome `.status`/`.message`
  (303-313); `_consolidate` default status `failed` (345); any failed step forces overall
  `failed` (368-370); exceptions in cloud passes revoke writes and finish `failed` with
  stage `failed` (270-281). With cloud off and no speaker step and no redecode, the
  outcome is `disabled`; with redecode and cloud off nothing in `_report_without_cloud`
  sets a terminal state other than through the branches listed: note
  `elif not self.cloud_enabled` sets `disabled` without steps (390-391) even after a
  completed redecode. Keep this behavior identical (test
  `test_cloud_off_replaces_transcript_when_redecode_enabled`).

### meeting/finalization.py
- `run_agent_call`, `CANCEL_GRACE_S=5.0`, `POLISH_TIMEOUT_S=180.0`,
  `POLISH_MAX_SEGMENTS=80`, `POLISH_MAX_TEXT_CHARS=8000`, `POLISH_OVERLAP_SEGMENTS=8`,
  `polish_blocks`, `polish_transcript`, `summary_stats`, `saved_state_detail`,
  `insights_ready_message`, `sparse_redecode_detail`, `STEP_ORDER`, `STEP_NAMES`,
  `STEP_DETAILS`, `NOT_OPENAI_REASON`, `NO_CONSENT_REASON`, `NO_KEY_REASON`,
  `SpeakerPassGate`, `speaker_pass_gate`, `make_step`, `failed_steps_message`.
- `polish_blocks` (82-111): loop invariant every block advances past `covered`; if the
  overlap context alone exhausts the text budget, restart at `covered` with no overlap.
- `failed_steps_message` falls back to step id when name missing (test
  `test_legacy_failure_without_detail_and_success`: "polish failed").
- Also used by agent scheduler, sidecar, openrouter_direct, installed core (timeout
  constant) and reinsight (`run_agent_call`).

### meeting/recovery.py
- Public: `RecoveryScanError`, `STALE_HEARTBEAT_S=60`, `FINALIZE_DRAIN_TIMEOUT_S=600`,
  `RECOVERED_CHUNK_MAX_S=20`, `reconcile_meeting_audio(repo, meeting, strict=)` ->
  `{orphan_chunks, tail_chunks}`, `reconcile_startup_audio`, `is_session_dead`,
  `find_recoverable_meetings`, `finalize_meeting(repo, meeting, asr_model, asr_language,
  on_progress, model_lease)`. Private `_pid_alive` is imported by
  `services/opencode_component.py:199`.
- Threading: synchronous; caller runs it on a worker (`meeting-recover` thread). The ASR
  engine it builds has its own workers; `on_chunk_result` commits from the ASR thread.
- Edge cases to keep: reload chunk inventory after orphan registration so the PCM tail
  does not overlap (400); idempotent registration fallback for repos without
  `register_chunk_if_missing` (84-93); trust WAV header duration over sidecar (184-186);
  orphan with canonical already present keeps orphan path (113); legacy raw PCM with prefix
  and no `pcm_origin_s` is left for manual salvage (255-264); a tail write failure stops
  that channel without advancing (356-363); `cursor_s >= artifact_end - 0.5/rate` skip
  (316); sample rounding `floor(x*rate + 0.5)` (318-320); `pcm_slice_to_wav` returning
  <= 0 stops (334); meeting without `ended_at` gets now, otherwise kept (712-715);
  unparseable state_json left untouched (720-722); `audio_incomplete` -> `failed` (725-729);
  remote route language preferred over setting (652-657); finalize uses
  `saved_remote_route(meeting)` and no model lease for remote.

### meeting/refinalize.py
- Public: `FinalizationBusyError`, `FinalizationClaim`, `is_running`, `rerun_finalization`,
  `rerun_redecode`, `rerun_polish`, `rerun_speakers`, `run_speaker_pass`,
  `speaker_step_outcome`, `acquire_model_lease`, `release_model_lease`,
  `OPTIONAL_RERUN_STEPS`, `reload_store`, `strip_unevidenced_proposed`,
  `assign_session_speakers`.
- Threading: blocking; called on worker threads from the desktop runtime and from the web
  API (`asyncio.to_thread` via `ctx.run_post_meeting_pass`). Global claim set under a
  module mutex; `_latest_claims` is a weak map.
- Retry semantics: `_steps_from_state` reuses stored steps and inserts a missing requested
  step in `STEP_ORDER` position (249-256); `_run_ids` with unknown `from_step` returns
  `[from_step]` if valid else empty (298-299); `finalize` step returns immediately and
  later steps are not run (999-1000); `_complete_retry` adds a completed finalize when
  absent (1177-1181); redecode step does not count `applied` (1085); speaker step adds
  `applied` even when skipped (1103); consolidation `ValueError` becomes a failed step
  (1143-1144); `card_deferred` preserved on every persisted snapshot (356).
- `rerun_redecode` order: transcribe -> empty check -> coverage guard -> diarizer choice
  (`off` none; default builds fresh only when loopback present) -> speaker assignment ->
  store. Backend always released in `finally` (498-499).
- `rerun_polish` returns `{ok:True, applied:0}` when no segments (673-674);
  `AgentUnavailable` maps to an error result.

### meeting/engine.py
- Public surface: `MeetingEngineOptions` (fields at 84-129), `MeetingEngine(options,
  repository=None)`, attributes `meeting_id`, `store`, `clock`, `repository`,
  `model_lease`; methods `add_listener`, `start`, `pause`, `resume`, `end`, `wait_for_end`,
  `shutdown`, `wait_for_processing`, `is_active`, `set_cloud_enabled`, `regenerate_tokens`,
  `request_note_adjustment`, `apply_client_action`, `undo`, `add_guest`,
  `publish_finalization`, `allow/revoke_agent_writes`, `agent_writes_allowed`,
  `recent_agent_activity`, `get_transcript`, delivery helpers. AgentToolHost is
  `reinsight.StoreToolHost`, not the engine itself.
- Threads: caller thread for `start`; `meeting-processing-start`; `meeting-diarizer-start`;
  `meeting-heartbeat`; `meeting-end`; `meeting-backup-agent`; plus ASR, capture, diarizer,
  agent reader and web-server threads calling back in. Locks: `_lifecycle_lock` (RLock),
  `_listener_lock`, `_voice_listener_lock`, `_chunk_lock`, `_activity_lock`,
  `_intelligence_start_lock` (RLock), `_fast_features_lock` (RLock). Events:
  `_start_complete`, `_processing_complete`, `_processing_stop`, heartbeat stop.
- State machine (meeting status): `active` <-> `paused` -> `ending` -> `ended` |
  `needs_recovery`; `failed` on aborted start (with artifacts) or recovery with lost audio.
  Engine flags: `_starting`, `_active`, `_end_thread` (claimed), `_asr_ready`,
  `_asr_release_emitted`, `_degraded_diarization`, `_intelligence_generation`,
  `_intelligence_restarted`, `_backup_agent_tried`, `_fast_features_stopped`.
- Finalization state machine (`FinalizationState.status`): `pending` | `running` |
  `completed` | `disabled` | `unavailable` | `failed`; terminal = last four.
- Constants: `HEARTBEAT_INTERVAL_S=10`, `END_DRAIN_TIMEOUT_S=300`,
  `SHUTDOWN_DRAIN_TIMEOUT_S=30`, `CONSOLIDATION_TIMEOUT_S=900`,
  `START_WAIT_TIMEOUT_S=120`, `MAX_GUEST_NAME_LEN=80`, `AGENT_ACTIVITY_HISTORY=50`,
  `AGENT_ACTIVITY_MIN_INTERVAL_S=1.0`.
- Edge cases a port must keep:
  - End racing start waits for start to finish (827-846; test
    `test_end_racing_start_waits_for_the_pipeline`); start on its own thread does not wait
    for itself (840).
  - `_asr_release_emitted` emits `asr_released` exactly once, even when ASR never existed
    (307-309); `asr_released` comes after `ended` and after the offline pass;
    `background_ready` only if stop succeeded and after `asr_released` (968-969).
  - Deferred ASR: handle published before native load; End during load stops it (1531-1547);
    `_adopt_asr` re-checks stop flag; chunk replay under `_chunk_lock` before
    `_asr_ready=True` (1577-1583).
  - `_on_chunk` is idempotent per chunk id and defers to SQLite while not ready (1476-1497).
  - `_on_chunk_result` returns without publishing when commit reports not committed
    (1648-1649).
  - Diarizer adoption and its status write are one guarded transition after blocking
    probes (1753-1765).
  - Intelligence generation checks before and after `initialize` so late initializers
    never adopt or overwrite newer consent (1949-1989).
  - Releasing cores: pending initializer shut down unless it is the adopted core; activity
    callback detached before shutdown (1422-1442).
  - `_set_finalization` refuses unknown statuses; returns False when store missing.
  - Pause/resume rejected after End claimed (796-798).
  - Failure logging: `FailureLog` warns first failure per key, debug on repeats, and
    `type_only=True` for paths that may carry transcript text or keys (privacy).

## Data and formats

**Settings (read by the caller `services/runtime/meeting.py:_build_options`, 1078-1142, and
passed as `MeetingEngineOptions`):**

| Key | Type | Default | Engine field |
|---|---|---|---|
| `meeting_whisper_model` | str | "auto" | asr_model |
| `meeting_language` | str (auto / ISO-639-1) | "auto" | asr_language |
| `meeting_asr_source` (+ remote route keys) | str | local | asr_remote |
| agent route keys (`resolve_meeting_agent_route`) | | | llm_provider, llm_model, llm_endpoint, agent_core_kind, sidecar_payload_dir |
| `meeting_backup_agent` | str ("" or installed id) | "" | backup_agent_kind (+ model via resolve_meeting_agent_model) |
| `meeting_speaker_id_backend` | off/local/openai | "local" | speaker_id_backend (demo forces local) |
| audio upload consent (`resolve_meeting_audio_upload_consent`) | bool | False | speaker_id_audio_consent |
| `meeting_server_bind` | localhost/lan | "localhost" | server_bind |
| `meeting_server_port` | int 0-65535 | 0 | server_port |
| `meeting_end_redecode` | bool | False | end_redecode (demo forces False) |
| `meeting_redecode_coverage_guard` | bool | False | redecode_coverage_guard |
| `meeting_end_polish` | bool | True | end_polish |
| `meeting_end_report` | bool | True | end_report |
| insight review / report views | dict / tuple | ('ribbon','brief','signal') | insight_review, report_views |
| `meeting_cloud_consent_given`, `meeting_cloud_last_enabled` | bool | False | (caller gates cloud_enabled) |
| TypeSafe flags (`resolve_typesafe_enabled`, `_feature_enabled("highlights")`, `_topic_shift_enabled`, `_voice_commands_enabled`, `_voice_command_names`) | | | read live by the engine |

`spool_root` = `config.MEETINGS_FOLDER`; engine fallback "meeting_audio" relative.

### Added by check

Further `MeetingEngineOptions` fields not in the table above (all with defaults):
`title` (''), `intent` (''; clamped to `MAX_INTENT_LEN`, `updated_at` set only when
non-empty), `mic_priority` (tuple of `{name, hostapi}`; empty = system default),
`mic_device_id` (legacy index), `diarization_model_path`, `diarization_model_resolver`
(callable run on the processing worker; raises a user-facing sentence), `demo_mode`,
`system_audio_policy` (`auto`|`required`|`disabled`, session only, never persisted),
`llm_provider` default `openrouter`, `agent_core_kind` default `pi`. Defaults for the
rest match the table. No environment variables are read anywhere in this area.

Module constants not listed elsewhere: `DIARIZATION_OFF/NO_MODEL/LOAD_FAILED/DEGRADED/DEMO`
(strings in A24-13), `_INTELLIGENCE_OFFLINE_MESSAGE`, `INSIGHTS_OFF_MESSAGE`,
`RECOVERY_PENDING_MESSAGE`, `_STEP_RUNNING_MESSAGES` (note: no entry for `finalize`),
`_PROCESS_QUERY_LIMITED_INFORMATION=0x1000`, `_STILL_ACTIVE=259`,
`_ERROR_ACCESS_DENIED=5`, `OPTIONAL_RERUN_STEPS={redecode, speaker_id, polish,
consolidation}`, `_NEW_DIARIZER` sentinel, `UNKNOWN_DATE` (from format_utils).

Listener `ended` payload: `{meeting_id, canceled:false, status, unfinished_chunks,
capture_error}` (plus `error` on failed End); see A24-57. Source comments list fewer
listener kinds than the engine really emits; the map's A24-08 list is authoritative.

Meeting row written at start (all columns): id `m_...`, title, status `active`,
started_at, ended_at null, paused_total_s 0.0, host/guest tokens, cloud_enabled,
asr_model, asr_remote_json (JSON or null), agent_provider, agent_model,
agent_endpoint_json (JSON or null), spool_dir, state_json null, state_seq 0, app_pid,
app_heartbeat_at. The backup agent later rewrites provider/model.

**Database (SQLite, SQLAlchemy models in `services/models.py`):**
- `meeting_sessions`: id (PK, `m_...`), title, status (active|paused|ending|ended|failed|
  needs_recovery), started_at, ended_at, paused_total_s, host_token, guest_token,
  cloud_enabled, asr_model, asr_remote_json, agent_provider, agent_model,
  agent_endpoint_json, spool_dir, state_json (full MeetingState JSON, including
  `finalization`, `capture.audio_incomplete`, `speech`, `cloud_enabled`), state_seq,
  app_pid, app_heartbeat_at, origin_device_id, origin_device_name.
- `meeting_audio_chunks`: id, meeting_id, channel, seq (unique per meeting+channel),
  file_path, start_s, duration_s, sample_rate, asr_status (pending|processing|done|failed),
  asr_attempts, asr_error.
- `meeting_segments`: id (`sg_...`), meeting_id, chunk_id (nullable; demo/redecode rows
  are null), channel, start_s, end_s, text, speaker_participant_id, speaker_source,
  speaker_pinned, embedding, created_at.
- Also events/state tables via `on_ops_applied` (state area).

**Files on disk (spool dir `<MEETINGS_FOLDER>/<meeting_id[:12]>/`):**
- Chunk WAVs (16 kHz mono int16) registered in DB; `<wav>.recovery.json` transaction
  sidecar `{meeting_id, channel, seq, start_s, duration_s, sample_rate}`; `<wav>.orphan`.
- `{ch}_session.pcm` (native rate raw), `{ch}_session.16k.pcm` (converted prefix),
  `{ch}_session.json` (`sample_rate`, `origin_s`, `pcm_origin_s`, `pcm_active`),
  `{ch}_session.wav` (final; its presence means no salvage).
- `{ch}_{seq:05d}.recovered.wav` salvage chunks.

**Wire (WebSocket broadcast from the engine):** `status` (A24-07),
`segments {items, removed_ids?}`, `speech_preview {channel,text,start_s,end_s,final}`,
`meeting_ended {status}`, `agent_activity {kind,label,tool,pass_kind,ts}` (host only),
`voice_feedback {phase, message, seq}`. Finalization object:
`{status, message, stage, current_step, total_steps, step_details, steps:[{id,name,status,detail}], summary_stats, card_deferred}`.
Step status values: pending|running|completed|failed (and `skipped` read by `_run_ids`).
REST routes that call into this area: `POST /api/meetings/{id}/reinsights`,
`POST /api/meetings/{id}/respeakers` (meeting/web/api.py:813-897; 409 while
finalization or insight review is `running`).

Existing installs depend on: the DB columns above, state_json shape, spool file naming
(recovery of meetings recorded by the Python app), UTC `Z` timestamps and legacy naive ones.

## External dependencies

| Python | Use | C++ / Qt 6 |
|---|---|---|
| threading (Thread, Lock, RLock, Event) | workers, guards | `std::thread`/`QThread`, `std::mutex`, `std::recursive_mutex`, `std::condition_variable` or a small `Event` class |
| time.monotonic | clock, throttles | `std::chrono::steady_clock` |
| datetime/timezone | ISO UTC, local display | `std::chrono` + `QDateTime` in Qt layer |
| secrets.token_urlsafe(32) | host/guest tokens | `QRandomGenerator::system()` + base64url (43 chars) |
| json | state_json patching preserving unknown keys | `QJsonDocument` or nlohmann/json (needs raw-preserving patch) |
| wave | WAV header validation | own RIFF reader (already needed for spool) |
| os.scandir/replace/getsize/makedirs | spool scans, orphan restore | `std::filesystem` (`rename` is atomic replace on same volume; on Windows use `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`) |
| ctypes kernel32 OpenProcess/GetExitCodeProcess | pid liveness | Win32 directly; POSIX `kill(pid,0)` |
| Quartz CGPreflight/RequestScreenCaptureAccess | macOS grant | CoreGraphics C API |
| platform.mac_ver/machine | platform policy | `QSysInfo` / `uname` |
| numpy | frame arrays in interfaces | `std::vector<int16_t>` / `std::span` |
| re | 401/403 detection | `std::regex` or manual |
| weakref.WeakValueDictionary | latest claim | `std::weak_ptr` map |
| collections.deque(maxlen) | activity ring | fixed ring buffer |
| SQLAlchemy (via repository) | persistence | other area (Qt SQL or sqlite3) |

## Platform-specific code

- `recovery.py:_pid_alive` (433-474): Windows OpenProcess probe (never `os.kill`, which
  terminates on Windows); POSIX `kill(pid, 0)` with EPERM = alive.
- `platform.py`: Windows supported; macOS >= 13 with Screen Recording TCC grant for system
  audio (preflight never prompts; request prompts once per binary; deep link
  `x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture`);
  Linux x86_64/aarch64 "preview" (setup available, not publicly supported). No
  X11/Wayland/Hyprland distinctions in this area.
- `MeetingEngineOptions.system_audio_policy` (`auto`/`required`/`disabled`): `required` is
  the Linux dual-channel readiness mode (consumed by capture_session).
- Paths: spool paths stored as strings in DB; C++ must read Python-written paths (Windows
  backslashes) and use `path_from_utf8`.

## Engine-specific code

Drop or rework:
- `refinalize.py:_RedecodeBackend.load` (532-551): `LocalWhisperBackend`,
  `services.local_asr.catalog.MODELS` + `LocalSpeechBackend` (faster-whisper, Moonshine,
  Qwen3, Parakeet MLX, unified engine) -> drop; replace with loading the Parakeet
  `Recognizer` (or reusing the app's loaded one). Error string mentioning Whisper becomes
  a Parakeet/model-missing sentence.
- `finalization.py:STEP_DETAILS["redecode"]` says "Whisper decode" -> reword (deliberate
  difference).
- Model lease (`ModelLease`, `release_local_engine`/`restore_local_engine`) exists so only
  one Whisper model is resident. With in-process Parakeet the meeting can share the
  dictation `Recognizer` on `EngineWorker` instead of leasing; keep the API shape so
  recovery/retry still serialize with dictation, but it may become a no-op or a
  "pause dictation" lease. Never unload the runtime (CLAUDE.md).
- Term rules (`_active_term_rules`, `repository_term_rules`) prime Whisper's text prompt.
  Parakeet accepts speech contexts (boosting) in the C ABI, not an initial prompt; adapt to
  speech contexts if the runtime supports them for TDT, else apply corrections as text
  post-processing only.
- `language` pin: Parakeet TDT English-only models ignore it; keep plumbing for
  multilingual Parakeet.
- `transcribe_offline_session` (ASR area) is a full-session re-decode; with Parakeet it is
  windowed `Recognizer::transcribe` over session audio. The "window {curr}/{total}"
  progress contract stays.

Undecided backends (mapped, marked): remote ASR route (`asr_remote`,
`saved_remote_route`, `MeetingRemoteBackend`, `_on_asr_connection_status`,
`_record_remote_route`), OpenAI speaker pass (`cloud_pass`, `speaker_pass_gate`),
all LLM intelligence (agent cores, scheduler, polish, consolidation, backup agent,
note adjustment, insight review), TypeSafe fast features and voice commands.

Must adapt: `MeetingAsrEngine` construction (`defer_load`, `load_backend`, `drain`,
`requeue_pending`, `start_preview`, `stop`, `is_available`, `last_error`) becomes a
Parakeet-backed meeting ASR that posts to `EngineWorker`'s thread rather than owning a
model; the Python worker-process plumbing behind it is dropped.

## Dependencies on other areas

Calls out (lazy imports in Python):
- Capture: `meeting.capture_session.MeetingCapture`, `FailureLog`, `where_it_failed`
  (start/stop/flush/integrity_error/spools/check_spool_health); `meeting.capture.spool`
  helpers (paths, recovery meta, `pcm_slice_to_wav`, `TARGET_RATE=16000`).
- ASR: `meeting.asr.engine.MeetingAsrEngine`, `meeting.asr.audio.load_wav_int16`,
  `meeting.asr.offline.load_channel_session`, `transcribe_meeting_sessions`,
  `meeting.asr.remote.saved_remote_route`, `MeetingRemoteBackend`.
- Diarize: `meeting.diarize.clustering.create_diarizer`, `meeting.diarize.assign.
  assign_from_frames`, `refresh_labels`, `meeting.diarize.cloud_pass.run_cloud_speaker_pass`.
- State: `meeting.state.schema` (MeetingState, FinalizationState incl. `coerce`,
  `normalize_historical`, `default_for_cloud`, `FINALIZATION_STATUSES`, CARD_KEYS,
  CardItem.protected, MeetingIntent, new_id, now_iso, parse_state_json),
  `meeting.state.store.MeetingStateStore` (apply, undo, snapshot, with_state,
  update_runtime_fields, replace_document), `state.segment_ops.make_segment_handler`,
  `state.repair.repair_meeting_state`, `state.patches.MAX_INTENT_LEN`,
  `repository_segment_lookup`.
- Persistence: `meeting.persist.repository.SqlMeetingRepository`,
  `meeting.persist.data_lifecycle.delete_meeting_data`; `meeting.stored.open_store`,
  `finalization_from_meeting_row`.
- Web: `meeting.web.server.MeetingWebServer` (start, stop, broadcast(host_only=),
  host_url, guest_url, base_url, invalidate_connections), `meeting.web.auth.
  generate_token_pair`.
- Agent: `meeting.agent.base.create_agent_core`, `agent.scheduler.CheckpointScheduler`
  (start, stop, prepare_for_end, notify_segments, notify_guidance, seed_sent_segments,
  run_final_polish, run_consolidation, request_note_adjustment), `agent.prompts.
  build_system_prompt`, `agent.typesafe_signals.topic_shift_probability`;
  `meeting.reinsight` (StoreToolHost, stored_agent, AgentUnavailable, rerun_insights,
  DEFAULT_TIMEOUT_S).
- Other meeting modules: content.build_untitled_title_ops, insight_review
  (review_config, start_review), live_signals.LiveSignals, citation_verifier,
  voice_commands, voice_actions.correction_op, corrections (term_rules_from_items,
  repository_term_rules).
- Services: `services.transcript_cleanup.find_api_key`, `services.openai_retirement`,
  `services.settings` resolvers, `services.typesafe`, `services.components.
  speaker_model_path`, `services.format_utils.UNKNOWN_DATE`.

Called in by:
- `services/runtime/meeting.py` (MeetingRuntime: engine lifecycle, listener event
  dispatch incl. `asr_released`/`background_ready`, recovery scan and finalize,
  retries with `FinalizationClaim`, `is_running`).
- `services/meeting_rerun.py` (gate and rerun options), `services/history_calendar.py`,
  `services/opencode_component.py` (`_pid_alive`).
- `meeting/web/api.py` (reinsights/respeakers, duration), `meeting/web/server.py`
  (engine client actions, activity, tokens, transcript).
- `meeting/agent/*` (POLISH_TIMEOUT_S, run_agent_call), `meeting/capture/spool.py`
  (MeetingClock), exports (time_utils), ui_qt widgets/dialogs (platform, time_utils).

## Tests

| File | Pins |
|---|---|
| tests/test_meeting_engine.py (2581) | deferred processing startup ordering, End cancelling loads, required services (server failure discards empty meeting, zero sources, mic-only, required/disabled system audio), intelligence health and offline wording, backup agent (once, not while ending, both reasons), cloud toggle forward-only cursor, full End lifecycle (spool loss -> needs_recovery, event ordering `ended` < `asr_released` < `background_ready`, polish/report on/off, redecode with own diarizer, needs_recovery after redecode terminal state, failed End still emits `ended`), diarization reasons and degradation latch, store wiring, demo meeting, cloud speaker step, agent write gate, terminal finalization adopts topic title, `card_deferred` preserved, sparse redecode message, insight review start, final highlights. Uses fakes in tests/fakes/ (agent_core.py, scheduler.py) and `tests/helpers.py`. |
| tests/test_meeting_finalization.py (126) | failed_steps_message, insights_ready_message, saved_state_detail exact strings; speaker gate ordering and repr hiding; run_agent_call result/raise/timeout order (revoke before cancel). |
| tests/test_meeting_polish_batches.py (64) | polish_blocks coverage, text budget, oversized segment, always advances. |
| tests/test_meeting_refinalize.py (890) | coverage guard, redecode replace + repair, one run per meeting, step selection from `failed` / `redecode`, polish failure and block bounds, human-pinned cards survive, endpoint snapshot, card_deferred kept, model lease bracketing/release on success and failure, speaker gate skip/upload, off backend no re-diarize, borrowed claim semantics. |
| tests/test_meeting_recovery.py (540) | _mark_ended normalization (running -> failed, pending -> settled, audio_incomplete -> failed), pid/heartbeat liveness, recoverable filtering and deferred skip, scan failures raise, finalize_meeting branches (no id, list failure, no pending, transcribes, unavailable ASR, drain timeout, unfinished -> needs_recovery), reconcile orphan once, sidecar after commit, PCM tail only unregistered part, raw already converted ignored, startup scan of terminal meeting with stranded WAV. |
| tests/test_meeting_clock.py (83), tests/test_meeting_time_utils.py (60) | clock semantics with mocked monotonic; UTC/legacy parsing, elapsed across mixed pairs, running duration. |
| tests/test_meeting_dev_fixture.py (66) | demo seed writes transcript and cards. |
| tests/test_meeting_unsupported_platform.py (527) | platform policy (first class) plus UI dialog/tab locking (other areas). |
| tests/test_meeting_cloud_speakers.py (436) | rerun_speakers headless store, unknown meeting raises, refused gate uploads nothing (rest is cloud_pass area). |
| tests/test_meeting_runtime.py (2149) | caller contract: recovery scan emits/silent/failure retryable, finalize_recovered messages and follow-up step, retry claim/publication races, background handoff. |
| tests/test_meeting_failure_logging.py (207) | FailureLog warn-once, type-only privacy, diarization chunk reload skip. |
| Also touching: test_meeting_remote.py, test_meeting_fast_features.py, test_live_signals_lifecycle.py, test_meeting_intent.py, test_meeting_title_consistency.py, test_voice_commands.py, test_meeting_voice_preview.py, test_meeting_diarize.py, test_meeting_sidecar.py, test_meeting_scheduler.py | engine integration points for other areas. |

Golden-vector candidates (pure, deterministic): `polish_blocks` (segment lengths ->
block index ranges), `summary_stats` + `insights_ready_message` + `saved_state_detail` +
`failed_steps_message`, `speaker_pass_gate` (with injected date), `_steps_from_state` +
`_run_ids` + `_overall_from_steps`, `EndRun.plan`/announce/outcome with fake callables
(record the `set_finalization` call sequence), `time_utils` formatters and
`meeting_duration_s`, `MeetingClock` with scripted monotonic times,
`FinalizationState.normalize_historical` via `_mark_ended`, `_recover_pcm_tails` chunking
(rates, origins, coverage -> list of (seq, start_s, frames)), `_agent_failure_text`.

## Porting notes

Decomposition:
- Qt-free core `src/meeting/` (namespace `fv::meeting`, no Qt, like `src/engine`):
  `clock`, `time_utils`, `types` (interfaces), `finalization` (strings, stats, blocks,
  gate, step helpers), `end_run`, `refinalize` (retry pipeline + claims), `recovery`
  (reconcile, liveness, headless finalize), `demo_fixture`, `platform_policy`.
  All take abstract `IRepository`, `IStateStore`, `IMeetingAsr`, `IDiarizer`,
  `IAgentCore`, `IScheduler` so they test with fakes and golden vectors.
- `MeetingEngine` orchestrator: Qt-free core class with a listener callback interface and
  explicit worker threads (mirrors Python); a thin Qt adapter (`MeetingEngineBridge`,
  QObject) converts listener events to queued signals for the GUI, matching the existing
  `EngineWorker` rule that engines are reached only through queued signals.
- Speech: meeting ASR submits chunk decode jobs to the shared Parakeet `Recognizer` on
  `EngineWorker` (one runtime thread); drop the lease/unload dance; offline re-decode
  = windowed transcribe over the session WAV.

Order of work: (1) types, clock, time_utils, platform policy; (2) finalization helpers
with golden vectors; (3) repository + state store (other areas) to the interfaces used
here; (4) recovery reconcile + liveness + headless finalize (needed for crash safety
before shipping capture); (5) engine start/pause/end without intelligence (cloud off path,
redecode, speaker off/local); (6) EndRun and retry pipeline; (7) demo fixture; (8)
undecided backends (LLM intelligence, backup agent, OpenAI speakers, remote ASR, TypeSafe,
voice commands) once decided.

Risks:
- Lifecycle races are many and test-pinned (End vs start, End vs deferred model load,
  late agent initialize, diarizer adoption); a straight port with coarse locking will
  either deadlock (RLocks re-entered from callbacks) or leak late state writes.
- Exact strings and event ordering are consumed by the desktop card, dashboard JS and
  tests; drift breaks parity silently.
- Recovery must read spools and DB rows written by the Python app (file naming, sidecar
  JSON, legacy naive timestamps, legacy PCM metadata without `pcm_origin_s`).
- Patch state_json preserving unknown keys: a typed C++ model round-trip would drop them.
- Large share of engine.py (~40%) is LLM/TypeSafe/voice plumbing whose fate is undecided;
  stub behind interfaces so the core lands first.
- Model lease semantics change with a shared in-process recognizer; recovery while
  dictation is active must still serialize runtime calls.

Rough C++ size: core helpers ~1,400 lines (finalization 350, end_run 350, refinalize
900, recovery 650, clock/time/platform 300, demo 250 — overlapping), engine orchestrator
~2,000 without undecided backends, +~900 with them; Qt bridge ~250. Total about
4,500-5,500 lines plus tests.

Native gains: no Python worker processes or model reloads for re-decode (reuse the loaded
Parakeet model; End-to-ready latency drops by the model load time); PCM salvage and WAV
validation stream with memory-mapped I/O; heartbeat and liveness are cheap; startup
recovery scan can run before the GUI shows without import cost.
