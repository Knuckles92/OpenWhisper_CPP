# Meeting capture (mic and system audio), meeting ASR, diarization, playback

Area a25. Source root: `D:\coding\whisper_local`. 24 files, ~8.9k lines, all read in full:
`meeting/capture_session.py`, `meeting/capture/{__init__,devices,health,linux_audio,sck_stream,sd_stream,soundcard_stream,spool}.py`,
`meeting/asr/{__init__,audio,engine,hallucination,offline,preview,remote}.py`, `meeting/audio_playback.py`,
`meeting/diarize/{__init__,assign,cloud_audio,cloud_pass,clustering,embedder,fbank}.py`.

This area has **no Qt widgets**. Everything here is headless back end driven by the meeting engine (a24); the user
sees its results through the meeting dashboard (web, a30) and the Qt meeting UI (a31): capture status messages,
live caption previews, transcript rows labelled Me / Others / "Speaker N", the playback WAV, and error banners. The
user-visible strings below are what those surfaces show verbatim, so they must be kept word for word.

## What the user gets

- **Two-channel recording of a meeting.** "Me" = the microphone (ranked mic list from Settings, else system default,
  else any input). "Others" = system audio (what the speakers play): WASAPI loopback on Windows, ScreenCaptureKit on
  macOS 13+, PulseAudio / PipeWire-Pulse monitor on Linux. Either channel can be missing; the meeting degrades to
  mic-only (or loopback-only) with a one-line note, and fails to start only when *no* channel opens, or when the
  session's system-audio policy is `required` and loopback is unavailable.
- **Self-healing capture.** A 1 s watchdog restarts a dead or stalled source within 12 s, follows default-device changes
  (for "System default" mic and for loopback), skips a microphone that failed for the rest of the meeting, and keeps all
  audio recorded before a restart in the same per-channel spool.
- **Crash-safe audio.** Each channel is cut into ~5-20 s 16 kHz mono WAV chunks at quiet points, written atomically,
  registered in SQLite as `pending` *before* the ASR is told; plus a continuous native-rate session PCM that becomes a
  16 kHz `<channel>_session.wav` at the end. A hard kill at any point leaves files that startup recovery (a24) can
  re-register.
- **Live transcript.** A background worker decodes each chunk, stamps segments in meeting time, and commits them through
  the engine. If the worker falls behind, audio stays on disk and the user sees "Live transcription is behind; audio is
  saved and captions will catch up." then "Live transcription caught up."
- **Live caption preview** (Parakeet): about once a second, the last <= 8 s of each channel is decoded best-effort and
  shown as non-final `speech_preview` text, never delaying durable decoding.
- **End-of-meeting clean re-decode** (optional setting): the continuous session WAV is re-cut on longer pauses and
  re-decoded; mic segments that merely echo system audio are dropped.
- **Speaker identification on the Others channel**: on-device (WeSpeaker ONNX embeddings + online clustering into
  "Speaker 1..N", human pins win), or an optional post-meeting OpenAI `gpt-4o-transcribe-diarize` pass.
- **Seekable playback** of the whole meeting as one mono WAV (mic and loopback mixed) served by the dashboard.
- **Remote speech host** option: meeting audio decoded on a paired computer, with saved-route identity checks.

## Feature inventory

Disposition: **port**, **drop (engine-specific)**, **undecided backend**, **other-platform only** (port, but only
meaningful on the named OS).

