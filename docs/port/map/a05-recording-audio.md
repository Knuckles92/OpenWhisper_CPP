# Recording and audio: recorder, devices, processing, live preview, dictation pipeline, stats

Area a05. Source checkout: `D:\coding\whisper_local` (paths below are relative to it).
Files: `services/recorder.py` (1135 lines), `services/audio_devices.py` (491), `services/audio_player.py` (277),
`services/audio_processor.py` (495), `services/dictation_language.py` (211), `services/dictation_pipeline.py` (597),
`services/dictation_stats.py` (694), `services/incremental_dictation.py` (480), `services/recording_journal.py` (340),
`services/streaming_transcriber.py` (758), `services/wav_metadata.py` (208), `ui_qt/widgets/capture_delay.py` (242),
`ui_qt/widgets/microphone_check.py` (163). All read in full.

## What the user gets

- Press a hotkey or button and dictation records from a ranked list of microphones at 44.1 kHz mono int16. If the
  preferred mic will not open, the next one is tried, ending at the system default (and on Windows the MME Sound Mapper).
- If a mic is unplugged mid-recording, the same recording continues on the next mic with 250 ms of silence between, and
  the overlay is told "old -> new" mic names. If no mic is left: "The microphone stopped and no other microphone could be
  opened; recording stopped."
- After the stop press, capture keeps running (post-roll) until the tail goes quiet (adaptive, typically ~300 ms) or
  1.2 s at most, so a last word is not clipped. 500 ms of silence is appended to the saved WAV.
- Pause/resume of a recording (walkthroughs use it): paused audio is not in the file at all.
- Cancel throws the capture away; a stop pressed during the cancel's post-roll saves nothing.
- Crash safety: captured PCM goes to a journal on disk as it is recorded; after a crash or kill the next start converts
  abandoned journals into `recordings/recording_<stamp>-recovered-<id>.wav`.
- Saved recordings carry Windows Explorer "Media created" metadata (LIST INFO ICRD/ISFT and a Broadcast Wave `bext`).
- Live level meter (smoothed RMS 0..1) feeding the waveform overlay.
- Live preview text while dictating (window re-decode every 3 s with 0.75 s overlap, or native engine streaming).
- Long dictations (> 30 s) are decoded window by window while still recording, so stop only waits for the last window.
- History playback of saved recordings (one at a time, stopped when recording or a meeting starts).
- Audio file preview (name, size, duration, rate, channels, chunk estimate) and, for the OpenAI API, silence-aware
  splitting into <= 23 MB WAV chunks with 2 s overlap.
- Dictation languages: a user list of languages, one active; a hotkey cycles them; the overlay shows a chip ("EN").
- The dictation pipeline: per-job focus capture, dictionary replacements, snippets, cleanup-prompt composition, caret
  joining, paste-target safety check, history context columns, stats counting.
- Stats window data: words dictated, pace (WPM), time saved, day streak, longest streak, activity map (371 days),
  per-period bars, hour/weekday grid, top apps, breakdowns, milestones, per-feature rows (meetings, files,
  walkthroughs, actions, commands, snippets, dictionary).
- Microphone check (Guided setup): opens the mic for 15 s, live level bar, verdict text.
- Walkthrough capture-delay chip and preset picker (0/1/2/3/5/10 s).

## Feature inventory