| ID | Feature | Exact user-visible behaviour | Source | Disposition |
|---|---|---|---|---|
| A25-01 | Meeting capture start | Opens mic, then system audio (unless policy `disabled`). Notes joined with spaces: "Microphone capture is unavailable." / "System audio disabled for this meeting by your microphone-only choice." / "System-audio capture is unavailable; recording microphone only." / "System-audio capture is unavailable." No channel: raises "No audio devices could be opened." Capture layer import failure: "Audio capture is unavailable: {exc}". | capture_session.py:MeetingCapture.start, _notes | port |
| A25-02 | System-audio policy | Per-session (never persisted) `auto` / `required` / `disabled`. `required` + no loopback raises `"LINUX_SYSTEM_AUDIO_REQUIRED: System-audio capture is required but unavailable."` (the prefix is a code a24/a31 match on). `disabled` skips loopback and the watchdog never touches it. | capture_session.py:_open_system_audio:231-259 | port |
| A25-03 | Loopback backend order | WASAPI `[Loopback]` input via PortAudio first, verified with `is_active()`; else ScreenCaptureKit (macOS), else SoundCard (Windows fallback, Linux main path). Each backend's `available()` rules itself out. | capture_session.py:start_fallback_loopback:282-315 | port |
| A25-04 | Mic selection and failover | Ranked `{name, hostapi}` list, then legacy int index, then system default, then any input. Up to 4 devices per open (`MIC_OPEN_ATTEMPTS`); a device that fails is excluded for the meeting; when every device is excluded, clear the list once and retry. | capture_session.py:_open_mic:614-660, devices.py:find_mic_device | port |
| A25-05 | Capture watchdog | Every 1.0 s per channel: restart when source inactive, stalled (`requires_audio_blocks` sources only) or default device changed; at most one attempt per channel every 3.0 s; total restore budget <= 12 s (retry 3 + poll 1 + SoundCard start 4 = 8). Stops when meeting not recording. | capture_session.py:_watchdog_loop, _watch_channel, _channel_healthy, _device_changed | port |
| A25-06 | Default-device following | Mic follows the default only when no ranked list and no index (`mic_device_id is None and not mic_priority`). Loopback with int device id compares to the probed WASAPI index; SoundCard compares default sink id; ScreenCaptureKit always current. | capture_session.py:_device_changed:558-582 | port |
| A25-07 | Spool reuse across restarts | Restart keeps the channel `SpoolWriter` (`reuse_spool=True`), so pre-restart audio is stitched; a failed start tears down only a newly created spool. Signal monitor is kept, `reset_stream()` bumps its generation. | capture_session.py:start_source:317-395, restart_channel:669-713 | port |
| A25-08 | Capture status in state | `state.capture` dict (see Data) updated only when changed; emits status note. "System audio restored." when loopback comes back. Gap message joins with "; ": "Microphone unavailable", "Microphone is connected but no audio blocks are arriving; check the selected input device", "System audio disabled for this meeting by your microphone-only choice", "System audio unavailable", "System audio is connected but no audio blocks are arriving; check the output device". Integrity error is prefixed. A deliberate mic-only message still appends a new mic stall. | capture_session.py:update_status:726-774, _missing_parts, _gap_message | port |
| A25-09 | Audio integrity error | First storage failure only; logs, updates status, emits `error` `{code:"capture_incomplete", message: msg + " Check available audio in Meeting History and free storage or reduce system load before recording again."}`. Flush failure message: "Meeting audio storage could not finish. Partial audio was kept." | capture_session.py:_record_integrity_error, flush, check_spool_health | port |
| A25-10 | Block router | Audio-thread callback: observe health (signal only while recording and not paused), drop block when not recording or paused, `spool.feed`, then `asr.feed_preview(block, meeting_time(t_mono))`; never raises; first failure logged once. Recording happens before preview even if preview throws (test pinned). | capture_session.py:make_block_router:397-428 | port |
| A25-11 | FailureLog / where_it_failed | First failure per key at ERROR (+ "; repeats are logged at debug until it recovers"), repeats DEBUG until `recovered(key)`. `type_only` logs `"{Type} at file.py:120 in fn < ..."` without message (privacy). Also used by a24 engine. | capture_session.py:44-105 | port |
| A25-12 | Capture signal monitor | 0.25 s RMS windows, threshold 300 int16; two consecutive loud windows count one `signal_windows` pulse, rate-limited to one per 2.0 s; `receiving` = last block < 3.0 s ago; `stalled` = >= 3.0 s since last block, or >= 4.0 s since start with no block. Signal counter survives restarts; generation increments. Quiet never flagged as fault. | capture/health.py:CaptureSignalMonitor | port |
| A25-13 | WASAPI loopback discovery | First WASAPI input with "[Loopback]" whose name contains the default render device name, else first. Outcome logs only on change (watchdog polls each second). | capture/devices.py:find_loopback_device | other-platform only (Windows) |
| A25-14 | PortAudio capture source | int16, blocksize 1024, native rate and channels, mean downmix; timestamp = monotonic minus (currentTime - inputBufferAdcTime) only when both finite, current > 0 and 0 <= adc <= current (MME batched-buffer fix). Raises from `start()` on failure. `requires_audio_blocks=True`. | capture/sd_stream.py:SdCaptureSource | port |
| A25-15 | SoundCard loopback source | 48 kHz, 1024 frames/read, <=2 ch, float to int16 clip; `start()` waits <= 4.0 s for first block; inactive after 3.0 s stall or 4.0 s without first block; Linux uses sample-count clock (`_SampleClock`, window 2.0 s, resync > 0.15 s, jump >= 2.5 s); Windows stamps `now - n/48000`; COM init after import. | capture/soundcard_stream.py | port (Windows fallback, Linux main) |
| A25-16 | Linux system-audio probe | Never raises; returns `LinuxAudioCapability{ready, reason, server_kind, default_sink, monitor_source, package_family, remediation_key, detail}` with stable reason codes (see Data). Checks arch, libpulse, SoundCard import, server kind, default sink, monitor (exact id `<sink>.monitor`, sink-keyed, or pactl "Monitor of:" proof), optional 1.5 s open verify in daemon thread. Used by the a31 pre-start dialog. | capture/linux_audio.py:probe_linux_audio | other-platform only (Linux) |
| A25-17 | Linux monitor pinning and reconnect | Record stream with `PA_STREAM_DONT_MOVE|ADJUST_LATENCY` and 4800-frame (100 ms) fragments so unplugging the output cannot reroute "Others" to the mic; reconnect after server restart; skip `pa_operation_unref(NULL)` abort. | capture/linux_audio.py:_pinned_recorder_type, load_soundcard, _harden_backend | other-platform only (Linux); most of it disappears with native libpulse |
| A25-18 | macOS ScreenCaptureKit loopback | macOS 13+, Screen Recording preflight (never prompts), 2x2 dummy video at 1 fps discarded, 48 kHz stereo float to mono int16, excludes own process audio, PTS used as `t_mono`; 5 s content/start timeouts, 2 s stop; first-block grace 3.0 s, stall 3.0 s. | capture/sck_stream.py | other-platform only (macOS) |
| A25-19 | Spool writer: chunks | Native rate buffered, resampled once per chunk to 16 kHz. Cut after 5.0 s at first 0.4 s window with RMS < 300 (cut at window end), hard cut at 20.0 s; scan hop 0.05 s. Files `{channel}_{seq:05d}.wav`, written `.tmp` then replace. Remainder < 0.25 s dropped at flush. | capture/spool.py:SpoolWriter, _CutScanner, find_cut_point | port |
| A25-20 | Spool timeline repair | Gap > 0.12 s filled with zeros (in <= 1 s allocations); overlap > 0.12 s trimmed off the incoming block; rate change finalizes pending chunk, spills session PCM, keeps meeting-time cursor. First 5 fixes logged INFO, rest DEBUG. Paused blocks dropped. | spool.py:_process_block:931-975 | port |
| A25-21 | Crash-safe registration | Order pinned: WAV replace, write `<wav>.recovery.json` sidecar, `register_chunk(... asr_status='pending')` (one retry after 0.1 s), remove sidecar, then `on_chunk`. Two failures: rename to `.orphan`, rewrite sidecar, health error "{channel} audio could not be added to the meeting. Its file was kept for recovery." | spool.py:_emit_chunk, _register_chunk | port |
| A25-22 | Bounded spool queue | 512 blocks (~12 s); full queue drops block, health error "{channel} audio blocks were dropped because recording storage fell behind. This meeting's audio is incomplete."; flush joins writer <= 30 s. Other health strings at spool.py:806, 841, 869, 876, 884, 893, 1067, 1102, 1116, 1145, 1169, 1228. | spool.py:feed, flush, _writer_loop | port |
| A25-23 | Session PCM and WAV | `{ch}_session.pcm` native int16; meta JSON rewritten every 5 s of audio (fsync + replace); rate change resamples it into `{ch}_session.16k.pcm` in 30 s windows; end wraps 16k PCM into `{ch}_session.wav` and deletes the 16k PCM. Origin is immutable first-sample meeting time. | spool.py:_append_session, _spill_session_pcm, _finalize_session, write_session_meta | port |
| A25-24 | Session WAV fallback | When `{ch}_session.wav` missing or <= 44 bytes, concatenate registered chunks for the channel with silence-filled gaps (> 1 sample). | spool.py:resolve_session_wav, concat_channel_chunks_to_wav | port |
| A25-25 | Recovery helpers (called by a24) | `load_chunk_recovery_meta`, `pcm_slice_to_wav` (native PCM range to atomic 16 kHz WAV, 30 s windows, fsync via r+b reopen for Windows), `pcm16k_to_wav`, `load_session_meta`. | spool.py:311-571 | port |
| A25-26 | Meeting ASR worker | One daemon worker; in-memory queue <= 64 chunks; over that, `enqueue` returns False and the worker refills pages from SQLite (`get_pending_chunks(limit=free+1, exclude_ids)`), ordered by start_s, seq, channel, id. Backlog messages (A25 "What the user gets"). Refill retry 5 s after a failure. | asr/engine.py:enqueue, requeue_pending, _worker, _report_backlog | port |
| A25-27 | Chunk retry policy | (Correction: `PermanentChunkError` sets status `blocked`, not `failed`. A remote outage refunds the attempt it counted.) 3 attempts; backoff 0.5 s then 2.0 s; status `processing` / `failed` / `blocked` / `pending`. Terminal messages via `on_chunk_failure("Saved audio chunk {id}: {reason}")`: "Transcription failed after three attempts. Audio is saved; check the speech engine and retry this meeting.", "Speech engine unavailable. Select or install a model, then retry saved audio.", "An unexpected transcription error occurred. Audio is saved; retry this meeting." Unreadable WAV = `PermanentChunkError` "Saved audio cannot be read ({exc}); restore or replace the WAV, then retry" (no retries); bad samples "Saved audio has invalid sample data ({exc}); repair the WAV, then retry". | asr/engine.py:_process_chunk, _attempt_chunk, _retry_after_failure, _transcribe_chunk | port |
| A25-28 | Digital-silence skip | Peak abs <= 8 int16: no decode, chunk still committed with zero segments. | asr/engine.py:_is_digital_silence | port |
| A25-29 | Segment ids and timestamps | Live id `sg_` + sha256(`{meeting}:{chunk}:{ordinal}`)[:20]; offline id `sg_` + sha256(`{meeting}:offline:{channel}:{round(start_s*1000)}:{ordinal}`)[:20]; ordinal counts *all* decoder segments, including dropped ones. start/end = chunk.start_s + seg times. | asr/engine.py:_stable_segment_id, offline.py:offline_segment_id | port (ids are evidence anchors stored in DB) |
| A25-30 | Drain at End | `drain(timeout)` waits for queue + in-flight + refill; re-checks SQLite before claiming done. | asr/engine.py:drain | port |
| A25-31 | Whisper decode tuning | Beam 5, or 1 when >= 3 chunks wait ("fast decode"); `vad_filter=True`, `condition_on_previous_text=False`; 50-word per-channel draft prompt with term-correction vocabulary hint; queue wait > 15 s warning. | asr/engine.py:_beam_size_for_backlog, _draft_prompt, _remember_draft_segments | drop (engine-specific): Parakeet's `SpeechDecoder.transcribe` ignores all of these (keep the 15 s wait warning) |
| A25-32 | Language voting | With `auto`, offline pass reuses live language when >= 5 confident (p >= 0.8) votes and top share >= 0.9. | asr/engine.py:dominant_language, _record_language | drop (engine-specific): Parakeet path reports no `language_probability`, so votes never accrue; configured language still passes through |
| A25-33 | Hallucination filter | Drop when no_speech > 0.6 and avg_logprob < -1.0, or compression > 2.4, or stock phrase (13 phrases) with no_speech > 0.3 or logprob < -0.8. | asr/hallucination.py:is_hallucination | drop (engine-specific): Parakeet segments carry zero scores, so it never fires; keep the empty-text skip |
| A25-34 | Live caption preview (Parakeet window) | Per channel rolling <= 8 s float buffer; decode when >= 1.0 s new audio, oldest channel first; skip when durable ASR busy; skip near-silence (max abs <= 0.00025); next decode delayed `max(0.1, 3 x decode time)` (<= 25% duty); new window on media gap > 0.15 s or wall gap > 0.25 s; queue 128 blocks, overflow dropped; event `{channel, text, start_s=end-len/16000, end_s, final:false}`. | asr/preview.py:WindowSpeechPreview | port (adapt to Recognizer) |
| A25-35 | Streaming preview | Streaming-model preview with 0.75 s minimum send, 30 s max stream, cancel when busy and > 5 s buffered, finish on gap. | asr/preview.py:MeetingSpeechPreview | undecided backend (Nemotron Streaming) |
| A25-36 | Offline clean re-decode | Session WAV cut: target 15 s, max 25 s, quiet window 0.7 s, overlap 0; 50-word prompt from previous windows; any window failure aborts the whole pass ("Offline ASR failed for {channel} window {i}/{n} at {t:.1f}s"); progress "{Microphone|System Audio}: decoding window {i}/{n} ({a:.1f}s - {b:.1f}s)"; merged sorted by (start, channel, id). | asr/offline.py:transcribe_meeting_sessions, transcribe_session_audio, offline_cut_ranges | port |
| A25-37 | Mic echo removal | Drop mic segment of >= 3 words when >= 60% of its words occur in loopback segments overlapping it +/- 1.0 s. Runs only in the offline pass. | asr/offline.py:drop_mic_echo | port |
| A25-38 | Remote meeting backend | Meeting-owned connection to paired host; checks fingerprint, engine availability, meeting-capable model, saved family/model; disconnected host defers without spending attempts (backoff 5/10/20/40/60 s, busy host honours retry_after >= 0.5 s, <= 60 s). Strings at remote.py:35, 51, 89-90, 118, 122-123, 129, 142, 159 and engine.py:690-692. Saved route ignored when its fingerprint is this computer. | asr/remote.py, asr/engine.py:_check_remote_ready, _remote_deferral, _remote_retry_delay | undecided backend (remote engine) |
| A25-39 | Playback WAV | `GET /api/meetings/{id}/audio` builds `playback.wav` (16 kHz mono, chunks mixed, overlapping samples averaged, 10 s render blocks), cached by watermark `[id, size, mtime_ns]` per chunk in `playback.json`; per-meeting lock; chunk path must be directly inside the spool dir ("audio chunk path is outside its meeting spool"); missing: "meeting audio is unavailable" (404). | audio_playback.py:build_playback, _render_mono | port |
| A25-40 | Fbank front end | Kaldi 80-bin log-mel: 25 ms/10 ms snip-edges, DC removal, pre-emphasis 0.97, Povey window (hann^0.85), 512 FFT, mel 20 Hz to 8000 Hz (Nyquist, not 7600), log floor 1e-10, int16 scaling, CMN. | diarize/fbank.py | port |
| A25-41 | Speaker embedder | ONNX (WeSpeaker), CPU, 1 intra + 1 inter thread; optional length input (name contains "len"); output "embs" if present; < 0.8 s audio returns none; L2 normalized. | diarize/embedder.py:SpeakerEmbedder | port |
| A25-42 | Online diarizer | Cosine assign >= 0.62, EMA 0.15 (skipped when pin-dominated), <= 10 clusters, new "Speaker N" participant (`kind:"others_cluster"`, provisional) via store; too-short segment inherits nearest label within 10 s; re-cluster every 25 embeddings or 60 s with average-linkage stop at distance 0.38 over a bounded working set (400 recent + all pins, <= 600) plus per-participant anchors (weight <= 99); pins weigh 100 and are never relabelled; over-merged groups split when >= 3 members; relabel ops `reassign_segment_speaker`; degrade-once to channel labels. | diarize/clustering.py:OnlineDiarizer, create_diarizer | port |
| A25-43 | Segment label assignment helper | Slice loopback frames per segment, `prepare_for_whisper`, `assign(seg, audio, 16000)`, set `speaker_source="diarizer"`; then refresh labels after later re-clusters. Shared by live, offline and Past Meetings retry. | diarize/assign.py | port |
| A25-44 | OpenAI speaker identification | Post-meeting: loopback session to MP3 32 kbps, windows under 20 MiB with 2 s overlap, up to 4 known-speaker 2-10 s reference clips (human-named clusters first, then anchors from earlier windows), map turns to segments by max overlap (ties to earlier turn), resolve to participants by majority, create "Speaker N", apply reassigns in batches of 40. Result `{ok, applied, created, windows, error[, retired]}`; progress "Identifying speakers in window {i}/{n}"; errors "No system-audio recording is available.", "No system-audio transcript segments to label.", "System-audio recording is empty.". Model retires 2027-02-26. | diarize/cloud_pass.py, diarize/cloud_audio.py | undecided backend (cloud); recommend drop |