| ID | Feature | Exact user-visible behavior | Source | Status |
|---|---|---|---|---|
| A05-01 | Recorder start | Opens input stream 44100 Hz, 1 ch, int16, blocksize 1024 (`config.SAMPLE_RATE/CHANNELS/AUDIO_FORMAT/CHUNK_SIZE`). Returns False and sets `last_start_error` on failure; PortAudio-ish errors become "No audio device available", otherwise the exception text or "Could not open the audio stream". Second start while recording: False, "Recording already in progress" logged. | `recorder.py:AudioRecorder.start_recording`, `_format_start_error` | port |
| A05-02 | Ranked microphone candidates | Order: explicit `device_id`; if no priority and default not failed, PortAudio default (no device query, fast path); then ranked matches in order, then default input (role `default`), then on Windows MME "Microsoft Sound Mapper - Input" (role `mapper`). Each candidate must pass `check_input_settings` at the dictation format. Failed devices are excluded by index and by `{name,hostapi}` key. | `recorder.py:_candidates,_mark_failed,_open_first`; `audio_devices.py:ranked_candidates` | port |
| A05-03 | Device identity matching ladder | Saved entries are `{name, hostapi}`, never indexes. Match groups: same host API, then default host API, then others; rungs: exact, without "N- " duplicate prefix, MME 31-char truncation, without ALSA "(hw:X,Y)"; lowest index wins within a rung. Conservative: "Microphone (USB" does not match. | `audio_devices.py:match_entry,_LADDER` | port |
| A05-04 | Device listing | Every PortAudio input with >0 input channels except names containing "[Loopback]"; MME names exactly 31 chars get `label` = untruncated name from another host API. | `audio_devices.py:list_input_devices,_untruncated` | port |
| A05-05 | Device display names | `display_name`: drop "N- " prefix and "(hw:X,Y)", close an MME-cut parenthesis, remove space before ")". `short_name`: "Microphone (Blue Snowball)" -> "Blue Snowball" when the outer word is generic (microphone, microphone array, mic, headset, headset microphone, external/internal microphone, line, line in). `hostapi_label`: strips "Windows ". System aliases ("Microsoft Sound Mapper - Input", "Primary Sound Capture Driver") shown as the System default row. | `audio_devices.py:display_name,short_name,hostapi_label,is_system_alias` | port |
| A05-06 | Legacy mic index migration | Old `audio_input_device` int index -> `audio_input_priority` `[{name,hostapi}]` once, only if a device exists at that index now; old key kept for downgrade; when unplugged/PortAudio down nothing is saved and it retries on a later load. `from_settings` never lists devices on the Qt thread: migration runs on daemon thread "dictation-mic-migration" and the recorder uses the default until then. | `audio_devices.py:load_priority,legacy_migration_pending`; `recorder.py:from_settings,_adopt_migrated_priority` | port |
| A05-07 | PortAudio refresh | Re-initialise PortAudio to see newly plugged mics, refused (False) while any stream is open; retries init once on failure. Never on the start path (~25 ms). | `audio_devices.py:refresh_portaudio,register_stream,open_stream_count` | port (backend-dependent, see Porting notes) |
| A05-08 | WASAPI auto-convert | WASAPI devices open with `WasapiSettings(auto_convert=True)` so 44.1 kHz is accepted. | `audio_devices.py:extra_settings` | port (Windows) |
| A05-09 | Mid-recording mic switch | Watcher (100 ms) detects loss: stream finished callback, stream inactive, no block for 1.0 s after blocks flowed, or no first block within 3.0 s. Opens next candidate at same format before closing the dead one (close on a thread, joined 1.0 s max); appends 250 ms zero samples; resets post-roll gate if stop not yet pressed; calls `device_switch_callback(old_short_name, new_short_name)` ("" = system default / mapper). No switch after the stop press. | `recorder.py:_watch_capture,_capture_lost,_switch_device,_append_switch_gap,_retire_stream,_switch_name` | port |
| A05-10 | Capture error handling | Lone input overflow: block kept, recording continues (count logged). 3 overflows within 5 s: "Audio input kept overflowing because the computer could not keep up; recording stopped." Any other status flag: "Audio input error (<status>); recording stopped." One visible failure per recording via `error_callback`. | `recorder.py:_status_error,_fail_capture,OVERFLOW_LIMIT/OVERFLOW_WINDOW_S` | port |
| A05-11 | Adaptive post-roll | Cap `POST_ROLL_MS`=1200. Histogram of per-block RMS dBFS (-120..0, 121 bins). At stop: floor = 10th pct, speech = 90th pct; threshold = max(floor+12 dB, speech-30 dB); ends after 300 ms of consecutive blocks below threshold. Falls back to cap if disabled, < 500 ms captured before stop, or speech-floor < 20 dB. Repeat stop keeps the first cap. Stop while paused ends at once ("paused"). One INFO log line "Post-roll ended by <reason> after N ms (M ms of audio after stop; <detail>)". | `recorder.py:PostRollGate,block_level_db,stop_recording,_log_post_roll`; `config.py:369-401` | port |
| A05-12 | Pause / resume | `pause()` False unless recording and not stopping/canceled. While paused, blocks are dropped (mic still watched for loss); frame count continues from where it paused. Next recording starts unpaused. | `recorder.py:pause,resume,_audio_callback` | port |
| A05-13 | Cancel | `cancel_recording`: sets canceled, stop + end post-roll ("cancel"), discards the journal asynchronously (writes a `discarded` marker first). `capture_canceled` stays True until next start. | `recorder.py:cancel_recording,capture_canceled` | port |
| A05-14 | Crash-safe recording journal | Per recording dir `<parent>/.<stem>-recovery/<uuid hex>/` holding `active.lock` (OS lock), `session.json`, `audio.pcm`. Callback enqueues (queue 512 blocks, non-blocking); writer thread "dictation-writer" writes and fsyncs at most every 1.0 s. Queue full -> "Recording storage cannot keep up; recording stopped." Write failure -> "Recording could not be saved: <exc>". Finish timeout 3.0 s. | `recording_journal.py:RecordingJournal` | port |
| A05-15 | Recovery of interrupted recordings | On startup (`TranscriptionRuntime.recover_recordings`, executor): each session dir not locked by a live process; `discarded` marker -> delete; else write `RECORDINGS_FOLDER/recording_<YYYYmmdd_HHMMSS>-recovered-<dirname>.wav` via `.wav.tmp` + rename (idempotent), then delete the session. Validates rate 8000..384000, channels 1/2, width 1..4. Empty session (folder without files) removed silently. | `recording_journal.py:recover_recordings,_recover_session` | port |
| A05-16 | Save WAV | Atomic: temp file in the same dir, WAV header + journal PCM + 500 ms silence (`END_PADDING_MS`), then origination tags, fsync, `os.replace`. Refuses if journal did not finish ("Recording storage did not finish; audio kept for recovery.") or errored; dropped frames -> "Audio blocks were lost; the partial recording was kept."; an incomplete capture saves only with `allow_incomplete=True`. Previous output is kept until a new save succeeds. | `recorder.py:save_recording,_capture_saveable,_end_padding_bytes,_write_wav_file` | port |
| A05-17 | WAV origination metadata | Appends `LIST/INFO` (ICRD "YYYY-MM-DD", ISFT "OpenWhisper") and 602-byte `bext` (originator "OpenWhisper" at 256..288, date 320..330, time 330..338) after the data chunk, fixes RIFF size; pads an odd-length file first; on failure truncates back to the untagged WAV; refuses > 4 GiB RIFF. Failure only logs a warning. | `wav_metadata.py:append_origination`; `recorder.py:_stamp_origination` | port (owner: rename to FioriVoce?) |
| A05-18 | WAV helpers | `is_mono_pcm16(path)` (header walk, 16 chunks max), `read_info_icrd`, `read_info_isft`, `read_bext_origination`. | `wav_metadata.py` | port |
| A05-19 | Level meter | Per block RMS/32767 (int16), exponential smoothing `level = 0.3*prev + 0.7*rms` (`WAVEFORM_LEVEL_SMOOTHING`=0.3), clamped 0..1, delivered on the audio thread to `audio_level_callback`. | `recorder.py:_calculate_and_report_level` | port |
| A05-20 | Incremental reading of a recording | `read_recorded_bytes(offset)` returns PCM committed by the writer from `offset`, None if cleared/restarted. | `recorder.py:read_recorded_bytes`; `recording_journal.py:read_from` | port |
| A05-21 | Capture metrics | At end of each capture: `diagnostics.record_metrics(captured_frames, dropped_frames, device_switches, input_overflows)`. | `recorder.py:_report_capture_stats` | port |
| A05-22 | Window live preview | Every 3.0 s of new audio (`STREAMING_CHUNK_DURATION_SEC`), decode window = 0.75 s overlap tail + new audio, resampled to 16 kHz (FFT resample), `beam_size=1, vad_filter=False`; join with bounded seam de-duplication (overlap words = min(12, ceil(0.75*8)) = 6, at least one new word kept); callback `(full_text, True)`. Queue >= max(10, ceil(3.0 s * rate / 1024)) blocks, drop when full. Stop never decodes; `finalize_preview(timeout=5)` decodes the leftover only when needed (empty final transcript). Slow windows (>5 s) warn after 3, at most every 30 s. | `streaming_transcriber.py:StreamingTranscriber,append_preview_text,prepare_preview_audio,fft_resample` | port (adapt to Recognizer) |
| A05-23 | Native streaming preview | Pushes every 0.75 s (`STREAMING_NATIVE_UPDATE_SEC`) with no overlap to `backend.stream_audio("dictation-preview", pcm16k, "auto", finish)`; queue 10 s; ledger of interim/final events; stop waits <= 5 s for the finish push; `finished_stream()` exposes `FinishedStream(backend, generation, text, audio_sec)`. | `streaming_transcriber.py:NativeStreamingTranscriber,NativePreviewLedger,FinishedStream` | undecided backend (Nemotron); Moonshine/Parakeet Unified parts drop |
| A05-24 | Stream-final reuse | For a `stream_final` model, dictation >= 30 s (`STREAM_FINAL_MIN_SEC`) whose stream heard the recorded length within 0.1 s: paste stream text, skip file decode. | `incremental_dictation.py:stream_final_text` | undecided backend (Nemotron) |
| A05-25 | Incremental dictation | For engines in `INCREMENTAL_DICTATION_BACKENDS` (parakeet, ...) and recorder mono int16: once >= 30 s captured, a daemon thread polls every 1.0 s, resamples recorder PCM to 16 kHz with the same resampler as the file path, cuts windows with `WindowSplitter` (30 s max, split at min-energy 100 ms frame in 24..30 s) and decodes each. At stop: CRC-32 of followed PCM vs saved WAV prefix, SHA-256 fingerprint per window vs `windows(saved_file)`, resampled total within 1 sample; decodes remaining windows; any doubt -> plain file decode. Invalidated by engine reload/cancel/switch, recorder switch, meeting start, language/vocabulary change, error, capture cleared. | `incremental_dictation.py:DictationSession,IncrementalDictation` | port (adapt; resampler is ours) |
| A05-26 | History playback | Plays only the recorder's own format (PCM, 1 ch, 16-bit, 44100 Hz); errors "not a WAV recording", "not in the recorder's format", "the recording is empty". Own output stream; samples load on "history-playback" thread; `on_finished` exactly once per play; position/duration in seconds; process-wide `player()`, `stop_playback()`. | `audio_player.py` | port |
| A05-27 | Audio file preview | `preview_file(path, engine_splits)`: empty file -> "This file is empty."; permission -> "OpenWhisper isn't allowed to read this file."; no audio stream -> "This file has no audio track."; no samples -> "This file has no audio in it."; otherwise "This doesn't look like an audio file.". Duration from stream header, then container, then counted by decoding. Size text "X.Y MB" or "N KB"; duration via `format_audio_duration`. `over_upload_limit` > 23 MB. | `audio_processor.py:AudioFilePreview,preview_file,_probe_audio_header` | port |
| A05-28 | Silence-aware splitting | Streaming decode to mono s16 at native rate; window max samples = (23 MiB - 44) / 2; overlap 2.0 s (<= max/4); search for silence of 0.5 s below 0.01 (100 ms moving average of |x|/32768) from the end backwards in 0.1 s steps, at least 30 s into the window; else cut at window end - overlap. Chunks `chunk_%03d.wav` in a temp dir "audio_chunks_*"; progress "Decoding and splitting audio..." / "Created audio chunk N..."; cancel -> RuntimeError "Transcription canceled". `combine_transcriptions` joins with single spaces. | `audio_processor.py:split_audio_file,_split_blocks,_chunk_boundary,_find_best_silence,_moving_average,combine_transcriptions` | undecided backend (OpenAI API only) |
| A05-29 | Dictation languages | Accepted per engine (Parakeet: en, ru, es, fr, pt; "auto" excluded); chosen list cleaned; active used only when accepted; `cycle` wraps; `add_chosen` keeps the engine's own language active; chip text "AUTO"/"EN"; label lookup. Single-language reasons e.g. "This engine uses one language.". Switching never reloads the engine nor writes `local_asr_language`. | `dictation_language.py` | port (drop whisper/moonshine/mlx/unified branches; remote undecided) |
| A05-30 | Dictation job | `DictationJob` (mode dictation/command/transform/action/walkthrough, focus/target/selection futures, recognition, 16-hex id, action_id, source, route, routed, changed_by_program); `begin_job` starts focus capture right after the recorder opens; walkthrough reads nothing on screen; programs' jobs never read on-screen text. | `dictation_pipeline.py:DictationJob,begin_job,_focus_requests` | port |
| A05-31 | Recognition context | Language = `job_language(settings)`; phrases from dictionary when "steer recognition" is on. | `dictation_pipeline.py:recognition_for` | port |
| A05-32 | Text preparation | Dictionary replacements on every transcript; snippets only for dictation/action/walkthrough with snippets on; whole-utterance snippet skips AI cleanup. Every failure keeps the input. | `dictation_pipeline.py:prepare_text,PreparedText` | port |
| A05-33 | Cleanup prompt composition | Base (profile, or level preset/custom + learned rules) then, each separated by a blank line and omitted when empty: app style (no profile), inline-lists block (dictation, no profile, not terminal style), dictionary, snippet guard, caret context, then batch context with injection guard. | `dictation_pipeline.py:compose_cleanup_prompt,_base_cleanup_prompt,cleanup_level` | port (cloud AI cleanup itself is another area) |
| A05-34 | Finishing text | Expand snippets in cleaned text; lost placeholder -> uncleaned expansion with `ok=False`; plain + HTML; `raw_text`. | `dictation_pipeline.py:finish_text,FinishedText` | port |
| A05-35 | Caret join and paste-target check | Dictation joins onto text before a known caret (plain and HTML must agree). Rewrites paste only when the starting app provably still has focus (`same`/`changed`/`unknown`, 0.5 s budget on Linux). | `dictation_pipeline.py:text_for_paste,html_for_paste,paste_target,paste_target_ok,PASTE_CHECK_TIMEOUT_S` | port |
| A05-36 | After paste / history fields / stats counts | Dictionary learning scheduled for dictation only; history context columns `entry_kind, app_id, app_name, app_category, cleanup_level, language` (unknowns omitted, never window titles/text); stats counts `snippets, snippet_words, corrections`. | `dictation_pipeline.py:after_paste,history_fields,stats_counts,record_stats` | port |
| A05-37 | Stats recording | One `dictation_stats` row per saved history entry of kinds dictation/command/transform/action/file/walkthrough; not for other computers' entries (`origin_device_id`) or re-transcriptions; dedupe by `entry_id`; listeners called on the worker. One-time backfill of old history behind marker rows; meetings counted after they end. Reset deletes all and writes both markers. | `dictation_stats.py:record,_stat,_ensure_backfilled,count_features,reset,add_listener` | port |
| A05-38 | Stats summary | Words = CJK chars or whitespace tokens containing a word char; words edited via SequenceMatcher opcodes; pace over recordings >= 1 s; time saved vs 40 WPM; streak counts today or from yesterday; activity 371 days; periods week (7), month (30), all (monthly bars, 6..12); top 5 apps; top 3 action targets; milestones 1k, 5k, 10k, 25k, 50k, 100k, 250k, 500k, 1M, then each million. | `dictation_stats.py:load_summary,_period,_features,words,words_edited,next_milestone` | port |
| A05-39 | Microphone check widget | 15 s listen (`LISTEN_MS`), 4 s quiet warning, bar = sqrt((level-0.004)/(0.2-0.004)) * 100, "heard" at level >= 0.02. Strings listed in Modules. | `ui_qt/widgets/microphone_check.py:MicrophoneCheck` | port |
| A05-40 | Capture delay chip and picker | Chip "Delay · Instant" / "Delay · Ns" / "Snap in N.Ns" countdown ring; picker 264x170 with 6 presets; arrow keys step; saves `walkthrough_capture_delay`. | `ui_qt/widgets/capture_delay.py:CaptureDelay,_DelayPicker` | port |

### Added by check

| ID | Feature | Exact behavior | Source | Status |
|---|---|---|---|---|
| A05-41 | Recorder failure strings not listed above | Callback exception: journal error `Audio capture failed: <exc>` then capture stops (reported by the watcher, never logged on the audio thread). Watcher crash: "Audio capture stopped unexpectedly: <exc>". Stop-time journal states: "Recording storage did not finish; audio kept for recovery." (journal did not finish in 3 s), the journal's own error, then "Audio blocks were lost; the partial recording was kept.". All go through `_fail_capture` once per recording. `get_input_devices()` logs and returns `[]` on enumeration failure. Setters `set_audio_level_callback` / `set_streaming_callback` exist besides the attributes. | `recorder.py:_audio_callback,_wait_for_stop,_capture_saveable,get_input_devices` | port |
| A05-42 | Dictation job focus-capture rules | `begin_job` runs only when app awareness (`resolve_app_context_enabled`) is on for `focus`; walkthrough captures nothing. `include_text` = user-started source AND "read text" setting; `include_selection` = user-started AND (command: always; dictation: same as include_text; action: only when the action wants the selection; transform/walkthrough: never). User-started = `catalog.user_started(source)` (programs/API/MCP/D-Bus jobs get only the focused app, never on-screen text). With app awareness OFF: command/transform/action jobs, and any dictation with a `route`, still make a text-less `target` request so the paste check can work; plain dictation requests nothing. Any exception: warning, job continues with no capture. `DictationJob.program_words` = `changed_by_program` or source not user-started. | `dictation_pipeline.py:_focus_requests,begin_job,DictationJob.program_words` | port |
| A05-43 | Future resolution | `_resolved(future, timeout)`: timeout <= 0 returns the result only if already done, else None; a timed-out, cancelled or failed future gives None (debug log). `_as_future` wraps a plain str as a finished Future; anything else non-Future is None. | `dictation_pipeline.py:_resolved,_as_future` | port |
| A05-44 | Cleanup level recorded | `cleanup_level(settings, profile)`: "profile" when a cleanup profile ran; else "custom" when a custom prompt is saved; else the preset name; "" for level NONE or on error. `history_fields` entry_kind = the job mode when not dictation; else "dictation" if `live` else "file"; app_id/app_name from focus identity; app_category via `app_styles.category_for` (works with styles off); `cleanup_level` from the CleanupInfo; `language` from the job's recognition. Empty values are dropped. | `dictation_pipeline.py:cleanup_level,history_fields` | port |
| A05-45 | Stats counts detail | `stats_counts`: `snippets` = number of snippets used (whole-utterance counts 1, else placeholders); `snippet_words` = sum(max(0, words(snippet.text) - words(trigger))); `corrections` = `words_edited(original, dictionary-applied text)`; zero values omitted; failure returns {}. | `dictation_pipeline.py:stats_counts` | port |
| A05-46 | Stats row rules (`_stat`) | Kind = `entry_kind`, else "dictation" if `source_name` starts "Quick Record" else "file". Counted: dictation, command, transform, action, file, walkthrough; skipped when `origin_device_id` set; skipped for `file` with `stats_counts.retranscription`. `spoken_words` is filled ONLY for kind dictation (from `spoken_text` or `raw_text` or `text`); other kinds store 0. `final_words` = words in `text`. `words_edited` only for command/transform/action/dictation (counted kinds) with raw text, between `raw` and `cleaned_text or text`. `audio_seconds` kept only for dictation and file/walkthrough/meeting. `target`, `snippets`, `snippet_words`, `corrections`, `shots` clamped to >= 0 ints. Row timestamp defaults to now (UTC ISO). | `dictation_stats.py:_stat,_kind,_count` | port |
| A05-47 | Stats backfill and feature counting | `_ensure_backfilled`: skipped if a marker row with `target` NULL exists; otherwise counts history rows with `origin_device_id` NULL and (`entry_kind` = dictation or (NULL kind and source "Quick Record%")) not already counted by `entry_id`, then writes the marker; logs "Counted N earlier dictations for Stats". `count_features` (also called by `load_summary`): first run (no "features" marker) adds history file/walkthrough rows (also NULL-kind non-"Quick Record" rows), skipping `failed_recording` entries and own `recording_*.wav` re-transcriptions, then writes the "features" marker. Every run adds finished meetings (`origin_device_id` NULL, `ended_at` set, status not active/paused/ending) not yet counted, and, when a marker exists, only those ended after the marker timestamp. Meeting row: `entry_id` = meeting id, timestamp = `started_at`, `final_words` = sum of words in `meeting_segments.text`, `audio_seconds` = (end - start) - `paused_total_s`, floored at 0; skipped if either time is unparsable. Logs "Counted N files, walkthroughs and meetings for Stats". `reset()` logs "Stats reset". `add_listener`/`remove_listener` exist. | `dictation_stats.py:_ensure_backfilled,_history_features,_meeting_stat,count_features,reset` | port |
| A05-48 | Stats summary shape | `StatsSummary`: `words_all_time` (sum of spoken words over all counted+feature rows), `entries`, `day_streak`, `longest_streak`, `dictated_today`, `first_day`, `activity` (371 `(day, words)` oldest first), `periods` (week, month, all). Streak/active days use only counted kinds (dictation/command/transform/action); a rewrite-only day is active with 0 words. `PeriodStats`: words, `previous_words` (same span just before; None for all time), dictations, rewrites (= other counted kinds), `average_wpm` (rows >= 1 s with words, rounded to 0.1), `words_cleaned_up` (sum words_edited), `top_apps` (5; ties by casefolded name), `bars` (per day, or per month for all: ends at current month, goes back to the first month but at least 6 and at most 12 bars), `hours` (24), `weekdays` (Mon first), `grid` 7x24, `categories`/`cleanup_levels`/`languages` (words shares, most first then key; empty key = unknown; the list is omitted when no row names one), `features` (7). `minutes_saved` = words/40 - words/average_wpm, 0 when WPM <= 40. | `dictation_stats.py:load_summary,_period,_month_bars,_shares,_streak,_longest_streak,_pace,_top_apps` | port |
| A05-49 | Stats per-feature rows | meetings: count, seconds, words (final_words). files: count, seconds, words. walkthroughs: count, seconds, `shots`. actions: count and `top` 3 targets (by times sent, ties casefolded name). commands (command + transform kinds): count, words = sum of words_edited. snippets: count = sum of `snippets`, words = sum of `snippet_words`. dictionary: count = sum of `corrections`. Each carries `trend` = counts per bar of the span chart. Feature rows use every row in the span, including file/meeting rows; headline words/dictations use counted kinds only. | `dictation_stats.py:_features` | port |
| A05-50 | Dictation language rules detail | `chosen_languages`: saved list limited to known codes (labels table), never "auto", no repeats, order kept. `language_choices` = chosen AND accepted by the engine. `job_language` = active code only if it is a current choice, else "" (engine's own). `engine_language` (Parakeet path) = `local_languages.selected_language(engine, local_asr_language)`. `current_language` = job language else engine language. `set_chosen` stores the cleaned list and, if the stored active language is no longer a choice, sets it to the first choice or "". `cycle` does nothing with < 2 choices; from a current language not in the list it goes to the first choice. Labels come from `local_languages.LANGUAGE_LABELS` plus the meeting language choices (English names). | `dictation_language.py` | port |
| A05-51 | Audio file preview extras | Missing path raises FileNotFoundError "Audio file not found: <path>". `needs_splitting` is true only when the engine splits AND size > 23 MB (`MAX_FILE_SIZE_MB`); otherwise one chunk of the whole duration. Preview estimate uses `_generate_time_based_splits` (time-only, no decoding). Logs "Audio preview: name, X MB, Ys, N chunk(s)". Split: "Upload size limit is too small for a WAV file" and "Invalid audio sample rate" ValueErrors; empty/no-audio errors above. | `audio_processor.py:preview_file,split_audio_file` | port (splitting undecided) |
| A05-52 | Recovery format validation | An invalid session format raises "Invalid recovery audio format" (session skipped, logged); a short PCM source while copying raises OSError and the temporary `.wav.tmp` is not promoted. | `recording_journal.py:_session_format,_copy_pcm` | port |

## Modules

### services/recorder.py

Responsibility: dictation capture (also used by walkthroughs, the microphone check and the cleanup-rules dialog) with
device fallback, post-roll, pause, cancel, journal, save.

Public surface:
- `block_level_db(audio) -> float` (pure), `PostRollGate(rate)` with `observe(level_db, frames) -> bool`, `arm()`,
  `describe()`, fields `floor_db, speech_db, threshold_db, fallback, frames_before_stop, frames_after_stop`.
- `AudioRecorder.get_input_devices()`, `AudioRecorder.from_settings()`, `AudioRecorder(device_id=None, output_file=None, *, device_priority=None)`.
- Attributes: `device_priority`, `active_device`, `output_file` (default `config.RECORDED_AUDIO_FILE`), `is_recording`,
  `last_start_error`, `last_capture_error`, `dropped_frames`, `input_overflows`, `channels`, `dtype`, `rate`, `chunk`.
- Callbacks (all called from audio or watcher threads): `audio_level_callback(float)`, `streaming_callback(ndarray)`,
  `error_callback(str)`, `device_switch_callback(old, new)`.
- Methods: `start_recording()`, `stop_recording()`, `pause()`, `resume()`, `paused`, `wait_for_stop_completion(timeout=None)`
  (default (1200+800)/1000 + 3.0 = 5.0 s), `save_recording(filename=None, *, allow_incomplete=False)`,
  `get_recording_duration()`, `has_recording_data()`, `read_recorded_bytes(offset)`, `cancel_recording()`,
  `capture_canceled`, `clear_recording_data(*, discard=False, wait=True)`, `acknowledge_recording()`, `cleanup()`.

Threading:
- PortAudio callback thread runs `_audio_callback` under `_callback_lock`. No logging or file I/O there (comment at
  recorder.py:645, :459); finished callback only sets an event.
- Watcher thread per recording (`_wait_for_stop`): `_watch_capture` loop waits on `stop_event` every 0.1 s; then waits on
  `post_roll_end_event` up to the deadline; `finally` closes stream (abort then close; abort measured 7 ms vs 33 ms for
  stop, recorder.py:751), logs post-roll, finishes journal, reports metrics, clears state, sets complete event.
- Fresh `threading.Event`s per recording so an old thread cannot signal a new session (recorder.py:319).
- `_stream_generation` counter: every open/close bumps it; stale stream blocks and finished callbacks are ignored
  (recorder.py:614, :460). During a switch the replacement is opened with generation+1 while the current is bumped,
  so its blocks wait until installed (recorder.py:804-808, test `test_blocks_from_the_replacement_never_land_before_the_gap`).
- `_session_token`: the journal's error callback only fires for the current session (recorder.py:343-347, test
  `test_old_writer_error_cannot_stop_next_recording`).