44 rows.

### Added by check

Behaviours the table above only implied or skipped (source: engine.py, capture_session.py, assign.py, audio_playback.py).

| ID | Feature | Exact behaviour | Source | Disposition |
|---|---|---|---|---|
| A25-45 | Capture flush at End | `MeetingCapture.flush()` order: stop the preview first (`asr.stop_preview`), then flush every spool in turn, passing each returned last chunk to `on_chunk`; a spool whose flush raises records the integrity error "Meeting audio storage could not finish. Partial audio was kept." and continues with the others; finally `check_spool_health()`. `stop()` stops the watchdog, then every source (errors logged, never raised); it does not flush. Integrity error is recorded once, so later ones are ignored. | capture_session.py:430-476 | port |
| A25-46 | Remote connection status strings | Reported through `on_connection_status(message, connected)`: "Transcribing on {host_name}" (connected) when the host is ready; `RemoteMeetingUnavailable` gives "{exc} Audio is saved locally; retrying the remote connection." (disconnected); a busy host (`retryable`) gives "{exc} Audio is saved locally; waiting for host capacity." (connected). The chunk goes back to `pending` with the error text. Remote errors from `remote.py`: "Pair a computer in Settings → Dictation → Remote engine first." / "This meeting's remote speech configuration is invalid." / "{host}: {status or 'speech engine is not ready'}." / "{host}'s engine does not support meetings. Select Whisper, Parakeet, Nemotron or Moonshine in Remote engine → Manage host models." / "Restore {model} on {host} to resume this meeting." / "Meeting transcription stopped." / "Cannot connect to {host}." / "Pair with {host} again to transcribe this meeting. Its audio will not be sent to a different computer." | asr/engine.py:669-693, asr/remote.py | undecided backend (remote engine) |
| A25-47 | Enqueue semantics | `enqueue` returns False when stopping; True without re-queuing when the chunk id is already queued; once any chunk has been deferred to SQLite (`_deferred_known`), later live chunks are also sent through SQLite (not queued directly) so order is kept, and `_refill_needed` is set. `enqueue` records the enqueue time (basis of the 15 s wait warning). | asr/engine.py:314-345 | port |
| A25-48 | Preview start and busy rule | `start_preview` is a no-op when a preview already exists, when the model is not in the local catalog (Whisper) or when it is neither streaming nor Parakeet. The busy predicate handed to the preview is `outstanding chunks > 0 or engine stopping`; language passed is the configured language or `"auto"`. `feed_preview` does nothing when no preview exists. `stop_preview` detaches then stops it. | asr/engine.py:289-318 | port (window preview only) |
| A25-49 | Offline pass guards | `transcribe_offline_session` returns `[]` (no error) when the backend is missing or unavailable or has no `model`; it passes `dominant_language()` (configured language, else vote result). Per channel: skipped when the spool dir is missing and no chunk rows, or when session audio is absent or empty (logged INFO). Windows with empty converted audio are skipped. Each window's earlier-window overlap tail is dropped (`drop_overlapped_prefix`, segments ending at or before the previous window's end) and, when a window keeps segments, earlier segments starting at or after this window's origin are removed. Echo removal then runs over all channels before the final sort. | asr/engine.py:405-433, asr/offline.py:111-130, 150-250, 357-412 | port |
| A25-50 | Diarizer label assignment details | `assign_from_frames(diarizer, segments, frames, rate, origin_s, on_unlabeled=None)`: slice indices `round((start - origin) * rate)` clamped to the buffer; segments with an empty slice are skipped silently (no `on_unlabeled`); a diarizer exception is logged per segment and treated as unlabeled; `on_unlabeled` is called for no label; returns the labelled segments in input order. `refresh_labels` re-reads `current_label(segment_id)` per segment, ignoring failures and empty labels. | diarize/assign.py:16-80 | port |
| A25-51 | Playback errors | `build_playback` raises `ValueError("unknown meeting")` for an unknown id, `FileNotFoundError("meeting audio is unavailable")` when the spool dir is unset/missing or no chunk rows exist. The cache is considered valid only when `playback.json`'s watermark equals the current one and `playback.wav` exists; both files are written as `.tmp` then replaced, temp files removed in a finally. Render length = max(start_s + duration_s) of all chunks; chunks that are not 16 kHz mono 16-bit are skipped. | audio_playback.py:47-125 | port |
| A25-52 | Digital silence and unreadable audio ordering | Per chunk: load WAV (missing/permission/wav error/non-16-bit -> `PermanentChunkError`: status `blocked` with the message as error and reported through `on_chunk_failure`, no retries), then silence check, then conversion (`ValueError`/`ZeroDivisionError` -> `PermanentChunkError`), then empty audio returns no segments. Segments with empty text are dropped before the hallucination check; the dropped ones still consume ordinals. | asr/engine.py:845-905 | port |