- Discard of a journal runs on a "dictation-release" thread (`clear_recording_data(wait=False)`); `cleanup` joins them 0.5 s.

State machine: idle -> recording (stream open, watcher running) -> [paused <-> recording] -> stop requested (gate armed,
deadline set) -> post-roll (ends on quiet/cap/paused/cancel/capture error/cleanup/microphone lost) -> closed (journal
finished) -> saved (WAV written) -> acknowledged (journal deleted) or kept for recovery.

Edge cases to keep:
- Stop with no recording returns False (recorder.py:529).
- Repeat stop during post-roll keeps the first deadline (recorder.py:535).
- Stop while paused ends post-roll immediately (recorder.py:545).
- Overflow handling: only `input_overflow` without `input_underflow` is tolerated (recorder.py:675).
- `_capture_lost` returns True if stream not `active` (recorder.py:787).
- Switch ending during stop/cancel: replacement closed, old retired, post-roll ended "microphone lost" (recorder.py:822-835).
- Lost stream close may hang in the driver: closed on a thread joined at most 1.0 s (recorder.py:874-883; tests in
  `test_personalize_fix_f3_lost_mic_close.py`).
- `_mark_failed(None)` resolves the default device so its key is excluded too (recorder.py:426-438).
- `_candidates` is lazy: failures marked between tries are skipped (recorder.py:399-424).
- Device listing failure falls back to default only (recorder.py:419-421).
- `save_recording` with no data removes the temp file and returns False (recorder.py:946-951).
- Origination stamp failure keeps the WAV (recorder.py:173-178, test `test_save_recording_keeps_wav_when_metadata_stamp_fails`).
- Cleanup with an active recording ends the post-roll at once and joins the thread 0.5 s (recorder.py:1104-1116).

### services/audio_devices.py

Responsibility: microphone identity, listing, matching, ranking, migration, PortAudio refresh; shared with Meeting Mode
(`meeting/capture/devices.py` re-exports it; meeting passes `check=False`, native rates).

Public surface: `InputDevice` (frozen: index, name, hostapi, channels=1, samplerate=44100, label, default_api, role;
`key`, `display`), constants `LOOPBACK_MARKER`, `SOUND_MAPPER_NAME`, `MME_NAME_LIMIT=31`, `ROLE_DEFAULT`, `ROLE_MAPPER`,
`portaudio_lock` (RLock), `device_key`, `normalize_priority`, `list_input_devices`, `is_system_alias`, `match_entry`,
`extra_settings`, `can_open`, `default_input`, `ranked_candidates(priority, *, exclude, sd, devices, check, samplerate,
channels, dtype)`, `sound_mapper`, `legacy_migration_pending`, `load_priority(settings=None, *, manager, sd)`,
`register_stream`, `unregister_stream`, `open_stream_count`, `refresh_portaudio`, `display_name`, `short_name`,
`hostapi_label`.

Threading: every device query and stream open happens under `portaudio_lock` because re-initialising PortAudio
invalidates all indexes and streams. Default input: `sd.default.device[0]` (an `_InputOutputPair`, indexable but not a
tuple: audio_devices.py:96-108) then the default host API's `default_input_device`.

### services/recording_journal.py

Responsibility: bounded queue + writer thread to a raw PCM file, OS-lock lease, recovery.
Constants: `QUEUE_BLOCKS=512`, `CHECKPOINT_SECONDS=1.0`, `FINISH_TIMEOUT=3.0`, `COPY_BYTES=1 MiB`, `POLL_SECONDS=0.05`,
`_WAKE` sentinel (finish wakes the writer immediately; saved 29-56 ms per stop, recording_journal.py:27-30).
Public: `recovery_directory(output_file)`, `RecordingJournal(output_file, rate, channels, sample_width, on_error, *, queue_blocks)`
with `append(bytes)->bool`, `finish(timeout)->bool`, `read_from(offset)`, `copy_into(wave_writer)`, `close(*, discard)`,
`error`, `written_bytes`, `dropped_frames`, `directory`, `path`; `recover_recordings(output_file, recordings_folder) -> list[str]`.
Lease: `msvcrt.locking(LK_NBLCK, 1 byte)` on Windows, `fcntl.flock(LOCK_EX|LOCK_NB)` elsewhere; released by the OS on kill.
Edge cases: discard writes a `discarded` marker with fsync before anything else, so recovery honours Cancel even if killed
(recording_journal.py:198-215); a timed-out writer keeps the lease and removes audio when it unblocks
(`_run` finally, :135-139); empty `audio.pcm` removed on release (:226); `_remove_session` deletes only the four known
names then `rmdir` (:230-234); recovery treats `BlockingIOError`/`PermissionError` as "active" and `FileNotFoundError`
as an empty session (:304-315); copy size rounded down to whole frames.

### services/wav_metadata.py

Pure binary RIFF code; described in A05-17/18. `append_origination` works on the fd (not buffered writer) so a partial
tag can be rolled back (wav_metadata.py:42-58).

### services/audio_player.py

`recording_frames(path)`, `UnsupportedAudio`, `AudioPlayer(sounddevice=None)` with `play(path, on_finished)->bool`,
`stop()`, `is_playing`, `session` (play counter, 0 when idle), `path`, `position`, `duration`, `last_error`;
module `player()`, `stop_playback()`. Two locks: `_lock` (state) and `_stream_lock` (stream ops) because PortAudio's
finished callback takes `_lock` while `abort()` runs under `_stream_lock` (audio_player.py:89-91). Stream closed from a
helper thread "history-playback-close" since it cannot be closed in its own finished callback. Output: int16 mono 44100,
callback copies samples then zero-fills and raises `CallbackStop`.

### services/audio_processor.py

`UnreadableAudioError`, `NOT_AUDIO_MESSAGE`, `AudioFilePreview` (file_path, file_name, file_size_mb, duration_seconds,
sample_rate, channels, needs_splitting, estimated_chunks, chunk_durations; `over_upload_limit`, `duration_formatted`,
`file_size_formatted`), `_moving_average` (blocked cumsum, block 1<<22 samples, matches `np.convolve(mode="same")` to 3e-8),
`AudioProcessor` with `check_file_size`, `preview_file`, `split_audio_file(path, progress_callback, should_cancel)`,
`_find_split_points` (array helper), `_generate_time_based_splits` (preview estimate), `cleanup_temp_files`,
`combine_transcriptions`; module singleton `audio_processor`. Memory bounded to one upload window + 1 sample; the +1
lookahead avoids an overlap-only final chunk (audio_processor.py:221-226). Decode: PyAV `AudioResampler(format="s16",
layout="mono", rate=native)` with `frame.pts=None`, flush at end (audio_processor.py:334-364).

### services/streaming_transcriber.py

- `fft_resample(samples, n)` (pure; scipy-compatible Nyquist handling) - also used by `meeting/capture/spool.py`,
  `meeting/asr/audio.py`, `meeting/diarize/clustering.py`.
- `append_preview_text(existing, chunk, *, max_overlap_words)` (pure; word regex `\w+(?:['’]\w+)*`, casefold, ’ -> ',
  looks at last/first 512 chars).
- `prepare_preview_audio(pcm, rate)` -> 16 kHz float32 mono (int16 / 32768).
- `FinishedStream` NamedTuple, `NativePreviewLedger.apply(events)` (events with `id` = Moonshine lines sorted by `start`;
  else `final` commits, non-final replaces partial).
- `StreamingTranscriber(backend, chunk_duration_sec=3.0, overlap_sec=None)`: `start_streaming(rate, callback)`,
  `feed_audio(block)` (called from recorder callback; non-blocking), `stop_streaming() -> str` (no wait),
  `finalize_preview(timeout=5.0) -> str` (worker thread only), `cancel_streaming()`, `cleanup()`. Worker thread polls
  0.1 s, `_WAKE` wakes it. States: idle, streaming, stop pending finalize (`_finalize_pending`), finalizing, discarded.
  Retained audio = worker's partial window + queued blocks; a new start, cancel or cleanup drops it.
- `NativeStreamingTranscriber(backend, update_interval_sec=None)`: session name "dictation-preview"; stop joins <= 5 s;
  failure sets `_failed` and cancels the session once; a dropped block forbids `finished_stream`.
Callers: `services/runtime/streaming.py` (chooses native vs window by `STREAMING_NATIVE_PREVIEW_BACKENDS` /
`STREAMING_PREVIEW_BACKENDS`, sets recorder callbacks), `transcription.py:821` (`keep_stream_final`).

### services/incremental_dictation.py

`DictationSession.create(controller, recognition)` (eligibility), `start`, `discard`, `poll`, `finish(audio_path)`,
`matches(recognition)`, `early_windows`; `stream_final_text(backend, finished, recorded_sec)`; `IncrementalDictation`
(runtime slot) with `start(controller, *, recognition)`, `discard()`, `keep_stream_final(finished, recorded_sec)`,
`transcribe(backend, audio_path, *, recognition)`. Backend contract used: `generation`, `should_cancel`,
`is_available()`, `backend_id`, `model_name`, `is_transcribing`, `recognition_inputs(recognition) -> (language, phrases)`,
`decode_window(audio, language, phrases=...)`, `transcribe_windows(iterable, language, phrases=...)`,
`join_texts(list)`, `transcribe(path, recognition=...)`. `finish` sets `backend.is_transcribing=True` while joining the
poll thread so a cancel tears down the in-flight decode (incremental_dictation.py:276-285). Only applies when the job's
audio path is `config.RECORDED_AUDIO_FILE` and the session's recorder output file.

### services/dictation_language.py

Pure settings logic plus one module-level remote state (`note_remote_runtime`, lock-protected). Functions: `engine`,
`accepted_languages`, `single_language_reason`, `chosen_languages`, `language_choices`, `job_language`,
`engine_language`, `current_language`, `set_active`, `set_chosen`, `add_chosen`, `cycle` (mutators for
`settings_manager.mutate_settings`), `label`, `short_label`. Callers: waveform overlay chip, language menu, local engine
controls, ui_controller, omarchy controls, `dictation_pipeline.recognition_for`.

### services/dictation_pipeline.py

Stage functions called by `TranscriptionRuntime`, `runtime/command.py`, `actions/runtime.py`,
`automation/filters.py`: `begin_job`, `recognition_for`, `prepare_text`, `compose_cleanup_prompt`, `cleanup_level`,
`finish_text`, `text_for_paste`, `html_for_paste`, `paste_target`, `paste_target_ok`, `after_paste`, `history_fields`,
`stats_counts`, `record_stats`; types `JobMode`, `FILE_ENTRY_KIND="file"`, `DictationJob`, `PreparedText`,
`FinishedText`, `PasteTarget`. Invariant: no stage raises into the runtime; each failure logs and returns its input
(dictation_pipeline.py:1-9). Futures: Qt-thread callers only use timeout 0; workers wait up to
`CONTEXT_CAPTURE_DEADLINE_S`=0.4 s.

### services/dictation_stats.py

Constants: `COUNTED_KINDS=("dictation","command","transform","action")`, `FEATURE_KINDS=("file","walkthrough","meeting")`,
`_MARKER_KIND="marker"`, `_FEATURES_MARKER="features"`, `FEATURES=("meetings","files","walkthroughs","actions",
"commands","snippets","dictionary")`, `TOP_TARGETS=3`, `MIN_PACE_SECONDS=1.0`, `TOP_APPS=5`, `TYPING_WPM=40`,
`ACTIVITY_DAYS=371`, `MONTH_BARS=12`, `MIN_MONTH_BARS=6`, `MILESTONES`, `PERIODS`. CJK ranges `぀-ヿ㐀-䶿一-鿿豈-﫿`.
Types `PeriodStats` (incl. `minutes_saved`), `FeatureStats`, `StatsSummary`. `_write_lock` serialises writers.
Timestamp rules: new rows aware UTC ISO; old naive values are local wall time (`local_time`); meetings compare in UTC
(`_utc` treats naive as UTC). `load_summary` is blocking (call off the UI thread). Kind inference for legacy rows:
`source_name` starting "Quick Record" -> dictation, else file. Re-transcribed own recordings
(`source_name` "recording_*.wav") and `failed_recording` entries are excluded from the feature backfill.

### ui_qt/widgets/microphone_check.py (UI)