## Modules

### meeting/capture_session.py (811 lines)
- **Responsibility**: one meeting's capture: sources, per-channel spools and monitors, mic selection/failover, watchdog,
  `state.capture` reporting. Imports the capture layer lazily inside methods.
- **Public surface**: `where_it_failed(exc)`, `FailureLog(log).failed(key, msg, *args, type_only=False)` / `.recovered(*keys)`,
  `MeetingCapture(options, clock, repository, *, recording, asr, store, on_chunk, emit_status, emit)` with
  `start(meeting_id, spool_dir) -> note|None`, `stop()`, `flush()`, `check_spool_health()`, `update_status(msg="")`,
  `start_source(src, reuse_spool=False) -> bool`, `start_fallback_loopback(reuse_spool=False)`, `restart_channel(ch, desired=None)`,
  `make_block_router(ch, spool, monitor=None)`, `source(ch)`; attributes `sources`, `spools`, `stall_channels`,
  `mic_excluded`, `mic_device`, `integrity_error`.
- **Threading**: `start()` on the engine's start worker; block router on audio threads (PortAudio callback, SoundCard
  thread, SCK dispatch queue); watchdog daemon thread `meeting-capture-watchdog`; `_lock` is an RLock around
  `sources/spools/_monitors`; `FailureLog` has its own lock. `_stop_watchdog` joins <= 3 s and never joins itself.
- **Constants**: `CAPTURE_WATCHDOG_INTERVAL_S=1.0`, `CAPTURE_RETRY_INTERVAL_S=3.0`, `CAPTURE_RECOVERY_BUDGET_S=12.0`,
  `MIC_OPEN_ATTEMPTS=4`.
- **Edge cases to keep**: source verified with `is_active()` right after `start()`; an inactive just-started source is
  stopped and treated as unavailable (:369-385). Lost mic is excluded *before* reopening because the watchdog probe may
  have named the same dead device (:687-692). Loopback restart reuses the watchdog's probe to stay inside 12 s (:693-697).
  `update_status` compares the new capture dict to the stored one and only writes on change (:770-774). The
  "System audio restored." note fires on the rising edge only and clears the explicit mic-only message (:741-748).
  Watchdog exits as soon as `recording()` is False (:511).

### meeting/capture/devices.py (225)
- `find_loopback_device() -> {index, name, samplerate, channels}|None` (WASAPI only; under `portaudio_lock`);
  `find_mic_device(priority, exclude=()) -> {index, name, hostapi, samplerate, channels}|None`. Re-exports
  `services.audio_devices` helpers (`LOOPBACK_MARKER="[Loopback]"`, `device_key`, `list_input_devices`,
  `ranked_candidates(..., check=False)` because meetings open devices at native rate, `portaudio_lock`).
- `_log_on_change(key, level, msg)` demotes repeats to DEBUG (watchdog polls every second). Missing WASAPI is WARNING on
  Windows, DEBUG elsewhere. A bool priority is not an int index (:192). Invalid index warns
  "Preferred mic device index %s is not a valid input device; falling back to default".

### meeting/capture/health.py (111)
- `CaptureSignalMonitor(signal_rms=300.0)`: `reset_stream(now=None)`, `observe(block, signal_enabled=True, now=None)`,
  `snapshot(now=None) -> {receiving, stalled, signal_windows, generation}`.
- Constants: `SIGNAL_RMS_INT16=300.0`, `SIGNAL_WINDOW_S=0.25`, `MIN_CONSECUTIVE_SIGNAL_WINDOWS=2`,
  `SIGNAL_REPORT_INTERVAL_S=2.0`, `FIRST_BLOCK_GRACE_S=4.0`, `BLOCK_STALL_S=3.0`.
- Sum of squares computed outside the lock (audio callback); rate change resets the window; paused blocks still count
  as delivery but reset signal accumulation (:68-72).

### meeting/capture/sd_stream.py (180)
- `SdCaptureSource(channel, device_index, samplerate, n_channels)`: `start(on_block)` (raises), `stop()`, `is_active()`,
  `device_id` (int), `requires_audio_blocks=True`. `BLOCKSIZE=1024`. Callback copies the buffer (PortAudio reuses it),
  mean-downmixes via int32, logs first status once and <= 5 callback tracebacks; `finished_callback` marks inactive.
- ADC timestamp rule at :122-142 (see A25-14), pinned by tests/test_meeting_sd_stream.py.

### meeting/capture/soundcard_stream.py (409)
- `SoundcardLoopbackSource(selection=None)`: static `available()` (Linux: `probe_linux_audio(verify_open=False).ready`;
  else SoundCard importable), `start(on_block)` (waits <= 4 s on `_settled`), `stop()` (join 2 s), `is_active()`,
  `is_default_device_current()` (fail closed), `device_id` string (`"soundcard-default"` then sink/speaker id).
- `_SampleClock(rate).stamp(n, now)` (A25-15). Recorder thread `meeting-sc-loopback`. Failure tags
  `device_unavailable` / `recorder_failed` / `start_timeout`. `_check_linux_monitor` refuses non-loopback or identity
  mismatch ("selected Linux monitor is not loopback", "... identity mismatch").

### meeting/capture/sck_stream.py (368)
- `ScreenCaptureKitLoopbackSource()`: static `available()`, `start(on_block)` (never raises), `stop()`, `is_active()`,
  `is_default_device_current() -> True`, `device_id="screencapturekit-system-mix"`.
- `_decode_mono_int16(CMSampleBuffer)`: refuses non-32-bit, planar vs interleaved by flag `1<<5`, drops ragged tail.
- One ObjC delegate class built once; sink retained (SCK does not retain it). Late start is stopped (:301-311).

### meeting/capture/linux_audio.py (975)
- Probe and SoundCard plumbing (A25-16/17). Public: `LinuxAudioCapability`, `LinuxMonitorSelection{sink_id, sink_name,
  monitor_id, monitor_name, channels, server_kind, soundcard_id}`, `probe_linux_audio(*, verify_open=True, platform,
  machine, soundcard_module, libpulse_loader, open_timeout_s=1.5)`, `load_soundcard()`, `open_monitor_recorder()`,
  `default_sink_id()`, `resolve_linux_monitor()`, `detect_linux_audio_server()`, `soundcard_connection_alive()`.