- Layout: `QVBoxLayout`, margins 0, spacing 6: `QProgressBar` objectName `microphoneCheckBar`, range 0..100, text hidden,
  fixed height 8, accessible name "Microphone level"; `WrappedLabel` objectName `guidedDetail`, plain text, dynamic
  property `tone` = "" / "attention" / "ready" (via `set_style_property`, the stylesheet keys on it).
- Signals: `running_changed(bool)`; private `_level(float)` queued from the audio thread.
- Timers: stop after 15000 ms, quiet check 4000 ms (single shot).
- Strings: "Listening. Say something and watch the bar. Nothing is recorded." / "Hearing you. This microphone works."
  (ready) / "Not hearing anything yet. Check that the microphone isn't muted, or pick another one." (attention) /
  "Didn't hear anything. Check that the microphone isn't muted, or pick another one." (attention, on stop without
  hearing) / "Couldn't open the microphone: <reason>." / "Couldn't open the microphone. Pick another one, or check that
  OpenWhisper is allowed to use it." (attention).
- Stops on hide; uses `cancel_recording()` + `cleanup()` so nothing is kept.

### ui_qt/widgets/capture_delay.py (UI)

- `CaptureDelay(QWidget)`: signals `changed(int)`, `cancelled()`. Fixed size width = max(148, advance("Delay · Instant")
  in chrome font 12 + 61), height 34. Focus: NoFocus when parented, StrongFocus otherwise. Pointing-hand cursor,
  translucent background, always-show tooltips. Tooltip: "Wait after each click or mark before taking a screenshot.
  Keep drawing to restart the countdown. Esc while drawing cancels a pending shot." Accessible name "Capture delay: N
  seconds" / "Capture delay: instant".
- Paint: rounded rect radius 10 (0.5 px inset), pen 1 px `accent-tint-border` when active or focused else `border`; fill
  `accent-tint` active / `surface-hover` hovered / `surface`; opacity 0.4 when disabled. Ring at (11,9,16,16): pen 1.6
  (`border-strong` when active, else ink); active arc from 12 o'clock clockwise, span = remaining/total; idle draws clock
  hands (19,12)-(19,17)-(22,19) pen 1.8 round cap. Ink = `accent` if active or seconds > 0 else `text-secondary`. Text
  rect (35, 0, w-61, h), chrome font 12, colour `text`: "Snap in 1.5s" / "Delay · 3s" / "Delay · Instant". Right side:
  active -> "drop" icon at (w-23,10,14,14) (click on x >= w-27 emits `cancelled`); idle -> chevron at x=w-17 pen 1.4.
- Keyboard: Esc closes picker; Left/Down step to shorter, Right/Up to longer preset (saves).
- `_DelayPicker`: Tool | Frameless | StaysOnTop | DoesNotAcceptFocus | NoDropShadow, translucent, show without activating,
  not quit-on-close, fixed 264x170, window title "Automatic screenshot delay". Positioned under the chip +7 px (above if
  no room), clamped to available geometry. macOS: nonactivating, all spaces, cover, `PARKED_LEVEL`; excluded from
  screenshots (`affinity.keep_out_of_screenshots`). Closes after the pointer is away from it and the chip's window for
  > 0.6 s (100 ms timer). Panel `paint_panel` radius 14 inset 2, shadow 0. Title "Screenshot delay" (chrome 13, `text`,
  rect 16,13,232,22); subtitle "Restarts after each click or mark" (chrome 11, `text-secondary`, rect 16,38,232,18).
  Options grid 3 columns: rect(14 + i%3*80, 68 + i/3*42, 76, 36), radius 8; selected `accent-border` pen,
  `accent-tint-strong` fill, `accent` text; hover `surface-hover`; labels "Instant", "1s", "2s", "3s", "5s", "10s"
  (chrome 12). Click = press and release on the same option.
- Depends on `ui_qt/snap` (chrome_font, paint_panel, paint_icon, macos, affinity) and `ui_qt/utils/palette` (a02/walkthrough areas).

### Added by check

- `NO_MICROPHONE_LEFT` text is the one in "What the user gets"; `COPY_BLOCK_BYTES` = 1 MiB is the WAV copy block (`COPY_BYTES` in the journal likewise).
- Recorder startup order in `start_recording`: journal created with an error callback bound to the session token, candidates tried, `begin_job` caller runs afterwards (see A05-30).
- `audio_player.py`: output stream callback runs `_fill` (copy, then zero-fill and `CallbackStop`); a start failure sets `last_error` (exception text) and still calls `on_finished` once; `play` calls `stop()` first (one playback at a time) and reads only the header on the caller's thread.
- `MicrophoneCheck` calls `_release` (cleanup) on every stop and on hide, and the bar shows `round(100 * min(1, (level-0.004)/0.196) ** 0.5)`; the "quiet" timer is cancelled once the level crosses 0.02.


## Data and formats

Settings keys (in `openwhisper_settings.json`):

| Key | Type | Default | Read where |
|---|---|---|---|
| `audio_input_priority` | list of `{name: str, hostapi: str}` | absent (= system default) | `audio_devices.load_priority`, settings_microphones dialog, meeting |
| `audio_input_device` | int (legacy PortAudio index) | absent | `audio_devices._legacy_index` (kept for downgrade) |
| `dictation_languages` | list of codes | `[]` | `dictation_language.chosen_languages` |
| `dictation_active_language` | str | `""` (engine's own) | `dictation_language.job_language` |
| `local_asr_language` | str | `"en"` | `dictation_language.engine_language` (read only) |
| `selected_model` | choice | `config.DEFAULT_BACKEND` | `dictation_language.engine` |
| `walkthrough_capture_delay` | int in (0,1,2,3,5,10) | 0 | `CaptureDelay` |
| (via resolvers) app context enabled/read text, app styles, dictionary steer recognition, snippets enabled, transcript cleanup prompt | | | `dictation_pipeline` |

Config constants that matter (config.py): SAMPLE_RATE 44100, CHUNK_SIZE 1024, CHANNELS 1, int16; POST_ROLL_* (above);
END_PADDING_MS 500; WAVEFORM_LEVEL_SMOOTHING 0.3; WHISPER_TARGET_SAMPLE_RATE 16000; MAX_FILE_SIZE_MB 23;
SILENCE_THRESHOLD 0.01; MIN_CHUNK_DURATION_SEC 30; SILENCE_DURATION_SEC 0.5; OVERLAP_DURATION_SEC 2.0;
STREAMING_ENABLED False; STREAMING_CHUNK_DURATION_SEC 3.0; STREAMING_OVERLAP_SEC 0.75; STREAMING_QUEUE_SIZE 10;
STREAMING_QUEUE_SEC 3.0; STREAMING_NATIVE_UPDATE_SEC 0.75; STREAMING_NATIVE_QUEUE_SEC 10.0;
INCREMENTAL_DICTATION_POLL_SEC 1.0; STREAM_FINAL_MIN_SEC 30.0; STREAM_FINAL_TOLERANCE_SEC 0.1;
CONTEXT_CAPTURE_DEADLINE_S 0.4.

Files on disk:
- `RECORDED_AUDIO_FILE` = `user_data_path("recorded_audio.wav")`: the last dictation (overwritten atomically per save).
- Journal: `<dir of recorded_audio.wav>/.recorded_audio-recovery/<32 hex>/{active.lock, session.json, audio.pcm, discarded}`.
  `session.json` = `{"version":1,"rate":44100,"channels":1,"sample_width":2,"created":"<local ISO>"}`; `audio.pcm` raw
  interleaved little-endian PCM. A FioriVoce install sharing the data folder must keep this layout to recover
  OpenWhisper journals (or ignore them; owner decision).
- Recovered WAVs: `RECORDINGS_FOLDER` (`user_data_path("recordings")`) `recording_<YYYYmmdd_HHMMSS>-recovered-<hex>.wav`.
- Split chunks: `%TEMP%/audio_chunks_*/chunk_000.wav`... mono s16 at the source rate.
- Saved WAV layout: RIFF/WAVE, `fmt ` PCM 1 ch 16-bit 44100, `data`, then `LIST`(INFO: ICRD, ISFT) and `bext` (602 bytes).
  `is_mono_pcm16` and History playback depend on this.

Database (`openwhisper.db`, SQLite via SQLAlchemy, table owned by the database area):
`dictation_stats(id INTEGER PK autoincrement, entry_id TEXT NULL, timestamp TEXT NOT NULL (ISO UTC), entry_kind TEXT NOT NULL,
app_name, app_category, cleanup_level TEXT NULL, spoken_words INT NOT NULL 0, final_words INT NOT NULL 0,
words_edited INT NOT NULL 0, audio_seconds REAL NULL, language TEXT NULL, target TEXT NULL (v18), snippets INT 0,
snippet_words INT 0, corrections INT 0, shots INT 0)`. Marker rows: `entry_kind='marker'`, `target` NULL (dictation backfill
done or reset) or `'features'`. Reads `transcription_history` (id, timestamp, text, raw_text, cleaned_text, audio_duration,
entry_kind, source_name, app_name, app_category, cleanup_level, language, origin_device_id), `meeting_sessions`
(id, started_at, ended_at, paused_total_s, status, origin_device_id), `meeting_segments(meeting_id, text)`.
History-save fields consumed by `record`: `spoken_text`, `stats_counts` (`retranscription`, `target`, `snippets`,
`snippet_words`, `corrections`, `shots`).

Event shapes: native stream events are dicts `{text, final?, id?, start?, end?}`; remote runtime info
`{family, languages: [...], selected: {language}}` (`note_remote_runtime`).

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| sounddevice (PortAudio) | input/output streams, device list, host APIs, `check_input_settings`, WASAPI auto-convert, `_terminate/_initialize` | PortAudio C library (vcpkg `portaudio`, with WASAPI/WDM-KS/DirectSound/MME on Windows, ALSA/JACK/Pulse on Linux, CoreAudio on macOS). Keeps the same device names and host API names that `audio_input_priority` stores. Qt Multimedia `QAudioSource`/`QMediaDevices` is the alternative but exposes no host API and different names (breaks saved keys). |
| numpy | PCM buffers, RMS, histogram, FFT resample, moving average | `std::vector<int16_t>/float`, hand loops; FFT via pocketfft (header-only) or KissFFT if `fft_resample` must be bit-close |
| wave | WAV read/write | own RIFF writer/reader (engine already has `src/engine/wav.cpp` reader) |
| av (PyAV / FFmpeg) | decode any audio file, s16 mono resample, 16 kHz windows | FFmpeg libavformat/libavcodec/libswresample (vcpkg `ffmpeg`). Same library family, so resampled samples can match bit for bit |
| threading, queue | threads, events, bounded queues | `std::thread`/`std::jthread`, `std::condition_variable`, a bounded SPSC ring buffer for the callback path (lock-free) |
| msvcrt.locking / fcntl.flock | journal lease | `LockFileEx` (Windows) / `flock` (POSIX), or `QLockFile` (but QLockFile uses PID files, not OS locks: avoid) |
| os.fsync, os.replace, tempfile | durability, atomic save | `FlushFileBuffers`/`fsync`, `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`/`rename`, or `QSaveFile` |
| zlib.crc32, hashlib.sha256 | incremental verification | zlib (vcpkg) or a small CRC32; SHA-256 small implementation or `QCryptographicHash` outside the engine |
| difflib.SequenceMatcher | `words_edited` | port SequenceMatcher (autojunk=False) faithfully; opcodes must match for golden tests |
| re | names, word tokens | `std::regex` is slow and has no Unicode `\w`; use ICU or QRegularExpression (Qt side) or hand-written classifiers |
| sqlalchemy | stats table | Qt SQL (`QSqlDatabase` SQLite) or sqlite3 C API, per database area decision |
| concurrent.futures.Future | focus/selection futures | `QFuture`/`std::shared_future` with timeout-0 polling |
| PyQt6 | widgets | Qt 6 Widgets |

## Platform-specific code

- Windows: MME 31-char name truncation and the `label` repair; "N- " duplicate numbering; System aliases ("Microsoft Sound
  Mapper - Input", "Primary Sound Capture Driver"); Sound Mapper appended last to candidates only when
  `sys.platform == "win32"` (audio_devices.py:350); WASAPI `auto_convert`; journal lease via `msvcrt.locking`;
  WAV `bext`/`LIST INFO` exist for Explorer's "Media created" column; loopback devices "[Loopback]" (PortAudio WASAPI
  loopback builds) excluded.
- Linux: ALSA "(hw:X,Y)" suffix stripped for display and matching; `fcntl.flock`; `PASTE_CHECK_TIMEOUT_S` 0.5 s exists for
  hyprctl/X11 focus queries (dictation_pipeline.py:474-476).
- macOS: Core Audio default host API; capture delay picker uses `ui_qt/snap/macos` (non-activating panel, all spaces,
  parked level). `parakeet_mlx` language branch is macOS-only (drop).
- Wayland/Hyprland: no code in this area beyond the paste-check timeout.

## Engine-specific code

Drop (non-Parakeet engines):
- `dictation_language.py`: `local_whisper`/`api` Whisper code lists, `_whisper_is_english_only`, Moonshine,
  Parakeet Unified and Parakeet MLX reasons (lines 65-74, 79-84, 98-105).
- `streaming_transcriber.py`: `NativePreviewLedger` Moonshine `id`/`start` line handling; `transcribe_cancelable`
  faster-whisper shape `(segments, info)` and `beam_size/vad_filter` arguments (adapt to `Recognizer::transcribe`).
- `config.STREAMING_PREVIEW_BACKENDS`/`INCREMENTAL_DICTATION_BACKENDS` entries other than parakeet (and nemotron, undecided).
- The worker-process plumbing behind `backend.decode_window`, `transcribe_windows`, `generation`, `should_cancel`,
  `is_transcribing` (another area); FioriVoce has `EngineWorker` + `Recognizer` in process.

Undecided backends:
- Nemotron Streaming: `NativeStreamingTranscriber`, `FinishedStream`, `stream_final_text`, `keep_stream_final`. Would map to
  a NeMo `streaming_recognize` session (Parakeet TDT rejects it, per CLAUDE.md).
- OpenAI API: `split_audio_file`, `_chunk_boundary`, `_find_best_silence`, `_moving_average`, `combine_transcriptions`,
  `check_file_size`, `needs_splitting`/`over_upload_limit`.
- Remote computer engine: `note_remote_runtime`, the `remote` branches of `accepted_languages`/`engine_language`.

Must adapt to in-process Parakeet:
- Window preview: decode on `EngineWorker`'s thread through queued signals (never GUI thread); preview windows and the
  final decode share one Recognizer and one thread, so the "stop never decodes" and lazy `finalize_preview` design is
  even more important (it frees the engine for the final pass immediately).
- Incremental dictation: the resampler and window splitter become FioriVoce's own C++ code used by both the file path and
  the live path, so bit identity holds by construction; keep the CRC/fingerprint verification anyway (cheap insurance).
  Parakeet's languages: en, ru, es, fr, pt (+ auto) from `services/local_asr/languages.py:12`.
- `prepare_preview_audio` produces float32 16 kHz; FioriVoce's engine API takes what `wav.h`'s `AudioBuffer` holds:
  align on one sample type.

## Dependencies on other areas

Calls out:
- `config` (a01 app shell): constants and `user_data_path`.
- `services.settings` (settings area): `SettingsKey`, `settings_manager.load_all_settings/mutate_settings/save_setting`,
  `MeetingLanguage`, resolver functions, `WALKTHROUGH_CAPTURE_DELAYS`, `compose_transcript_cleanup_prompt`.
- `services.diagnostics.record_metrics` (a01).
- `services.local_asr.audio` (`windows`, `WindowSplitter`, `resampler`, `MAX_SAMPLES`, `SAMPLE_RATE`),
  `services.local_asr.languages`, `services.local_asr.catalog.MODELS` (`stream_final`), `transcriber.optional_backend.LocalSpeechBackend` (engine area).
- `services.format_utils.format_audio_duration`.
- Personalisation areas: `app_styles`, `cleanup_prompts`, `dictionary`, `focus_context` (`FocusSnapshot`, `get_service().request/identity_now`, `same_target`, `join_with_context`, `join_html_with_context`, `prompt_block`), `snippets` (`SnippetPlan`, `plan_expansion`, `expand`, `prompt_guard`), `recognition_context.RecognitionContext`, `cleanup_profiles.compose_profile_prompt`, `batch_upload.compose_batch_cleanup_prompt`, `automation.catalog.user_started`.
- `services.database.db`, `services.models` (database area).
- UI: `ui_qt.utils.restyle.set_style_property`, `ui_qt.widgets.wrapped_label.WrappedLabel`, `ui_qt.utils.palette.current_palette`, `ui_qt.snap.{chrome,icons,macos,affinity}`.

Called in by:
- `services/application_controller.py` (creates `AudioRecorder`, sets `error_callback`/`device_switch_callback`, stops playback), `services/runtime/transcription.py` (save, wait, acknowledge, recover, pipeline stages, incremental), `runtime/streaming.py` (preview), `runtime/hotkeys.py`, `runtime/walkthrough.py` (pause, durations, cancel), `runtime/meeting.py` (`stop_playback`, `load_priority`), `runtime/command.py`, `actions/runtime.py`, `automation/filters.py`.
- `meeting/capture/devices.py` (re-exports audio_devices), `meeting/capture/spool.py`, `meeting/asr/audio.py`, `meeting/diarize/clustering.py` (`fft_resample`), `meeting/persist/repository.py` (`count_features`).
- UI: `settings_microphones.py` (device list, names, playback test), `settings_dialog.py`, `settings_cleanup_rules.py`, `settings_home.py`, `upload_file_tab.py` (preview), `history_playback.py`, `waveform_overlay.py` (language chip), `language_menu.py`, `local_engine_controls.py`, `ui_controller.py`, `guided_setup.py` (MicrophoneCheck), `walkthrough_tab.py` and `snap/toolbar.py` (CaptureDelay), `omarchy_controls.py`.
- `transcriber/openai_backend.py` (splitting), `services/release_checks.py`.

## Tests

| Test file | Pins |
|---|---|
| `tests/test_recorder.py` | start/stop/duplicate start, open failure, has/clear/duration, save refusals, partial save needs `allow_incomplete`, storage error keeps journal, stamp failure keeps WAV, single tagged write, level callback, journal-to-WAV streaming |
| `tests/test_recorder_postroll.py` | every PostRollGate branch (quiet window, short pause, continued speech, noise only, loud room, disabled, < 500 ms, lead-in, no audio, near-digital-silence floor), repeat stop, cleanup not waiting, fresh gate, `block_level_db`, percentiles; pause semantics |
| `tests/test_recording_journal.py` | lease skip and idempotent recovery, kill recovery (subprocess), disk full reported once, slow disk never blocks enqueue, finish wake, drain-before-finish, cancel removes journal, disconnect without another mic, previous output kept, partial write prefix, timed-out cancel cleanup, canceled never recovered, stale writer error, stalled cancel checkpoint, stop wait covers final checkpoint |
| `tests/test_personalize_s7_recorder.py` | candidate fallback, WASAPI conversion, no driver query without ranking, "No audio device available", mid-recording switch, gap ordering, finished-stream vs own close, stall detection, overflow window rules, no switch after stop, cancel during switch, mapper fallback, from_settings order and migration, listing failure fallback |
| `tests/test_personalize_fix_f3_lost_mic_close.py`, `test_personalize_fix_f3_legacy_mic.py` | hung close still ends recording; legacy migration retries and is off the calling thread |
| `tests/test_personalize_s7_audio_devices.py` | loopback skip and MME name repair, matching ladder (parametrized), ALSA suffix, conservative matching, candidate order, format check, Sound Mapper Windows only, meeting lookup `check=False`, legacy migration, stale index, PortAudio unavailable, cleaning without writing, refresh waits for streams, `display_name`/`short_name` table, `device_key` |
| `tests/test_wav_metadata.py` | tag round trip, PCM preserved, odd padding, buffered writer, rollback on failure, > 4 GiB refusal, `is_mono_pcm16` table |
| `tests/test_audio_processor.py`, `tests/test_chunking.py` | header-only preview, stereo channels, chunk estimate, duration fallback, error wording, splitting exactness, upload limit with overlap and header, streaming bounded memory, exact-size no overlap tail, silence boundary keeps samples, min-duration advance, cancel cleanup, format variety, `_moving_average` vs `np.convolve`, split points unchanged |
| `tests/test_resample.py` | `fft_resample` length/dtype, tones, amplitude, aliasing, identity, odd lengths |
| `tests/test_streaming_transcriber.py`, `tests/test_preview_lazy_tail.py` | window decode, unloaded engine skip, seam join table (`test_overlap_join_is_bounded_...`), finalize equality with the old drain, late output suppression, cancel/restart, native push intervals, ledger semantics, runtime stop/cancel wiring |
| `tests/test_incremental_dictation.py`, `tests/test_personalize_s4_incremental.py` | splitter independence from framing, bit identity, fall-back reasons (parametrized), cancel paths, ineligible engines, recognition context matching |
| `tests/test_cancel_after_stop.py` | cancel during post-roll transcribes nothing; recorder cancel keeps nothing after; native preview recordings take the file path |
| `tests/test_personalize_s8_language.py` | accepted languages per engine, cleaning, cycle, set_chosen/set_active, labels |
| `tests/test_personalize_foundation_pipeline.py` (+ many `test_personalize_*`/`test_automation_*` that use `DictationJob`) | prompt block order, failures left out, dictionary/snippets scope, focus request rules, caret join, paste target, history fields |
| `tests/test_personalize_s9_stats.py`, `tests/test_stats_features.py`, `test_personalize_fix_f5_stats_spoken.py` | word counting (CJK), words_edited table, local time, counting rules, streaks, pace, time saved, month bars, activity, top apps, milestones, listeners, backfill once, reset, features, meetings counted once, schema v17 -> v18 migration, Stats window rows |
| `tests/test_guided_setup.py` | MicrophoneCheck within guided setup |
| `tests/test_walkthrough_runtime.py`, `test_walkthrough_library.py` | recorder pause/duration use by walkthroughs, CaptureDelay usage |

Golden-vector candidates (pure, replayable from Python into C++): `audio_devices.display_name/short_name/match_entry/normalize_priority`
(with a fake device list), `recorder.block_level_db` + `PostRollGate` (feed level/frame sequences, compare
`threshold_db`, end block index), `wav_metadata.append_origination` output bytes for a fixed date, `streaming_transcriber.append_preview_text`
and `fft_resample`, `audio_processor._moving_average` and `_find_split_points` / `_generate_time_based_splits`,
`local_asr.audio.split_point`/`WindowSplitter` windows, `dictation_stats.words/count_words/words_edited/next_milestone` and
`load_summary` over a fixed row set and `today`, `dictation_language` functions over settings dicts.

## Porting notes

Suggested decomposition:
- Qt-free core (`src/engine` or a new `src/audio` library, namespace `fv`): `audio_devices` (PortAudio wrapper behind an
  interface so tests use a fake), `Recorder` (callback -> lock-free ring -> journal writer thread; watcher thread;
  `PostRollGate`), `RecordingJournal` + recovery, `WavWriter` + origination tags, `AudioPlayer`, FFmpeg-based
  `AudioFileReader` (preview, decode, 16 kHz resample, `WindowSplitter`), `Splitter` (if OpenAI kept),
  `PreviewJoiner` (`append_preview_text`), `DictationLanguage`, `StatsCalculator` (pure over rows) with the SQL in the
  app layer or a storage module. Keep UTF-8 `std::string` and `path_from_utf8`.
- Qt layer (`src/app`): `StreamingPreview` controller (receives blocks, posts windows to `EngineWorker` by queued signal),
  `IncrementalDictation` (same), dictation pipeline (depends on personalisation areas; port when those exist),
  `MicrophoneCheck`, `CaptureDelay`, Stats window (other area) fed by `StatsSummary`.

Order: 1) WAV writer + metadata, journal + recovery (pure, golden tests); 2) device listing/matching/ranking;
3) Recorder with post-roll, pause, cancel, switch (fake PortAudio tests ported from the Python suite); 4) level meter +
microphone check; 5) window preview on EngineWorker; 6) incremental dictation; 7) language choices; 8) audio file
preview/decoding (FFmpeg); 9) player; 10) stats; 11) pipeline stages as their dependencies land; 12) capture delay with the
walkthrough area; 13) undecided: splitting, native streaming.