- Server detection order: SoundCard connection's "server name" (contains "pipewire" -> `pipewire-pulse`), then `pactl
  info`, then `systemctl --user is-active pipewire|pipewire-pulse|pulseaudio` (2 s timeouts).
- Never fabricates `<sink>.monitor` without list evidence (:526-559). Unknown server + sink/monitor failures are
  re-classified as `pipewire_pulse_missing` or `audio_server_unavailable` (:955-975).

### meeting/capture/spool.py (1336)
- Pure helpers: `resample_to_16k`, `gap_fill_frame_count`, `gap_fill_frames`, `find_cut_point`, path helpers
  `session_pcm_path/session_meta_path/session_wav_path/chunk_recovery_meta_path`, sidecar
  `write/load/remove_chunk_recovery_meta`, `write/load_session_meta`, `resample_pcm_file_to_16k`, `pcm_slice_to_wav`,
  `pcm16k_to_wav`, `concat_channel_chunks_to_wav`, `resolve_session_wav`.
- `SpoolWriter(meeting_id, channel, spool_dir, clock, repository, on_chunk, target_sec=5.0, max_sec=20.0,
  quiet_window_s=0.4, queue_size=512, initial_seq=0)`: `feed(block)` (audio thread, never blocks),
  `flush(timeout_s=30) -> last SpooledChunk|None` (idempotent), `health_error`.
- Threading: bounded `queue.Queue`, writer daemon `meeting-spool-{channel}` polling 50 ms; sentinel on flush; writer
  finalizes remainder then session even after a crash in the loop.
- Constants: `TARGET_RATE=16000`, `GAP_TOLERANCE_S=0.12`, `QUIET_RMS=300`, `QUIET_WINDOW_S=0.4`, `MIN_FLUSH_S=0.25`,
  `_CUT_SCAN_HOP_S=0.05`, `QUEUE_MAX_BLOCKS=512`, `DEFAULT_TARGET_SEC=5.0`, `DEFAULT_MAX_SEC=20.0`,
  `FLUSH_TIMEOUT_S=30`, `_REGISTER_RETRY_DELAY_S=0.1`, `_MAX_GAP_LOGS=5`, `SESSION_RESAMPLE_WINDOW_S=30`,
  `_SESSION_META_EVERY_S=5`, `GAP_FILL_CHUNK_S=1.0`.
- Must keep: resample once per chunk, never per block (docstring :23-33, drift 4.7 s/h otherwise); chunk start from
  cumulative native counts so chunks are contiguous; `_CutScanner` incremental prefix sums equal to the one-shot
  `find_cut_point`; cut returns window *end*; `cut <= 0` guard; Windows fsync reopen r+b (:543-546).

### meeting/asr/audio.py (61)
- `WHISPER_SAMPLE_RATE=16000`; `prepare_for_whisper(int16, rate) -> float32 16 kHz in [-1,1]` (divide by 32768,
  mean channels, FFT resample, clip); `load_wav_int16(path) -> (mono int16, rate)` raising ValueError for non-16-bit.

### meeting/asr/engine.py (990)
- `MeetingAsrEngine(model_name, meeting_id, repository, language=None, term_rules=None, remote=None,
  on_connection_status=None, *, defer_load=False, on_backlog_status=None, on_chunk_failure=None)`; `load_backend()`,
  `start(on_chunk_result)`, `start_preview(cb)`, `feed_preview(block, start_s)`, `stop_preview()`, `enqueue(chunk)`,
  `drain(timeout_s)`, `transcribe_offline_session(spool_dir, chunks=None, *, progress_cb=None)`,
  `dominant_language()`, `stop()`, `requeue_pending(*, rows=None)`; attrs `is_available`, `last_error`, `language`.
  `PermanentChunkError`.
- `on_chunk_result(chunk, segments)` is the **durability boundary**: it must store segments and mark the chunk done or
  raise; a raise retries with the same prompt.
- Threading: worker `meeting-asr`; `_idle_cond` Condition guards outstanding/queued ids/refill flags; `_backend_lock`
  RLock for load vs stop; `_stop_event` cancels deferred load and waits. `stop()` joins <= 10 s and leaves the backend
  alive if a native decode is still running (:483-488).
- State: `_outstanding` stays > 0 through the post-chunk refill so `drain()` cannot succeed between dequeue and refill
  (:625-633); `_deferred_known` forces live chunks through SQLite once any were deferred, preserving order (:327-331).
- Backend selection (:236-246): remote -> `MeetingRemoteBackend`; catalog model (Parakeet, Moonshine, Qwen, Nemotron,
  MLX) -> `LocalSpeechBackend` (worker process); otherwise faster-whisper `LocalWhisperBackend`.
- Preview selection (:289-304): only for `LocalSpeechBackend`; `MeetingSpeechPreview` when model.streaming, else
  `WindowSpeechPreview` for `parakeet` / `parakeet_mlx`; Whisper gets none.

### meeting/asr/hallucination.py (76) - see A25-33. Stock phrase set at :28-42; normalization lowercases, replaces
`[^\w\s]+` with space, collapses spaces.

### meeting/asr/offline.py (466)
- `offline_cut_ranges`, `drop_overlapped_prefix`, `offline_segment_id`, `session_origin_s`, `transcribe_session_audio`,
  `load_channel_session(spool_dir, ch, chunks) -> (frames|None, rate, origin)` (origin from session meta, else min
  chunk start), `transcribe_meeting_sessions(..., transcribe_fn=None, progress_cb=None)`, `drop_mic_echo`,
  `OfflineWindowError`. Constants `OFFLINE_TARGET_SEC=15`, `OFFLINE_MAX_SEC=25`, `OFFLINE_QUIET_WINDOW_S=0.7`,
  `OFFLINE_OVERLAP_S=0.0`, `ECHO_TIME_SLACK_S=1.0`, `ECHO_WORD_OVERLAP=0.6`, `ECHO_MIN_WORDS=3`.
- The decoder iterator is fully consumed inside the try (:289-293) so a mid-window failure invalidates the pass.

### meeting/asr/preview.py (185) - A25-34/35. Threads `meeting-window-preview` (stop join 2 s) and
`meeting-speech-preview` (join 35 s). Feed copies frames (`dataclasses.replace`) so the spool's buffer is untouched.
`WindowSpeechPreview._run` drains up to 128 queued blocks before each decode so it never replays stale windows.

### meeting/asr/remote.py (168) - A25-38. `remote_route(settings)`, `saved_remote_route(meeting)` (reads
`asr_remote_json`, requires fingerprint/family/model strings), `MeetingRemoteBackend(route)`, exceptions
`RemoteMeetingUnavailable`, `RemoteMeetingBusy(retryable, retry_after_s)`.

### meeting/audio_playback.py (125) - A25-39. `PLAYBACK_RATE=16000`, `BLOCK_SAMPLES=160000`. Skips chunks that are
not 16 kHz mono 16-bit. Average only where more than one contributor (`mask = contributors > 1`).

### meeting/diarize/fbank.py (123), embedder.py (101), clustering.py (774), assign.py (69)
- See A25-40..43. `create_diarizer(model_path, store, repository, meeting_id) -> OnlineDiarizer|None` (missing path or
  load failure -> None, meeting keeps channel labels).
- `OnlineDiarizer`: `assign(segment, audio, rate) -> pid|None` (ASR worker thread, never raises; sets
  `segment.embedding = float32 bytes` for the commit to persist), `pin(segment_id, pid)` (web threads),
  `set_relabel_callback(cb)`, `current_label(id)`, `is_available()` (latches false).
- Locking rule: `_lock` never held across `store.apply` or the relabel callback (clustering.py:23-30); re-cluster in
  three phases (plan under lock, create split participants without lock, apply under lock re-validating pins);
  `_filter_stale_ops` re-checks pins just before dispatch; the current segment's own relabel travels as the return
  value, not as an op (:486-496).
- `_average_linkage`: Lance-Williams average update, masked +inf, tie on `argmin` flat index (row-major first).
  Dominant participant tie-break `max((weight, pid))` (:780); contender order `(-weight, -len, first id)` (:798-800).

### meeting/diarize/cloud_audio.py (196), cloud_pass.py (682) - A25-44. `plan_windows` (one window when estimate
`seconds*bitrate/8*1.05+1024` fits 20 MiB; else max(10 s, budget) windows, overlap clamped to 1/8 if too large),
`clip_sample_range` (expand to 2 s centred, clamp at edges; long keeps first 10 s), `encode_mp3` (PyAV libmp3lame),
`cut_reference_clip -> "data:audio/mpeg;base64,..."`, `map_turns_to_segments`, `resolve_speaker_participants`,
`speaker_slug` (`[^A-Za-z0-9]+` -> `_`, <= 32 chars, default "speaker"), `pick_reference_segments`,
`parse_diarized_result`, `default_transcribe` (OpenAI `audio.transcriptions.create`, `response_format="diarized_json"`,
`chunking_strategy="auto"`, file name `loopback.mp3`), `run_cloud_speaker_pass(...)`. Window-relative turns shifted to
meeting time unless already >= origin (:461-463).

## Data and formats

### Settings (read by a04 `services/runtime/meeting.py:_build_options`, resolvers in a10 `services/settings.py`)
| Key | Type | Default | Use here |
|---|---|---|---|
| `audio_input_priority` | list of `{name, hostapi}` | empty (system default) | `options.mic_priority` -> `find_mic_device` |
| `audio_input_device` | int (legacy) | none | migrated into priority by a05 `load_priority` |
| `meeting_speech_separate` | bool | false | false = meetings use the dictation engine |
| `meeting_asr_model` | str | `config.MEETING_ASR_MODEL` ('' or `parakeet-v3-mlx` on MLX Macs) | `options.asr_model` (catalog model id) |
| `meeting_whisper_model` | str | `"auto"` | Whisper meeting model (drop) |
| `meeting_asr_source` | `local`/`remote` | `local` | remote route (undecided) |
| `meeting_language` | `auto` or ISO code | `"auto"` | `MeetingAsrEngine(language=)` |
| `meeting_speaker_id_backend` | `off`/`local`/`openai` | `"local"` | diarizer on/off, cloud pass; `openai` resolves to `local` after retirement |
| `meeting_audio_upload_consent_given` | bool | false | gates the cloud pass (a24) |
| `meeting_end_redecode` | bool | false | offline re-decode at End (a24) |
| per-session `system_audio_policy` | `auto`/`required`/`disabled` | `auto` | never persisted |
| env `OPENWHISPER_SPEAKER_MODEL` | path | - | speaker ONNX override (a14 `speaker_model_path`) |

`MeetingEngineOptions` fields consumed here: `system_audio_policy`, `mic_priority`, `mic_device_id`, `asr_model`,
`asr_language`, `asr_remote`, `diarization_model_path`, `speaker_id_backend`.

### Database (owned by a28; columns this area writes or reads)
- `meeting_audio_chunks(id PK, meeting_id, channel 'mic'|'loopback', seq, file_path, start_s, duration_s, sample_rate,
  asr_status 'pending'|'processing'|'done'|'failed'|'blocked', asr_attempts, asr_error)`, unique
  `(meeting_id, channel, seq)`.
- `meeting_segments(id 'sg_...', meeting_id, chunk_id NULL for offline, channel, start_s, end_s, text,
  speaker_participant_id, speaker_source 'channel'|'diarizer'|'human', speaker_pinned, embedding float32 BLOB, created_at)`.
- `meeting_sessions.spool_dir`, `meeting_sessions.asr_model`, `meeting_sessions.asr_remote_json`
  (`{fingerprint, host_name, family, model, language}`).
- Repository calls used: `register_chunk(meeting_id, channel, seq, file_path, start_s, duration_s, sample_rate,
  asr_status)`, `next_chunk_seq(meeting_id, channel)`, `get_pending_chunks(meeting_id, limit, exclude_ids)`,
  `set_chunk_status(id, status, error=)`, `defer_chunk_after_connection_failure(id, error)`,
  `get_segments(meeting_id, after_start_s=-1.0)`, `get_audio_chunks(meeting_id)`, `get_meeting(id)`;
  engine-side `commit_chunk_transcription` (a24/a28).

### State store fields (a28 schema)
- `state.capture = {mic_available, loopback_available, mic_receiving, loopback_receiving, mic_signal_windows,
  loopback_signal_windows, mic_source_generation, loopback_source_generation, message, audio_incomplete,
  integrity_error}` via `store.update_runtime_fields(capture=...)`.
- Store ops emitted: `upsert_participant {display_name:"Speaker N", kind:"others_cluster", is_provisional:true}` and
  `reassign_segment_speaker {segment_id, participant_id}`, actor `("system", "diarizer")`.

### Files on disk (spool dir = `<MEETINGS_FOLDER>/<meeting_id[:12]>`, created by a24)
| File | Format |
|---|---|
| `{ch}_{seq:05d}.wav` (+ `.tmp`, `.orphan`) | 16 kHz mono int16 WAV |
| `{ch}_{seq:05d}.wav.recovery.json` (+ `.tmp`) | `{"version":1,"meeting_id","channel","seq","start_s","duration_s","sample_rate"}` compact JSON |
| `{ch}_session.pcm` | native-rate little-endian int16, headerless |
| `{ch}_session.16k.pcm` | 16 kHz int16 prefix after rate changes; deleted at end |
| `{ch}_session.json` (+ `.tmp`) | `{"sample_rate","sample_count","origin_s","pcm_origin_s","prefix_16k_samples","pcm_active"}` |
| `{ch}_session.wav` (+ `.tmp`) | 16 kHz mono WAV, valid when size > 44 |
| `playback.wav`, `playback.json` (+ `.tmp`) | mixed 16 kHz WAV; `{"watermark":[[chunk_id,size,mtime_ns],...]}` |

Existing installs depend on all of these names and JSON keys (recovery of meetings recorded by the Python app).

### Wire / protocol
- HTTP `GET /api/meetings/{meeting_id}/audio?token=` -> `audio/wav`, inline, `Cache-Control: no-store` (a30 route).
- Dashboard broadcast `{"type":"speech_preview", channel, text, start_s, end_s, final}` (engine forwards previews).
- Engine events: `error {code:"capture_incomplete"|"asr_unavailable"|"asr_chunk_failed", message}`.
- OpenAI `POST /v1/audio/transcriptions` multipart: model `gpt-4o-transcribe-diarize`, file mp3, `response_format=
  diarized_json`, `chunking_strategy=auto`, optional `known_speaker_names[]` + `known_speaker_references[]` data URLs.
- Linux remediation keys (consumed by a31 dialog): `ready`, `soundcard_missing`, `libpulse_missing`,
  `audio_server_unavailable`, `pipewire_pulse_missing`, `default_sink_missing`, `pactl_missing`,
  `monitor_source_missing`, `monitor_open_failed`, `unsupported_architecture`, `unknown_failure`; server kinds
  `pulse`, `pipewire-pulse`, `unavailable`, `unknown`.

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| numpy | all DSP, matrices | `std::vector`, hand loops; Eigen optional for clustering |
| `services.streaming_transcriber.fft_resample` (numpy rfft) | every resample | pocketfft (header-only, the FFT numpy uses) to reproduce bit-near output; or libsamplerate/soxr if exact parity is waived |
| sounddevice / PortAudio | mic capture, WASAPI loopback devices | PortAudio (vcpkg `portaudio`) keeps host-API names and device keys identical; miniaudio is the lighter alternative (native WASAPI loopback) but changes `hostapi` strings |
| soundcard (cffi to WASAPI / libpulse) | loopback fallback (Windows), monitor capture (Linux) | Windows: WASAPI `IAudioClient` with `AUDCLNT_STREAMFLAGS_LOOPBACK`; Linux: libpulse async API directly (`pa_stream_connect_record` with `PA_STREAM_DONT_MOVE|PA_STREAM_ADJUST_LATENCY`, fragsize 4800 frames) |
| PyObjC ScreenCaptureKit / CoreMedia / Quartz | macOS system audio | Objective-C++ `.mm` against ScreenCaptureKit, `CGPreflightScreenCaptureAccess` |
| ctypes `CoInitializeEx` | COM per thread | `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` |
| subprocess `pactl`, `systemctl --user` | Linux diagnosis | `QProcess` with 2 s timeout, or libpulse `pa_context_get_server_info` / sd-bus |
| wave | WAV read/write | existing `fv::read_wav` plus a small int16 WAV writer |
| hashlib sha256 | segment ids | small SHA-256 (e.g. picosha2) or `QCryptographicHash` in the Qt layer |
| json | sidecars | nlohmann-json or `QJsonDocument` |
| threading / queue | workers, bounded queues | `std::thread`, `std::condition_variable`; lock-free SPSC ring for the audio callback |
| onnxruntime | speaker embeddings | ONNX Runtime C++ API (vcpkg `onnxruntime`), CPU EP, 1 thread |
| PyAV (libmp3lame) | MP3 for OpenAI upload | LAME (vcpkg `mp3lame`) only if the cloud pass is kept |
| openai SDK | diarize call | `QNetworkAccessManager` multipart, only if kept |
| faster-whisper / CTranslate2 | Whisper decode | drop |
| `transcriber.optional_backend.LocalSpeechBackend` / `SpeechDecoder` | Parakeet via worker process | `fv::Recognizer::transcribe` in process |

## Platform-specific code

- **Windows**: mic and primary loopback via PortAudio WASAPI `[Loopback]` input devices (sounddevice >= 0.5 build);
  SoundCard fallback with COM init after import; MME timestamp quirk (currentTime 0); fsync of a read-only fd fails, so
  `pcm_slice_to_wav` reopens `r+b`. Missing WASAPI logs WARNING only here.
- **macOS**: ScreenCaptureKit only (13+, Screen Recording permission preflight; the actual prompt belongs to the meeting
  start flow in a31); PTS shares `time.monotonic()` timebase (mach); excludes own process audio. In C++ the
  `t_mono` source must be the same clock as the mic stamps: use `mach_absolute_time`-based steady clock on both.
- **Linux (X11/Wayland/Hyprland alike; audio-server dependent only)**: PulseAudio or PipeWire-Pulse monitor of the
  default sink; architecture gate via `meeting.platform.normalize_linux_machine`; libpulse presence check
  (`libpulse.so.0`, `libpulse.so`, `pulse`); package-family hint from a16 `services.linux_deps`; sample-count clock
  because Pulse delivers monitor audio in bursts; pinned stream so unplug does not reroute to the mic; reconnect after
  server restart. No display-server differences in this area.

## Engine-specific code

**Drop**
- faster-whisper backend construction and loading (`LocalWhisperBackend`, `_load_model(cancel_event)`), engine.py:236-256.
- Beam-size fast mode, `vad_filter`, `condition_on_previous_text`, draft prompt context and term-rule vocabulary hint
  (engine.py:805-817, 912-974; offline.py:197-246 prompt words). Parakeet ignores them (`SpeechDecoder.transcribe`
  takes `**_whisper_options` and discards them). Note: `term_rules` would then be unused in this area.
- Language voting (engine.py:439-464) and the hallucination score rules (hallucination.py): Parakeet segments have
  `avg_logprob=0, no_speech_prob=0`, no compression ratio and no `language_probability`, so neither ever acts.
- `LocalSpeechBackend` worker-process plumbing and `MODELS` catalog dispatch for Moonshine, Qwen3-ASR, Parakeet MLX.
- `prepare_for_whisper` naming only; the conversion (int16 to float, resample to 16 kHz) is still needed.

**Adapt to in-process Parakeet**
- `_transcribe_chunk` becomes: read WAV, digital-silence check, `Recognizer::transcribe(samples, 16000, {language})`,
  map `Transcript.segments` (seconds from chunk start) to meeting time, skip empty text. Ordinal counting must stay
  identical for stable ids.
- `MeetingAsrEngine.is_available`/`load_backend`/`stop` map to Recognizer construction on a meeting worker. CLAUDE.md
  requires recognizers be used from one thread at a time and reached through `EngineWorker`; the meeting worker and
  the live preview both need the model, and the preview must yield to durable decoding (`preview_audio` uses a
  non-blocking try-lock on the decode lock plus a `busy()` recheck). Port that as: one decode mutex per Recognizer,
  preview uses `try_lock`.
- `WindowSpeechPreview` calls `backend.preview_audio(samples, language, busy=)` -> `{text}`; port as a try-locked
  `Recognizer::transcribe` on <= 8 s.
- Offline pass: `transcribe_offline_session` uses `backend.model`; replace with a window decoder over `Recognizer`.
- Stop semantics: Python leaves a backend alive if a native decode outlives the 10 s join; FioriVoce never unloads the
  runtime anyway, but the Recognizer object must not be destroyed mid-decode.

**Undecided**: Nemotron streaming preview (`MeetingSpeechPreview`, `stream_audio`/`cancel_stream`), remote host
(`asr/remote.py`, remote branches of `engine.py`), OpenAI cloud speaker pass (`cloud_pass.py`, `cloud_audio.py`).

## Dependencies on other areas

**Calls out**
- a24 meeting engine: `meeting.interfaces` (`CaptureBlock`, `SpooledChunk`, `TranscriptSegment`, `CHANNEL_MIC`,
  `CHANNEL_LOOPBACK`, `CHANNELS`), `meeting.clock.MeetingClock` (`is_paused`, `meeting_time(t_mono)`),
  `meeting.platform.normalize_linux_machine`, `MeetingEngineOptions`.
- a28: `MeetingRepository` methods above; state store `apply(actor, source, ops) -> [OpResult{ok, reason, target_id,
  effect}]`, `with_state(fn)`, `update_runtime_fields(**)`; `meeting.corrections.correct_text`, `vocabulary_hint`
  (Whisper prompt only).
- a05: `services.audio_devices` (device listing, ranking, `portaudio_lock`, `LOOPBACK_MARKER`, `device_key`),
  `services.streaming_transcriber.fft_resample`.
- a14: `transcriber.local_backend.LocalWhisperBackend`, `transcriber.optional_backend.LocalSpeechBackend` /
  `SpeechDecoder`, `services.local_asr.catalog.MODELS` (`.backend`, `.streaming`, `.meeting`),
  `transcriber.remote_backend.RemoteSpeechBackend`, `services.openai_retirement` (`is_model_gone_error`,
  `SPEAKER_MODEL_RETIRED_MESSAGE`), speaker model path (`services.components.speaker_model_path`).
- a17: `services.remote_asr.settings.load_client_pairing`, `.client.RemoteHostBusy`, `.tls.own_fingerprint`,
  `.service._default_identity_dir`.
- a16: `services.linux_deps.detect_package_family`.
- `config.WHISPER_MODEL_CHOICES` (a01).

**Called by**
- a24 `meeting/engine.py`: `MeetingCapture` (constructed :210), `FailureLog`, `where_it_failed`,
  `MeetingAsrEngine` (:1503), `start_preview` (:1586), `drain` (:874), `transcribe_offline_session`,
  `create_diarizer` (:1771), `assign_from_frames` / `refresh_labels` (:1714-1718), cloud pass (:1075).
- a24 `meeting/recovery.py`: `load_chunk_recovery_meta`, `load_session_meta`, `session_meta_path`, `pcm_slice_to_wav`,
  `saved_remote_route`, `MeetingAsrEngine(...).drain`, `requeue_pending`.
- a24 `meeting/refinalize.py`: `load_channel_session`, `assign_from_frames`, `refresh_labels`, `create_diarizer`,
  `saved_remote_route`, `MeetingRemoteBackend`, `run_cloud_speaker_pass`.
- a30 `meeting/web/api.py:749-764`: `build_playback`.
- a31 `services/meeting_setup.py:159-200` (device check uses `find_mic_device`, `find_loopback_device`) and
  `ui_qt/dialogs/meeting_linux_audio_dialog.py` (`probe_linux_audio(verify_open=True)`).
- a11 `ui_qt/dialogs/settings_models.py:868` (`MeetingRemoteBackend(remote_route(settings))` test connection).
- benchmarks/meeting_mode (not ported).

## Tests

| Test file | Pins |
|---|---|
| tests/test_meeting_spool.py (25) | gap fill tolerance and counts; cut before target / quiet cut at 400 ms / hard cut at max; native-rate cut equals 16k cut; no drift over 60 s at 44.1 kHz; negative gap trims, positive gap zero-fills; paused blocks dropped; registered pending before `on_chunk`; short remainder kept; idempotent flush; registration failure orphans the WAV; session WAV written; pause creates no session files; session matches timeline; chunk concat fallback; rate change keeps origin and gap; bounded gap allocations; queue overflow and write failure set health error |
| tests/test_meeting_sd_stream.py (3) | ADC timestamp acceptance rules; MME batched callbacks produce a session WAV byte-identical to `resample_to_16k(all samples)` with zero gap logs |
| tests/test_meeting_capture_health.py (6) | signal thresholds, click vs continuous, rate limiting, generation and counter across restarts, paused counting |
| tests/test_meeting_capture_devices.py (15) | default-device index reading (list/tuple/pair), preferred index fallback, skip loopback-named inputs, log-once-per-outage |
| tests/test_meeting_failure_logging.py (8) | FailureLog first-error/repeat-debug/recovery, type-only formatting, probe warns once per channel |
| tests/test_meeting_soundcard_stream.py (1) | SoundCard import before COM init |
| tests/test_meeting_linux_capture.py (48) | every probe reason, monitor identity rules, pactl evidence, timeout daemon threads, reconnect and NULL-unref hardening, pinned recorder, fragment size, `_SampleClock` bursts / backlog / lost frames / long dropout |
| tests/test_meeting_macos_capture.py (25) | SCK decode (planar, interleaved, full scale, ragged, non-float, empty), availability gates, `is_active` grace/stall, fallback order in `start_fallback_loopback` |
| tests/test_meeting_asr.py (25) | meeting timestamps; bounded draft context and recovery hydration (Whisper, drop); digital silence commits; fast decode; 3 attempts; spacing; missing WAV blocked; remote refund / busy; bounded queue refill; stop keeps deferred chunks; refill not hot-looping; language vote; offline cuts, overlap keep, stable ids, failed window aborts; `interval_iou` (a28) |
| tests/test_meeting_hallucination.py (5) | non-speech dropped, real speech kept, mic echo drop, same words at another time kept, scoreless backends never filtered |
| tests/test_meeting_voice_preview.py (7) | 1 s cadence and 8 s window, busy skip keeps latest window, gap/pause reset, overflow copy safety, try-lock preview, record-before-preview order |
| tests/test_meeting_diarize.py (36) | AHC reference, pins never relabelled, stale-op filter, own-segment relabel via return, refresh labels, pin edge cases, degrade once, availability latch, bounded working set speed, 4-speaker fixture quality, split over-merge, convergence, split outside lock, fbank shape/determinism/int16/CMN/Nyquist/7800 Hz tone, embedding parity (needs env `OPENWHISPER_SPEAKER_MODEL` + `OPENWHISPER_SPEAKER_PARITY_REF` .npz, cosine >= 0.999) |
| tests/test_meeting_cloud_speakers.py (27) | turn mapping, participant resolution, slugs, window planning, clip ranges, MP3 encode, budget, injectable pass, consent gate |
| tests/test_meeting_remote.py (16) | remote route lifecycle (undecided) |
| tests/test_meeting_engine.py (92, a24) | capture-related: zero sources fail, mic-only start, required policy, disabled skips watchdog, stall restart, quiet capture, mic-stall message, default loopback change restarts only that channel, loopback restart preserves session audio, recovery budget <= 12 s, diarization availability and pins |
| also touch this area | test_meeting_recovery.py, test_meeting_refinalize.py, test_meeting_runtime.py, test_meeting_setup.py, test_meeting_linux_audio_dialog.py, test_openai_retirement.py, test_personalize_s7_audio_devices.py, test_meeting_data_lifecycle.py |

**Golden-vector candidates** (pure, deterministic; generate inputs/outputs from Python and replay in C++):
`fft_resample` / `resample_to_16k` (including `resample_pcm_file_to_16k` 30 s windows); `_CutScanner` /
`find_cut_point` and `gap_fill_frame_count`; whole `SpoolWriter` runs (block list with timestamps and rates -> chunk
table + session WAV + session JSON); `offline_cut_ranges` + `drop_mic_echo`; `compute_fbank` + `apply_cmn` (float
tolerance ~1e-4); `_average_linkage` and an `OnlineDiarizer` replay with fixed embeddings (labels and ops);
`CaptureSignalMonitor` and `_SampleClock` timed traces; `_render_mono` playback mix; `plan_windows`,
`clip_sample_range`, `map_turns_to_segments`, `resolve_speaker_participants` if the cloud pass is kept; segment id
hashing.

## Porting notes

**Decomposition**
- Qt-free core (`src/meeting/...`, namespace `fv`, no Qt, like `src/engine`):
  `audio_dsp` (fft_resample via pocketfft, int16/float conversion, WAV writer), `capture_types` (CaptureBlock,
  SpooledChunk, TranscriptSegment, channel constants), `spool` (CutScanner, SpoolWriter with SPSC ring + writer thread,
  session PCM/meta, recovery sidecars, fallback concat), `capture_health` (signal monitor, SampleClock), `capture_session`
  (MeetingCapture + watchdog, FailureLog) behind an abstract `CaptureSource` interface, `meeting_asr` (worker, queue,
  refill, retries, offline pass, echo drop, window preview) over an abstract `SpeechDecoder` with a Parakeet
  implementation, `diarize` (fbank, ONNX embedder, OnlineDiarizer, assign), `playback`.
- Platform sources as separate translation units: `capture_portaudio.cpp` (mic + WASAPI loopback devices),
  `capture_wasapi_loopback.cpp` (Windows fallback), `capture_pulse.cpp` + `linux_audio_probe.cpp` (Linux),
  `capture_sck.mm` (macOS).
- The repository and state store are interfaces supplied by a28; the core takes them as abstract classes.
- Qt layer: none needed here beyond wiring signals in a24/a31; previews and status reach the UI through the engine.

**Order of work**
1. DSP + WAV + resampler with golden vectors. 2. Spool writer + recovery files (golden SpoolWriter runs, byte-compare
session WAV). 3. Capture health + PortAudio source + MeetingCapture/watchdog on Windows (mic + WASAPI loopback).
4. Meeting ASR worker on Parakeet + commit callback + drain. 5. Window preview. 6. Offline re-decode + echo drop.
7. Diarization (fbank parity, ONNX, clustering replay). 8. Playback. 9. Linux Pulse source + probe. 10. macOS SCK.
11. Undecided: remote, streaming preview, cloud pass.

**Risks**
- Device-key compatibility: saved `audio_input_priority` entries are `{name, hostapi}` as PortAudio names them
  ("Windows WASAPI", "MME", ...). Switching to miniaudio or raw WASAPI breaks existing rankings unless mapped.
- Resampler parity: any non-FFT resampler changes chunk samples and therefore hashes nothing but does change ASR text
  and the byte-identical session test; decide whether exact parity is required.
- Audio-thread discipline: Python's GIL hides races; the C++ `feed()` must be allocation-free or bounded (ring buffer of
  fixed blocks), never lock against the writer.
- Shared Parakeet model between durable decode, preview and dictation: memory (two Recognizers) vs contention.
- ScreenCaptureKit and Pulse pinning are platform code nobody can test on Windows CI.
- OpenAI diarize model retires 2027-02-26 (about 3.5 months after today); porting the cloud pass plus LAME/multipart
  for that window is poor value.

**Rough C++ size**: core ~4,300 lines (spool 1,000, capture session 650, health/clock 200, ASR worker 650, offline +
preview 450, DSP/WAV 250, diarize 950, playback 150), platform sources ~1,400 (PortAudio 300, WASAPI loopback 250,
Pulse + probe 550, SCK 300), undecided ~800 (remote 200, streaming preview 150, cloud pass 450). About 5,700 without
undecided parts, 6,500 with them; tests another ~2,500.

**Native improvements**
- Lock-free SPSC ring from the audio callback; no per-block numpy allocation; writer does resample + WAV write without
  GIL contention, so the 512-block queue can shrink.
- In-process Parakeet removes the worker-process IPC per chunk and per preview; preview can share decoded encoder state
  only if NeMo-Speech.cpp later exposes it.
- Native libpulse removes the SoundCard hacks (NULL-unref patching, traceback walking to free a failed mainloop,
  `sys.modules` reloading) and the `pactl`/`systemctl` subprocesses for the common case.
- Average-linkage clustering in C++ is ~50-100x faster, which could relax `RECLUSTER_WINDOW` (but keep it for parity).
- Playback could be streamed (HTTP range over a virtual mix) instead of materializing `playback.wav`.