Risks:
- Callback thread discipline: PortAudio callback must not allocate, lock contended mutexes or log; Python got away with a
  lock because of the GIL. Use a pre-allocated SPSC ring and atomics; overflow behaviour of the 512-block queue must stay.
- Device identity must match OpenWhisper's PortAudio names exactly or saved `audio_input_priority` stops matching; Qt
  Multimedia would break this.
- `refresh_portaudio` (Pa_Terminate/Pa_Initialize) invalidates everything; the `portaudio_lock` discipline must be kept.
- Bit-exact incremental decoding relies on both paths sharing one resampler; using FFmpeg's swr for the file path and
  anything else live would silently fall back every time (correct but slow).
- `difflib.SequenceMatcher` and Python `\w`/CJK regex semantics must be reproduced for stats to match.
- Stats timestamps mix naive-local and aware-UTC strings; parse both exactly as `local_time`/`_utc`.

Rough size: ~5,500 lines of C++ (recorder+journal+devices ~1,900; WAV/metadata ~300; player ~250; file reader/splitter
~600; preview + incremental ~900; language ~200; pipeline ~500; stats ~650; widgets ~350), plus ~2,500 lines of tests.

Native gains: lock-free capture path (no GIL stalls, so input overflows should largely disappear); level meter and
post-roll gate computed with no allocation; one resampler for live and file paths removes the 16 kHz re-decode at stop;
the journal could use one preallocated file with `FILE_FLAG_WRITE_THROUGH` checkpoints; stats summary computed in
milliseconds without SQLAlchemy object loading; startup migration and device listing off the GUI thread are cheap.
