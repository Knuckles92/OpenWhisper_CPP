# Runtime services (services/runtime): transcription, streaming, hotkeys, models, meeting, remote, updater, walkthrough glue

Source: `D:\coding\whisper_local\services\runtime\` (11 files, ~9.6k lines). All paths below are
relative to that folder unless prefixed. Line numbers are from the checkout read for this map.

## What the user gets

This area is the orchestration layer between the Qt window/tray/hotkeys and everything that does
work. It has no widgets of its own. What the user experiences through it:

- **Dictation job lifecycle**: press the record shortcut (toggle or push-and-hold), see
  "Recording...", stop, see "Processing..." / "Transcribing..." / "Cleaning up...", and get the
  words pasted at the caret (or copied), saved to History, with exact status strings for every
  outcome (pasted, copy failed, paste failed, no speech, canceled, filter handled it, ...).
- **One job at a time**: a single "job slot" guards recording, uploads, retranscriptions,
  rewrites, walkthroughs. Every refusal has its own status line and a notice near the pointer.
- **Cancel** that works in every phase (recording, post-roll, WAV save, decode, AI cleanup,
  selection read, write-up) and is quiet on repeat.
- **Failure safety**: a failed dictation's audio is kept in History with "Retry transcription";
  other failures keep audio in Recordings; interrupted recordings are recovered at startup.
- **Uploads**: single file and multi-file batch (serial, optional "combine" into one transcript,
  per-file errors, progress, cancel keeps finished files).
- **Command Mode** (spoken instruction rewrites the selection or writes at the cursor) and
  **Transforms** (saved instruction rewrites the selection), with careful selection reading.
- **Actions / spoken routing glue**: action recordings, route-by-voice, text filters, action
  shaping, API-run actions (`run_action_text`).
- **Push-and-hold with a hands-free latch** (short tap then second press within 400 ms latches),
  keyboard-hook watchdog (sleep/resume rehook, periodic 5 min refresh that waits for held keys).
- **Live preview** (streaming partial text in an overlay while recording) and its "empty final
  pass falls back to preview text, not saved" behavior.
- **Model downloads / consent**, **component installs** (runtimes, GPU), **GPU fallback offer**.
- **Remote engine** client link watch with backoff, host-side engine switching for paired PCs.
- **Updater** scheduling (check 8 s after launch, then by interval; download with cancel tokens).
- **Meeting Mode** runtime: start/pause/end, consent round-trip, dashboard URLs, finalization
  checklist card, retry, background finishing, past meetings, crash recovery.
- **Walkthrough** runtime: record while taking screenshots, snap modes, countdown, Auto Mode
  (click-triggered screenshots on Windows/macOS), board edits, write-up, copy/paste delivery.

## Feature inventory

Disposition key: **port**; **drop** (engine-specific); **undecided** (Nemotron / cloud API /
remote-computer backends, not decided); **other-platform** (only macOS/Linux; OpenWhisper_CPP is
Windows-first but keep the seams).

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A04-01 | Single job slot | One job at a time (recording result, upload, batch, retranscribe, rewrite, walkthrough). Busy status: "A transcription is already in progress — wait before {action}" where action is e.g. "starting another recording", "processing this recording", "re-transcribing audio", "uploading audio", "rewriting text", "pasting", "recovering a walkthrough". During host update: "OpenWhisper is preparing an update. Try again shortly." | transcription.py:`_claim_job` 286, `_report_busy` 417 | port |
| A04-02 | Recording start gates | In order: backup → "Wait for backup or restore preparation to finish"; host update notice; meeting → "Meeting Mode is active — end the meeting to use dictation"; engine readiness message (controller); busy (pointer notice); already recording → silent false | transcription.py:`_can_start_recording` 650 | port |
| A04-03 | Listening status/overlay | Command: overlay COMMAND_LISTENING, "Command Mode · Listening..."; action: "{action.name} · Listening..."; walkthrough: "Walkthrough · Recording..."; plain: "Recording..." or "Recording · {profile}..." | transcription.py:`_show_listening` 701 | port |
| A04-04 | Recording destination label | Overlay label: "Walkthrough", "Agent · {agent name}" / "Agent" for agent actions, else "" | transcription.py:`_recording_destination` 83 | port |
| A04-05 | Profile / action recording format snapshot | Profile and its settings snapshot at start; edits during recording don't apply. "That action no longer exists", "Cleanup profile no longer exists" | transcription.py:`_recording_format` 678, 586-599 | port |
| A04-06 | Route by voice on dictation | Plain dictation gets route "auto" when voice routing enabled and "on dictation" set | transcription.py:`_dictation_route` 533 | port |
| A04-07 | Stop playback before recording | History audio playback stopped before capture opens (also before meeting) | transcription.py:561-565; meeting.py:897-903 | port |
| A04-08 | Clipboard snapshot prefetch | DICTATION with auto_paste and restore_clipboard: snapshot user clipboard at start, discarded on any non-paste outcome | transcription.py:624-628 | port |
| A04-09 | Start failure | "Failed to start recording: {reason}" (default reason "Could not open the audio stream"), overlay NONE | transcription.py:`_report_start_failure` 717 | port |
| A04-10 | Stop → post-roll off Qt | Stop shows overlay PROCESSING + "Processing..."; claims slot before post-roll; post-roll wait + WAV write on executor. Stop after cancel ignored. "Failed to stop recording" | transcription.py:`_stop_recording` 733, `finish_recording_job` 798 | port |
| A04-11 | Saved WAV validation | "Recording did not finish stopping; captured audio was kept for recovery." / "No audio data recorded" / "Failed to save audio file; recovery copy kept" / "Audio file not created" / "Audio file is empty or corrupted" (< 100 bytes) | transcription.py:`_saved_recording_size` 864, `MIN_RECORDING_BYTES` 139 | port |
| A04-12 | Capture error mid-recording | Status "{message} — preserving captured audio"; preview canceled; job claimed and finished so audio is kept, never auto-pasted | transcription.py:`on_capture_error` 446 | port |
| A04-13 | Cancel | Quiet on repeat; recording → "Recording canceled" + CANCELING overlay; decoding → "Transcription canceled"; job working but engine idle → "Canceled". Returns whether anything was stopped. Delivered/saving job is not canceled | transcription.py:`_cancel` 998-1061 | port |
| A04-14 | Cancel checkpoints | Cancel checked after post-roll, before decode, after decode, before/after cleanup, between batch files, between walkthrough sections; raises "Transcription canceled" | transcription.py:`_raise_if_canceled` 2155, 826, 849 | port |
| A04-15 | Final-pass recognition context | Language/vocabulary re-read at final pass; job updated so History records the language used | transcription.py:`_with_final_recognition` 1646 | port |
| A04-16 | Empty result + preview fallback | Empty final text: shows preview text if any with status "No speech detected — showing the live preview (not saved)", else "No speech detected (empty after VAD)"; never saved/pasted; + " — recording kept in History" for retry | transcription.py:2186-2272, `_complete_preview_fallback` 2169 | port |
| A04-17 | AI cleanup of transcript | Dictionary/snippets always; cleanup if enabled or profile; status "Cleaning up..." or "Formatting · {profile}..." overlay CLEANING; failure falls back to raw; snippet placeholder loss falls back | transcription.py:`_maybe_cleanup_transcript` 1509 | port (cleanup provider = cloud/LLM, see A12) |
| A04-18 | Profile formatting failure notice | " — {profile} formatting failed ({reason}); using raw transcript" | transcription.py:`_delivery_notice` 2332 | port |
| A04-19 | Text filters | Before-cleanup and after-cleanup filters; a filter that cancels or empties words → saved but not delivered: "Not pasted — a text filter handled it" | transcription.py:`_complete_live` 1867, `_filtered` 1931 | port |
| A04-20 | Spoken routing | Words naming an action become an ACTION job ("Action · {name}" source). Route-by-voice hotkey notices: "No action matched — pasted instead", "Couldn't reach Fast judgments — pasted instead" | transcription.py:`_route` 1961, 2389-2398 | port |
| A04-21 | Action shaping | raw / profile / cleanup+transform. Notices "This action's transform no longer exists — sent your cleaned words", "{transform} couldn't run — sent your cleaned words" | transcription.py:`_shape_action` 1994 | port |
| A04-22 | Delivery: Scratchpad | Focused Scratchpad takes the text: "Ready (Added to Scratchpad)" | transcription.py:`_insert_into_scratchpad` 2483 | port |
| A04-23 | Delivery: paste/copy | auto_paste → stage+paste: "Ready (Pasted)", "Ready (Pasted; clipboard restore unavailable)", "Transcription complete (paste failed)", "Transcription complete (copy failed)"; copy only → "Ready"; macOS no Accessibility → "Copied to clipboard — press Cmd+V (enable Accessibility to auto-paste)" | transcription.py:`_apply_clipboard_and_paste` 2653, `_paste_transcript` 2718 | port (macOS branch other-platform) |
| A04-24 | Forced paste for Command/Transform | Pastes even with auto-paste off, only if target app unchanged; else "Rewrite copied — the app changed; paste it where you want" / "...couldn't confirm the app; ..."; pointer notice "Rewritten"/"Inserted" | transcription.py:2370-2400, `_copy_rewrite` 2498, `_notify_forced_outcome` 2512 | port |
| A04-25 | Upload/retry delivery | No clipboard: "Transcript ready to copy" (live/retry) or "Ready"; live recovered → notice "Transcript recovered. Ready to copy." | transcription.py:2305-2314 | port |
| A04-26 | History persistence off Qt | Paste first, then history on single persistence worker; acknowledges recording WAV once retained; errors "Transcript saved, but audio could not be retained; recovery copy kept", "History could not be saved: {exc}. Copy the transcript before closing." | transcription.py:`_queue_history` 2562, `_persist_history` 2574 | port |
| A04-27 | Failed recording kept in History | Dictation failure: entry kind failed_recording; status " — recording saved in History. Select Retry transcription."; notice "Transcription failed. Retry the recording from History." or "Reconnect failed. Audio saved in History." | transcription.py:`_save_failed_recording` 2957 | port |
| A04-28 | Preserve audio on other failures | " — audio saved in Recordings as {name}" / " — audio retention failed; original and recovery copy kept" / " — could not retain audio; recovery copy kept" | transcription.py:`_preserve_failed_audio` 2984 | port |
| A04-29 | Error status | "Error: {message}" (refusals shown bare); walkthrough adds kept-note; error dialog via `show_transcription_error` | transcription.py:`on_transcription_error` 2898 | port |
| A04-30 | Retranscribe / Retry from History | "Meeting Mode is active — end it before retranscribing"; "Error: Audio file not found"; retry of failed entry completes that entry instead of adding one | transcription.py:`retranscribe_audio` 1063 | port |
| A04-31 | Upload single file | "Meeting Mode is active — end it before uploading audio"; "Processing uploaded file..." | transcription.py:`upload_audio_file` 1150 | port |
| A04-32 | Batch upload | "No audio files to transcribe"; "Error: Audio file not found: {name}"; "Processing {n} uploaded files..."; per-file progress signals; combine mode raises on first file error; "All {n} files failed: {first error}"; "Canceled — kept {the transcript / k of n files}"; " without AI cleanup"; " — AI cleanup failed ({e}); showing raw text"; batch text "Error: {e}" per failed file | transcription.py:`upload_audio_files` 1187, `transcribe_batch` 1240, `on_batch_complete` 1403, helpers 93-135 | port |
| A04-33 | Large file split announce | Backend-provided `large_file_size_mb`: "Splitting large file ({x:.1f} MB)..." | transcription.py:`_announce_transcription` 1375 | undecided (OpenAI API only) |
| A04-34 | Remote reconnect during job | Up to 3 attempts (delays 0,1,2 s, 12 s cap): "Retrying connection ({n}/3)..."; "Remote engine unavailable after {n} reconnect attempt(s). {device_info}" | transcription.py:`_retry_remote_for_recording` 935 | undecided |
| A04-35 | Startup recording recovery | "Recovered {n} interrupted recording(s) in Recordings"; walkthrough variant "Recovered {n} walkthrough(s) — open Saved Walkthroughs to finish them" | transcription.py:`recover_recordings` 425 | port |
| A04-36 | Walkthrough job | Decoded section by section (no early decoding), each section cleaned separately; "Transcribing section {i} of {n}..."; "Writing the walkthrough..."; doc write failure keeps words | transcription.py:`_complete_walkthrough` 1700, `_clean_sections` 1827 | port |
| A04-37 | Walkthrough delivery & write-up | Copy format per setting; AI write-up off-slot: "Writing it up with {agent}...", "Stopping the write-up..."; copied statuses (see Modules) incl. lost screenshots suffix | transcription.py:`_deliver_walkthrough` 2402, `_copy_walkthrough` 2434, `_start_write_up` 1761 | port |
| A04-38 | Recover walkthrough | "Wait for the walkthrough being saved to finish before recovering one"; "Couldn't recover the walkthrough: {exc}"; "Recovering walkthrough..." | transcription.py:`recover_walkthrough` 1107 | port |
| A04-39 | Rewrite jobs | `begin_rewrite_job` (non-blocking capture lock), REWRITING overlay "Rewriting..."; `abandon_rewrite_job`; `submit_rewrite` | transcription.py:2778-2874 | port |
| A04-40 | Run action on text (API) | Errors: "There are no words to send.", "Meeting Mode is active; end the meeting first.", "No enabled action has that id.", "OpenWhisper isn't taking actions right now.", "OpenWhisper is busy with another recording."; status "{action}..." | transcription.py:`run_action_text` 2061 | port |
| A04-41 | Paste text now | Paste regardless of auto-paste; busy refusal | transcription.py:`paste_text_now` 2876 | port |
| A04-42 | Engine readiness errors | "The speech model isn't downloaded yet. Approve the download, then try again."; else backend device_info | transcription.py:`_require_backend_ready` 3044 | port (adapt) |
| A04-43 | Model/engine switch | "End the meeting before changing transcription models"; saves selection, reconfigures preview, reloads | transcription.py:`on_model_changed` 3000 | port (adapt; Whisper branches drop) |
| A04-44 | Automation events | recording.started/stopped/canceled, transcription.preview (live only), transcription.completed/failed (no_speech/canceled/refused/failed), history.saved | transcription.py passim | port |
| A04-45 | Diagnostics metrics | `stop_to_result_s`, `cancel_s` | transcription.py 2193, 393, 1053 | port |
| A04-46 | Host update preparation | Refuse "OpenWhisper is in use on this computer" if anything busy | transcription.py:`prepare_host_update` 308 | undecided (remote) |
| A04-47 | Command Mode hotkey | Toggle or push-and-hold; tap < 250 ms cancels; "Set up AI cleanup to use Command Mode"; "Finish the current recording before using Command Mode" | command.py:`key_pressed` 182, `key_released` 197, `_start` 218 | port |
| A04-48 | Selection read | UIA answer wins (even empty); refusals "OpenWhisper doesn't read password fields", "OpenWhisper doesn't read text in {app}"; never copy in terminals; wait for modifiers (0.8 s) else "Release the shortcut keys, then try again"; clipboard can't see → "OpenWhisper can't read selected text on this desktop"; copy-line editors' one-line copy counts as empty | command.py:`_read_selection` 289, `_looks_like_copied_line` 122 | port |
| A04-49 | Command completion | Empty instruction → refused (text_rewrite.NO_INSTRUCTION_MESSAGE); selection wait `SELECTION_WAIT_S`=0.8+0.7+0.4=1.9 s → "Couldn't read the selected text. Try again."; nothing selected & insert off → "Select text first"; status "Writing..." / "Rewriting..." | command.py:`complete_recording` 435 | port |
| A04-50 | Transforms | "That transform no longer exists"; "Set up AI cleanup to use transforms"; "Rewriting · {name}..."; "Select text to transform"; history source "Transform · {name}" | command.py:`run_transform` 335 | port |
| A04-51 | Hotkey setup | Defaults merged over saved; callbacks wired; display updated; "Hotkeys updated" | hotkeys.py:`setup_hotkeys` 276, `update_hotkeys` 526 | port |
| A04-52 | Push-and-hold + hands-free latch | Hold ≥ 250 ms stops; shorter tap cancels (latch off) or waits 400 ms for 2nd press → latched: "Hands-free · press {hotkey} to stop"; next press stops | hotkeys.py:`record_key_pressed` 372, `record_key_released` 407 | port |
| A04-53 | Trigger mode switch | "Push-and-hold recording enabled" / "Toggle recording enabled" | hotkeys.py:`set_recording_trigger_mode` 319 | port |
| A04-54 | Paste last original / paste walkthrough | Waits for modifiers up; "Didn't paste the original: the shortcut keys were still held"; "Walkthrough not pasted: the shortcut keys are still held" | hotkeys.py 332-351 | port |
| A04-55 | Dynamic shortcuts | Profile, transform, enabled-action shortcuts registered; any conflict → skipped | hotkeys.py:`refresh_profile_hotkeys` 535 | port |
| A04-56 | Hook watchdog | 10 s tick; gap > 30 s → rehook; periodic rehook every 5 min (min 60 s apart), deferred while a hold is active | hotkeys.py 589-703 | port (Windows hook) |
| A04-57 | Focused-window hotkey filter | Qt event filter fallback for pynput backends | hotkeys.py:`ActiveWindowHotkeyFilter` 159 | other-platform |
| A04-58 | macOS Accessibility intro | Shown once when auto-paste on and not trusted | hotkeys.py:`_check_autopaste_permission` 504 | other-platform |
| A04-59 | STT enable/disable overlay | Status "STT Enabled"/"STT Disabled" toggles overlay state | hotkeys.py:`update_status_with_auto_hide` 709 | port |
| A04-60 | Omarchy (Hyprland) controls | Started on Wayland+Omarchy | hotkeys.py 310-317 | other-platform |
| A04-61 | Live preview setup | Enabled by `streaming_enabled`; window preview (chunk 3 s, overlap 0.75 s) sharing the loaded engine; native stream (0.75 s updates) for Nemotron/Parakeet Unified; dedicated tiny.en for Whisper | streaming.py:`_configure_streaming` 224, `_preview_backend` 246 | port (window over Parakeet); Nemotron native undecided; tiny.en drop |
| A04-62 | Preview unavailable status | "Live preview needs Local Whisper, Parakeet, Parakeet Unified, or Nemotron Streaming"; remote "...on {host}"; "Stop recording before changing streaming mode"; "Failed to reconfigure streaming" | streaming.py 29-50, 108-113, 244 | port (reword for OpenWhisper_CPP) |
| A04-63 | Preview session | Start feeds recorder blocks; skips when previous still finishing (no empty overlay); stop returns text; finalize decodes kept tail only for empty result; cancel hides overlay | streaming.py 125-219 | port |
| A04-64 | Audio level meter | Recorder level → `update_audio_levels([level]*20)` | streaming.py:`setup_audio_level_callback` 81 | port |
| A04-65 | HF consent & download | Consent dialog (once / always / open settings / cancel); "Model '{m}' unavailable — downloads disabled by HF_HUB_OFFLINE"; "...is unavailable — download declined"; "Downloading model '{m}'..."; "Whisper engine ready"/"Model failed to load"; throttled progress (≤10/s) | models.py 120-216, 312-354, 590-646 | port (adapt to Parakeet model files) |
| A04-66 | Batch model downloads | "Downloads are disabled by HF_HUB_OFFLINE"; "Another model request is in progress — try again when it finishes"; sequential; stop between models | models.py 218-250, 705 | port (adapt) |
| A04-67 | Model delete | "End the meeting before removing speech models"; "Model is in use — switch models first"; "A download for this model is in progress"; "Files are in use by another process"; "Model '{m}' deleted" | models.py 252-310 | port (adapt) |
| A04-68 | GPU fallback & "Use this GPU" offer | Device setting reverted to cpu; cause-specific messages; one-time offer | models.py 389-503 | drop (faster-whisper CUDA); idea may adapt to NeMo GPU runtime |
| A04-69 | Missing runtime prompt | Once per runtime per session; GPU runtime offered first | models.py:`_prompt_for_model_runtime` 559 | port (adapt: NeMo CPU vs GPU runtime) |
| A04-70 | Component install/remove | Throttled progress; "Installed and ready to use — no restart needed.", "Restart OpenWhisper to enable this component.", "Installation canceled.", "Installation failed: {e}", "Component removed", "Could not remove the component: {e}", "Finish the current speech job before installing/removing its runtime" | components.py | port (adapt to NeMo runtime downloads) |
| A04-71 | Remote client link watch | 1 s watch tick; reconnect backoff 2,5,15,30,60 s (last repeats) | remote.py 30-35, 333-456 | undecided |
| A04-72 | Remote model/runtime choice | "Switching {host} to {label}..."; "End the meeting before changing the engine"; "Finish recording before changing the engine"; "Finish the current activity before changing the remote runtime" | remote.py 128-174 | undecided |
| A04-73 | Host engine switch for clients | 10 s wait: "{hostname} didn't respond. Try again in a moment."; refusals "{name} is running a meeting...", "{name} is transcribing right now...", "{name} is loading its engine...", "{name} can't run {model}." | remote.py 178-263, 462-535 | undecided |
| A04-74 | Pairing/host/records events | History & Past Meetings refresh on record moves | remote.py 267-329 | undecided |
| A04-75 | Update check schedule | First check 8 s after start (launch check ignores interval), then by interval, never sooner than 1 h; offline failure shows ambient status | updater.py 65-140 | port (if updater kept; see A16) |
| A04-76 | Update download | Each attempt has its own cancel token; stale attempts discarded; "No installer is available.", "The download failed.", "The download was canceled.", "The update was canceled." | updater.py 144-232 | port |
| A04-77 | Update leftovers | Prune stale downloads, abandoned transactions, setup backup | updater.py:`prune_leftovers` 236 | port (macOS part other-platform) |
| A04-78 | Meeting start | Gates: update, backup, finalizing ("Final insights are still being prepared."), busy ("A meeting is already in progress"), engine loading, recording ("Finish recording or transcription before starting a meeting"); cloud consent round-trip; "Starting meeting..." / "Starting demo meeting..."; "Meeting in progress" | meeting.py:`_begin_start` 758, `_launch` 882, `_start_worker` 931 | port |
| A04-79 | Meeting start failure | "Could not start the meeting: {msg}" (Linux system-audio special text); state failed | meeting.py:`_start_failed` 987 | port |
| A04-80 | Pause/resume/end | "Meeting paused", "Meeting in progress", "Ending meeting — finishing transcription..."; ended messages | meeting.py 1144-1195, `_ended_message` 119 | port |
| A04-81 | Finalization checklist card | Hydrated on startup from newest meeting unless deferred; Keep for later; background handoff with "{n} meeting(s) finishing in the background. Results appear in Past Meetings." | meeting.py 336-695 | port |
| A04-82 | Retry post-meeting steps | "Retrying post-meeting steps…"; "Post-meeting steps are already running for this meeting."; "Post-meeting retry failed."; speaker re-run gate | meeting.py 1197-1494 | port |
| A04-83 | Dashboards | Open live dashboard (browser); past meeting via archive server; `?history={id}&view=report`; tokens never logged | meeting.py 1496-1680, `redact_meeting_url` 74 | port |
| A04-84 | Guest link, AI insights toggle | "No guest link available yet"; "AI insights on/off"; "AI insights stay off" | meeting.py 1682-1725 | port |
| A04-85 | Meeting recovery | Startup scan on worker; dialog finalize/discard; follow-up retry step | meeting.py 285-334, 1727-1797, 1958-1986 | port |
| A04-86 | Past meetings mgmt | Copy transcript ("No transcript to copy"), delete (keep/purge audio), clear ("Meetings cleared"/"Meetings and recordings cleared") | meeting.py 1799-1956 | port |
| A04-87 | Engine event bridge | status/server_started/error/background_ready/asr_released/ended/intelligence; voice_feedback & segments dashboard-only; stale retry events dropped | meeting.py 1988-2205 | port |
| A04-88 | Meeting model lease | Release dictation engine before meeting loads its own; restore on asr_released/background_ready/failure | meeting.py 966-973, 1792 | port (adapt: shared Parakeet recognizer) |
| A04-89 | Meeting shutdown | Bounded 5 s wait, logs stuck stack | meeting.py:`cleanup` 2325, `_shutdown_engine` 2229 | port |
| A04-90 | Walkthrough toggle | "Finish the recording in progress first"; "Wait for the last walkthrough's write-up, or cancel it"; overlay kept out of screenshots | walkthrough.py:`toggle` 134, `_begin` 144 | port |
| A04-91 | Screenshot shortcut | In walkthrough → shot; outside → region annotate & copy (image + file + path): "Screenshot copied", "...(its file couldn't be saved)", "...— the walkthrough had already stopped", "Couldn't copy the screenshot" | walkthrough.py:`snap` 520, `_copy_screenshot` 676 | port |
| A04-92 | Snap modes & countdown | region / window / screen / delayed (3 s countdown "Snapping in {n}…"); tray delay 300 ms; press again = keep / take now | walkthrough.py 548-622 | port |
| A04-93 | Shots on board | "Screenshot {n} added"; overlay label "Walkthrough · {n} shot(s)"; "Couldn't save the screenshot ({e}) — the walkthrough keeps recording"; undo/move/remove; pause "Walkthrough · Paused"; hold for note | walkthrough.py 635-753, `shots_label` 59 | port |
| A04-94 | Auto Mode | Windows/macOS: mouse click → screenshot after 350 ms settle or capture delay; live drawing marks with F8 keep / F9 clear unless bound; "Auto Mode couldn't watch for clicks. You can still use Snap." | walkthrough.py 231-499 | port (Windows); macOS other-platform |
| A04-95 | Walkthrough end | Countdown/tray timer stopped; screenshot being drawn kept on stop, dropped on cancel | walkthrough.py:`on_recording_state_changed` 757 | port |

### Added by check

Corrections to rows above (source re-read):
- A04-93: two different save-failure strings exist. The writer thread (session `on_save_failed`, off the Qt
  thread, status line only) says "Screenshot {n} couldn't be saved — the walkthrough keeps recording"
  (walkthrough.py:157); the Qt-side `_keep_walkthrough_shot` says "Couldn't save the screenshot ({reason}) —
  the walkthrough keeps recording" (644). A failed snap with no reason says "Couldn't take a screenshot" (666).
- A04-42: engine-not-ready has three outcomes, not one: an unavailable remote engine calls `remote.retry_now()`
  (undecided); a missing model starts the consent/download flow AND raises `EngineNotReadyError` (message shown
  bare, no "Error:" prefix); a model that failed to load for another reason triggers a reload and shows the
  device info. Any *other* exception while submitting a job shows "Failed to process audio: {exc}"
  (`_start_failure_message`, transcription.py:67; used by stop, retry, upload, batch, recovery).
- A04-51 hooks up 17 callbacks, so the port's shortcut table needs all of them: record toggle, record
  press/release (push-and-hold), command press/release, cancel, minimize to tray, meeting toggle, Scratchpad
  toggle, cycle language, paste last original, route-by-voice record (`toggle_route_recording`), walkthrough,
  screenshot, paste walkthrough, status-update (x2). `hotkeys.py:setup_hotkeys` 276-317.

New behaviour not in the table:

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A04-96 | Meeting hotkey flow | Optional global `meeting` shortcut: active -> ends the meeting; not claimed -> asks the UI for the platform-readiness gate (`ensure_meeting_start_readiness` returns a system-audio policy or None to abort), then starts with that policy | meeting.py:`toggle_from_hotkey` 226, `_on_platform_ack_requested` 233 | port |
| A04-97 | Meeting start options | Start carries `system_audio_policy` (default "auto") and a free-text `intent`; `start_demo_meeting` is a separate entry ("Demo Planning Sync" title, speaker backend forced local, no audio-upload consent, no end redecode, no remote ASR); demo status "Demo meeting loaded — End to test cleanup and the report" | meeting.py:`start_meeting` 723, `start_demo_meeting` 737, `_build_options` 1074, `_announce_started` 1050 | port |
| A04-98 | Meeting start gate texts | Besides A04-78: "OpenWhisper is preparing an update. Try again shortly.", "Wait for backup or restore preparation to finish", and the engine-loading message are `meeting_status_update`; "Finish recording or transcription before starting a meeting" goes to the main status line and also blocks while the backend is mid-transcription. Re-checked inside the lock after the leftover finalization card is filed | meeting.py:758-821 | port |
| A04-99 | Meeting start details | Playback stopped before capture; `meeting_cloud_last_enabled` saved on every launch; any retained finalization state cleared; status `{"status":"starting"}` published; background engines holding the port force `server_port=0`; start waits on dashboard URL or fails "Meeting dashboard did not provide an address."; "Linux system audio" failure becomes "System audio became unavailable while starting. Retry detection or continue microphone-only."; failed start publishes `{active False, status "failed"}` and restores the dictation engine | meeting.py:882-1050, `_require_dashboard_address` 93 | port |
| A04-100 | Meeting error strings | "Could not pause the meeting: {e}", "Could not resume the meeting: {e}", "Could not end the meeting: {e}", "Could not change AI insights: {e}", "Could not move this meeting to the background", "Could not save this meeting for later", "Could not delete the meeting: {e}", "Could not clear past meetings: {e}", "Could not discard the meeting: {e}", "Could not finalize the meeting: {e}", "Could not copy the transcript: {e}", "Could not load the past meeting: {e}", "Could not open the past meeting: {e}", "Could not create a meeting dashboard link", "The live meeting dashboard is unavailable", "That meeting no longer exists", "Post-meeting retry failed: {e}" (runtime retry failure text) | meeting.py passim | port |
| A04-101 | Ended-meeting status line | "Meeting ended — preparing final insights…" (finalizing); "Meeting ended — audio capture was incomplete; partial audio was kept in Meeting History" (capture_error); "Meeting ended — transcription recovery needed" (status `needs_recovery`); "Meeting ended". Engine events also surface: engine `note` or status as status line, "Meeting intelligence online" / "Meeting intelligence offline — transcript-only", finalization `message` ("Preparing final insights…" while running; terminal message shown as non-modal status, never an error dialog) | meeting.py:`_ended_message` 119, `_on_status_event` 2082, `_on_intelligence_event` 2158, `_apply_finalization` 2167 | port |
| A04-102 | Keep for later / leftover card | "Keep for later" refuses while finalizing ("Final insights are still being prepared.") or while a meeting runs ("A meeting is already in progress"); on success "Meeting saved. Open it from Past Meetings." (completed/disabled) or "Meeting saved for later. Open it from Past Meetings when you want to continue."; deferral persisted so the card stays hidden across restarts; a new start files the leftover card as deferred first | meeting.py:`defer_finalization_card` 634, `_persist_card_deferred` 430, `_file_leftover_card` 683 | port |
| A04-103 | Background finishing outcomes | `continue_in_background` only when finalizing, `_background_ready` (ASR model freed), no start/active meeting. Result messages: "Background meeting finished. Open it from Past Meetings." or "Background meeting needs attention. Open it from Past Meetings." (step failed, not deferred, or exception); background meetings can be opened (their own dashboard URL) and "This meeting is still finishing in the background." is the refusal for retry/delete/open | meeting.py:557-632, 1280, 1603, 1845 | port |
| A04-104 | Meeting busy / in-use queries | `meeting_busy(id)` (live, finishing, background, retrying, or a re-run claim from `meeting.refinalize.is_running`) gates record sync and deletion ("Wait for this meeting's post-meeting steps to finish."); `meeting_in_use(id)` also counts an open archive dashboard. `retitle_saved_meeting(id, title)` (MCP/API title edit) raises `ControlError("record_busy: Wait for the meeting's current work to finish.")` or `"update_rejected: The meeting title could not be changed."`, edits through the running dashboard server when one exists (so open dashboards stay consistent, others refreshed), else `rename_meeting` in the repository | meeting.py:496-555, 1829-1860 | port |
| A04-105 | Recovery scan errors | Startup also runs "restore last finalization" worker. Scan failure shows an error with retry: "Recovery could not check saved meetings. Retry the scan; your meeting files have not been removed." (+ " Details: {e}"); recovery dialog offers finalize/discard; recovered finalize results: "Interrupted meeting finalized", "Meeting transcription finished; captured audio remains incomplete. Review the partial meeting in History." (status failed), "Could not finalize the meeting — transcription may still be pending", "Interrupted meeting discarded"; follow-up step after recovery: redecode if a spool exists and end-redecode on, else (cloud on) polish, else consolidation | meeting.py:267-334, 1727-1795, 1958-1986 | port |
| A04-106 | Past meeting opening | "Opening a past meeting...", "Opening meeting report" (view=report), "Finish the current meeting first." (select/open/delete while one runs), "No meeting dashboard is available. Start a meeting or open ...", "No meeting report is available. Select a meeting from Past Meetings." Opening shuts the previous ended engine/archive server first; the archive dashboard is started on demand and reused | meeting.py:1496-1680 | port |
| A04-107 | Retry gates | Retry refused by backup; "speaker_id" step refused with `speaker_rerun_reason`; local model made available first for `failed`/`redecode`; claim runs only when no start/active/finalizing; generation counter and `FinalizationClaim` per retry; agent writes allowed on the live engine for the run and revoked after; busy -> "Post-meeting steps are already running for this meeting."; progress snapshots published only while the claim is active | meeting.py:1203-1450 | port |
| A04-108 | Model/engine cleanup glue | `ModelRuntime.on_hf_policy_changed("always")` fetches a missing selected model at once; `forget_runtime_prompts()` re-arms the missing-runtime prompt when engine selection changes; fetch-only downloads (Settings > Downloads) never change the selection or touch the loaded engine, unless the file is the missing selected one (then the engine is revived, but not during a meeting); downloads honor the app-wide shutdown cancel token; signals `engine_busy_changed(bool)` and `device_info_update(str, bool)` frame loads | models.py:96-216, 548-704 | port (adapt) |
| A04-109 | Model status strings | "Downloading model '{m}'...", "Downloading model '{m}' from Hugging Face..." (fetch-only), "Model '{m}' downloaded", "Model '{m}' is unavailable" (access denied), "Model '{m}' failed to load (details in openwhisper.log).", batch per-model failures use `describe_model_download_error`; batch stop logs "Batch download stopped after {n} of {m}" and emits `batch_download_finished(succeeded, total)` | models.py:624-752 | port (adapt) |
| A04-110 | Command/Transform rewrite context | Every rewrite reconfigures the shared cleanup client from settings, adds the personal dictionary block, and (when app-context "read text" is on) a caret-context block captured within `CONTEXT_CAPTURE_DEADLINE_S` (0.4 s); the rewrite result's `raw_text` is the original selection; "Writing..." vs "Rewriting..." status; transforms also run from a dynamic shortcut with `source="hotkey"` so programs cannot trigger them; model refusals (`text_rewrite.REFUSALS`) show as bare messages | command.py:387-466 | port |
| A04-111 | Selection privacy for action jobs | An action job reads the selection as context only when the action has `include_selection` and the job was started by the user (never by API/MCP) | transcription.py:`_reads_selection` 73, 598, 2099 | port |
| A04-112 | Batch context & cleanup | Per-file cleanup gets the user's batch description (`request.batch_context(item)`); combined batch cleanup runs once with `TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S` (120 s); cleanup unavailable -> "cleanup unavailable" (silent fallback to raw); cleanup failure reasons feed the " — AI cleanup failed ({reason}); showing raw text" suffix and "{profile} formatting failed ({reason}); using raw transcript"; snippet loss -> "snippet placeholder lost"; the dictionary applies even with cleanup off and to uploads; rich (HTML) paste text is kept from cleanup (`_delivery_html`) | transcription.py:1509-1623, 1240-1360 | port |
| A04-113 | Startup recovery also touches walkthroughs | `recover_recordings` additionally attaches recovered audio to unfinished walkthroughs and prunes the walkthrough library, before choosing which message to show (walkthrough count wins over recording count) | transcription.py:425-444 | port |
| A04-114 | Stats entry extras | History/Stats entry carries `spoken_text`, `stats_counts` (`retranscription: True`, action `target` name, walkthrough `shots`); empty walkthrough result dismisses the walkthrough panel ("Writing it up..." column); a walkthrough with neither words nor screenshots ends as no-speech | transcription.py:2536-2550, 2250-2262, 1740-1746 | port |
| A04-115 | Pointer-notice policy | `_notify(text, done)` shows the near-pointer notice from any thread via `notice_requested`. Failures show a notice only for refusals and non-dictation jobs (never after a cancel); a plain dictation failure uses the status line + dialog only | transcription.py:2520, 2898-2930 | port |
| A04-116 | Hotkey capture suspend | `set_capture_suspended(bool)` mutes global shortcuts while the Settings dialog records a new one | hotkeys.py:583 | port |
| A04-117 | Walkthrough start gates | Refuses during a meeting (`_refuse_dictation_during_meeting`), during a write-up (WAIT_FOR_WRITE_UP), and when the engine is not ready (readiness message); pause only takes effect if `recorder.pause()` succeeds; snap source "ui"/"tray"/"hotkey"; Snap-menu pick is saved as the new default mode and taken immediately; pause status "Walkthrough · Paused" / "Walkthrough · Recording..." | walkthrough.py:134-176, 520-560, 721-737 | port |
| A04-118 | Update check wording | Check runs on the job executor; failure text is the updater's user-facing message or "Could not check for updates."; automatic-check failures go to the ambient status line (`update_check_failure_status`), manual ones to the update dialog handler; an unreadable schedule disables auto-checks (logged); `stop()` cancels timer and any download | updater.py:65-140, 273 | port (if updater kept) |
| A04-119 | Remote strings (undecided) | "Couldn't switch models: {e}", "Updating the runtime on {host}...", host-side "Switching to {label} for {device}..." / "Switching to {label}...", `_reload_note` shown during reload | remote.py:153-260 | undecided |

## Modules

### `__init__.py`
Re-exports `HotkeyRuntime`, `MeetingRuntime`, `StreamingRuntime`, `TranscriptionRuntime`.

### `transcription.py` — `TranscriptionRuntime` (largest; ~3070 physical lines)

**Responsibility**: owns the single job slot and every job's lifecycle: recording start/stop/cancel,
upload, batch, retranscribe, walkthrough, rewrite jobs; final decode; cleanup/filters/routing/
shaping; delivery (paste/copy/scratchpad/action/walkthrough); history persistence; failure audio
retention. Not a QObject: reports via controller signals.

**Public surface** (called by controller/UI/other runtimes):
- Properties: `has_active_job`, `cancelable`, `current_job`, `recording_walkthrough`, `writing_up`,
  `canceling_recording` (bool attribute read by walkthrough runtime).
- Start/stop: `start_recording(profile_id="", mode=DICTATION, *, selection, action_id, source, route)`,
  `start_walkthrough(session, source)`, `stop_recording()`, `toggle_recording()`, `cancel() -> bool`.
- Jobs: `retranscribe_audio(path)`, `recover_walkthrough(folder)`, `upload_audio_file(path, duration)`,
  `upload_audio_files(BatchUploadRequest)`, `transcribe_batch` (worker), `begin_rewrite_job(job, source_name)`,
  `submit_rewrite(work)`, `abandon_rewrite_job(status)`, `run_action_text(action_id, text, source) -> job id`,
  `paste_text_now(text, job)`, `cancel_write_up()`.
- Qt slots wired by controller: `on_transcription_complete(text, raw, info)`, `on_transcription_error(msg)`,
  `on_batch_complete(result)`, `on_history_persisted(outcome)`, `on_walkthrough_written_up(result)`,
  `on_capture_error(msg)`, `on_model_changed(display_name)`.
- Others: `publish_preview(job, text, final)` (any thread), `report_backend_progress(msg, transcribing)`,
  `recover_recordings()` (startup worker), `begin_shutdown()`, `prepare_host_update()`.
- Exceptions: `EngineNotReadyError` (message is shown as is), `CommandRefused` (from command.py).
- Constants: `EMPTY_ASR_MESSAGE`, `REMOTE_RECOVERY_DELAYS_S=(0,1,2)`, `REMOTE_RECOVERY_WAIT_S=12`,
  `WALKTHROUGH_LABEL="Walkthrough"`, `MIN_RECORDING_BYTES=100`, `FILTER_CANCELED_MESSAGE`,
  `EMPTY_PREVIEW_FALLBACK_MESSAGE`.

**Threading**: `_capture_lock` (RLock) serializes start/stop/cancel/rewrite-claim; `_job_lock` guards slot
state (`_job_active`, `_active_job`, `_saving`, `_previewing`, `_walkthrough`, flags). Work runs on
`controller.executor` (2 workers), history on `controller.persistence_executor` (1 worker),
write-up on `controller.walkthrough_executor` (1 worker). Worker → Qt via controller signals
`transcription_completed(str, object, object)`, `transcription_failed(str)`, `batch_progress`,
`batch_item_finished`, `batch_completed`, `history_persisted(dict)`, `walkthrough_written_up`,
`status_update`, `overlay_state_update`, `large_file_detected`, `transcription_retry_progress`,
`notice_requested(str, bool)`, `recording_state_changed`, `recording_destination_changed`.

**Job slot state machine** (per job):
`idle` → `_claim_job` (refused if active, shutting down, backup, host update, or recorder busy when
required) → `working` (worker runs; cancel sets a per-job `threading.Event`, a *new* Event per job,
transcription.py:296-300) → result emitted → Qt slot calls `_settle()` (sets `_saving`; returns
whether cancel arrived first, transcription.py:268) → `saving` (history on persistence worker;
cancel no longer applies) → `on_history_persisted` → `_finish_job` resets every field (386-415) → idle.
Rewrite jobs add `_rewrite_pending` between claim and submit.

**Recording flow** (`_start_recording` 550): gates → remote reserve (undecided) → `recorder.start_recording()`
→ snapshot profile → `_begin_job` (never raises; falls back to bare DictationJob, 511-522) →
`_prepare_backend` thread (API only) → destination signal → clear stats/partial → start preview →
start incremental early decoding (not for walkthrough, 608-611) → `recording_state_changed(True)` →
event → `_previewing` set → listening status → clipboard prefetch.

**Stop flow** (`_stop_recording` 733): ignore if capture canceled → close walkthrough to new shots →
hide preview overlay → `begin_stop_streaming_session` → `recorder.stop_recording()` → PROCESSING →
end preview → event → claim slot (else publish canceled + busy) → executor `finish_recording_job`
(wait post-roll; stop preview and keep native final; cancel check; save WAV; set `_pending_*`;
capture error; cancel check; `_run_transcription_job`).

**Decode** (`transcribe_audio_file` 1625): announce → final recognition → walkthrough sections or
`_decode_and_complete` (decode via `recognition_context.transcribe(incremental, backend, path, rec)`,
which merges early-decoded windows; `RemoteDecodeNotStarted` retry only for dictation with saved audio,
1684-1691) → preview fallback → Command/Transform → `command_runtime.complete_recording`; live job →
`_complete_live`; upload → `_maybe_cleanup_transcript`.

**Edge cases a port must keep**:
- Cancel between worker emit and Qt slot is honored (2199-2202); cancel after settle reaches actions only.
- Filter that empties already-empty text is "no speech", not "filter handled" (1948).
- Filter/routing exceptions are logged by type only (never the dictated words; 1945, 1974).
- Command Mode refusal drops the instruction audio and its journal (`_drop_instruction_audio` 2124).
- Upload/batch never set `_pending_audio_path`, so preview fallback never applies to them (2180).
- Batch: single item takes single path (1198); `_pending_*` cleared for batches (1228).
- Batch combined result with canceled + empty → "Transcription canceled" (1416).
- Batch UI must receive transcript before overlay NONE (tab treats NONE as failure; 1427-1430).
- History entry drops `spoken_text`, `stats_counts`, `job_id`, `retry_entry_id` before saving (2580);
  retry completes the failed entry without overwriting audio fields (2583-2586).
- `on_history_persisted` always clears metadata and finishes the job (finally, 2630).
- Rewrite claim uses `acquire(blocking=False)` so the Qt thread never waits on a start that holds the
  lock while a Bluetooth device opens (2794).
- Program-started (non-user `source`) busy refusals are silent (2823-2826).
- `_model_info_for_history` writes model name + device info into History `model` column (2642).

### `command.py` — `CommandRuntime`, `CommandRefused`, `UnreadSelection`
- **Responsibility**: Command Mode recordings (instruction → rewrite selection or insert), transform runs,
  `rewrite_text` for action shaping. Never logs selections/instructions/results.
- **Surface**: `key_pressed(at)`, `key_released(at)` (hotkey dispatcher thread), `run_transform(id)` (Qt),
  `complete_recording(raw, job) -> (text, raw_text, info)` (worker), `rewrite_text(text, instruction)`,
  `cleanup()`. Constants: `NO_PROVIDER_MESSAGE`, `KEYS_HELD_MESSAGE`, `PASSWORD_MESSAGE`,
  `CLIPBOARD_HIDDEN_MESSAGE`, `SELECTION_WAIT_S`.
- **Threading**: `_QtCall` QObject hops lambdas to Qt (exceptions logged, 136-149); selection read on
  daemon threads "command-selection"/"transform-selection"; `_reading` WeakSet ensures each selection
  Future is read exactly once (265-277); selection resolves a `concurrent.futures.Future` with str or
  `UnreadSelection`.
- **State**: `_Recording(selection, job, press_at, trigger_mode)`; toggle → second press stops; push-hold
  release < `RECORD_MIN_HOLD_MS` (250) → `controller.cancel()`.
- **Edge cases**: focus probe taken before start when app-context disabled (229-233); "running" means
  recorder recording AND runtime `_job is recording.job` (210-216); `_resolve` tolerates
  InvalidStateError (80-85); copy-line editor heuristic `_looks_like_copied_line` (122-133);
  UIA empty selection is final (305-308); rewrite result raw_text is the selection when non-blank (420);
  `text_rewrite.REFUSALS` map to CommandRefused (413).

### `components.py` — `ComponentRuntime(QObject)`
- Signals: `component_progress(str id, str phase, int done, int total)`, `component_install_started(str)`,
  `component_install_finished(str, bool, str)`, `component_state_changed()`.
- `request_install(id)`: runtime ids refused during speech jobs; backends using that runtime cleaned up
  first; `component_coordinator.begin_install` dedups; runs on `component_executor` (1 worker).
- `_install_worker`: progress throttle (≥100 ms or percent change or final, 131-141); catalog miss →
  "Could not reach the download server. Check your internet connection and try again."; activate in
  process (Windows DLL dir registration) else restart message.
- `_on_install_finished`: runtime installed for current local backend → reload engine (unless remote);
  GPU_ACCEL → device "cpu" → "auto", reload unless already CUDA or remote (182-226).
- `request_uninstall`: runtime ids on executor under `_engine_lock`.

### `hotkeys.py` — `HotkeyRuntime(QObject)`, `ActiveWindowHotkeyFilter`, `is_hotkey_window`
- Signals (dispatcher thread → Qt): `transform_requested(str)`, `scratchpad_toggle_requested`,
  `cycle_language_requested`, `paste_last_original_requested`, `walkthrough_requested`,
  `screenshot_requested`, `paste_walkthrough_requested(object)`, `profile_record_requested(str)`,
  `action_record_requested(str)`, `hands_free_changed(bool)`, `stt_state_changed(bool)`.
- Constants: `HOTKEY_WINDOW_PROPERTY = "openwhisperHotkeyWindow"` (dynamic property set True on
  top-level windows like the Scratchpad to get focused-window shortcuts), `KEYS_STILL_HELD`.
- `OrderedDispatcher("hotkey-dispatch")` (a06 `_hotkey_common`) runs record/command press/release in
  hook order with `call_at(deadline, fn, gen)` and `cancel_deadline()`.
- **Push-and-hold state machine** (`_IDLE`, `_HOLDING`, `_TAP_PENDING`, `_LATCHED`, 47-53):
  - press: expire tap; drop stale latch (recording ended since hold); TAP_PENDING → latch; LATCHED →
    reset + stop; HOLDING with accepted start, same epoch, still recording → continue (lost release
    after rehook, 387-395); recorder busy (post-roll) → ignore; else start, HOLDING if accepted.
  - release: in TAP_PENDING/LATCHED ignore; not accepted → no-op; wait up to 0.5 s for stream open
    (polling 10 ms, 424-426); not recording → no-op; capture_canceled → no-op; held ≥ 250 ms → stop;
    latch setting off → cancel; same epoch → TAP_PENDING with deadline `released_at + 0.4 s`.
  - deadline → cancel tap (only if same epoch and recorder recording and not canceled).
  - `_recordings_ended` counter increments on every `recording_state_changed(False)` (365-370).
- Watchdog: `CoarseTimer` 10 s, `VeryCoarseTimer` 5 min; skip on Wayland; rehook ≥ 60 s apart;
  `keyboard_hold_active()` defers refresh, `_refresh_pending` runs it on next tick (656-703).
- `ActiveWindowHotkeyFilter` (pynput only): maps Qt keys to hotkey names ("space","esc","enter","tab",
  "backspace","delete","insert","home","end","page_up","page_down","left/right/up/down", a-z, 0-9,
  f1-f24, Linux keypad → "kp x", macOS native modifier bits 17-20 → cmd/ctrl/alt/shift). Ignores
  autorepeat; only when active window is main window or has the property.

### `meeting.py` — `MeetingRuntime(QObject)`, `redact_meeting_url`, helpers
- Signals: `meeting_engine_event(object engine, str kind, object payload)`, `meeting_state_changed(object dict)`,
  `meeting_status_update(str)`, `meeting_error(str)`, `meeting_server_started(object)`,
  `meeting_recovery_found(object)`, `meeting_recovery_scan_failed(str)`, `meeting_consent_requested()`,
  `meeting_platform_ack_requested()`, `meeting_guest_link_ready(str)`, `past_meetings_refresh_requested()`.
- Constants: `SHUTDOWN_JOIN_TIMEOUT_S=5.0`, `RETRY_BUSY_MESSAGE`, `RETRY_FAILED_MESSAGE`.
- State (under `_lock` RLock): `_engine`, `_repo` (lazy `SqlMeetingRepository`), `_host_url`, `_guest_url`,
  `_archive_dashboard`, `_archive_starting`, `_starting`, `_finalizing`, `_retry_generation`,
  `_retry_meeting_id`, `_retry_claim`, `_background_ready`, `_background_engines/_workers/_host_urls`,
  `_finalization`, `_card_meeting_id`, `_deferred_start_errors`, `_consent_pending_kind`
  ("start"/"start_demo"/"toggle"), `_pending_system_audio_policy`, `_pending_intent`.
- **Lifecycle state machine**: idle → `_starting` (claimed across consent) → `_launch` (state
  `{"status":"starting"}`) → `_start_worker` thread "meeting-start" (shutdown archive + previous engine;
  build options; port 0 if background engines hold the port; publish engine early; release dictation
  engine unless remote ASR; `engine.start()`; require dashboard URL) → `_adopt_started_engine` (drop if
  engine already stopping) → active (`controller.meeting_active=True`) ⇄ paused → ending →
  ended (event) → finalizing (status running) → terminal (completed/disabled/unavailable/failed) →
  card retained / deferred / background.
- `is_claimed` excludes finalization so dictation unlocks once capture ends (707-715).
- Engine events reach `_route_engine_event` on Qt; only from the current engine; retry events filtered by
  generation and claim currency (2002-2034); also forwarded to automation `meeting_events.forward`.
- Workers: recovery scan, restore last finalization, retry ("meeting-retry-finalization"), background
  finish ("meeting-background-{id}"), past meeting dashboard, delete, clear, discard, recover — all
  daemon threads.
- Shutdown: archive dashboard, engine shutdown on daemon thread with 5 s join; logs stuck stack via
  `sys._current_frames` (2258-2271); background workers joined against a shared 5 s deadline.
- **Edge cases**: dashboard URLs carry capability tokens → only `scheme://host/<redacted>` logged (74-90);
  `_with_history_target` adds `history`/`view` query params preserving others (63-71); a declined consent
  still starts transcript-only (850-858); toggle declined re-emits `cloud_enabled False` (866-878);
  deferred start errors shown only after success (980-985, 2123-2129); `_hydrate_finalization_card`
  refuses during start/active/finalizing/background (384-389) and skips `pending` without steps;
  `delete_past_meeting` refuses while live/background/busy; `clear` skips live/background/retry ids and
  tells record sync what to keep.

### `models.py` — `ModelRuntime(QObject)`
- Signals: `hf_consent_requested(str model, bool env_blocked, bool load_into_engine)`,
  `runtime_consent_requested(str)`, `model_download_started(str)`, `model_download_progress(str, int, int)`,
  `model_download_finished(str, bool)`, `model_deleted(str, bool, str)`, `model_cache_changed()`,
  `batch_download_planned(list)`, `batch_download_finished(int, int)`, `gpu_fallback_detected()`.
- `hf_access_coordinator` (a14) dedups requests (`begin_request`/`end_request`), evaluates access
  (`LOAD_CACHED`, `DOWNLOAD_ALLOWED`, `BLOCKED_BY_ENV`, `NEEDS_CONSENT`), grants once/always.
- Request slot ownership: requester claims; either download worker or consent slot releases. Batch:
  `claim_batch` gives one slot per model to the worker; unreached models released in outer finally
  (742-746).
- `_hf_model_worker` reloads backend under `_engine_lock` only if still the selected backend and no
  meeting (609-620). Declined download reverts Whisper selection to last loaded (356-385; Whisper only).
- `_gpu_offer_state`: None → "shown" → "answered"; model consent waits for the offer (135-140).
- `_on_model_download_finished`: tiny.en with streaming enabled → reconfigure preview (Whisper only).

### `remote.py` — `RemoteRuntime(QObject)` (undecided backend)
- Signals: `remote_catalog_received(object, object, object)`, `remote_link_poke()`, `remote_settled(str)`,
  `remote_retry_requested()`, `remote_job_retry_requested(object Event)`, `remote_clients_changed(list)`,
  `remote_pairing_changed()`, `remote_host_renamed()`, `records_changed()`, `client_model_switch_requested(object)`.
- Watch timer 1000 ms; retry single-shot with backoff `(2,5,15,30,60)` s; `_restoring` flag; reconnect on
  daemon thread "remote-reconnect" → `remote.restore_link()` outcome string ("offline","changed",
  "unpaired","reload", ...). Status updated only when something changed (440-446). After reconnect, flush
  pending streaming setup.
- Host side: `switch_engine_for_client`/`configure_engine_for_client` block the connection thread up to
  10 s on a `threading.Event` while Qt performs the switch (178-200).

### `streaming.py` — `StreamingRuntime(QObject)`, `preview_unavailable_reason`
- Signals: `partial_transcription(str, bool)`, `streaming_text_update(str, bool)`, `streaming_overlay_show()`,
  `streaming_overlay_hide()`, `streaming_setup_requested()`.
- Flags: `_stopping` suppresses partials once stop begins; `_configured_for` = (backend, backend_id,
  model_name) so setup is skipped when still valid or while recording.
- Preview choice (`_preview_backend` 246): Whisper → dedicated tiny.en (drop); remote not available →
  wait (`_pending_streaming_setup=True`); shared-decoder engines (`STREAMING_PREVIEW_BACKENDS` =
  parakeet, nemotron, parakeet_mlx, parakeet_unified) → wait if loading, native if backend in
  `STREAMING_NATIVE_PREVIEW_BACKENDS` (nemotron, parakeet_unified) and model advertises streaming, else
  window `StreamingTranscriber(chunk_duration_sec=setting, overlap_sec=0.75)`.
- Native warmup: one throwaway 0.5 s silence stream (first push 0.28 s vs 60 ms warm) (360-375).
- `start_streaming_session` sets `ui.streaming_flow_active=True` synchronously before emitting show, so a
  queued RECORDING overlay doesn't flash the waveform (147-151).
- `stop_streaming_session` returns text immediately (window preview keeps tail); `finalize_streaming_text`
  blocks to decode tail (worker only, only for empty final); `cancel_streaming_session`.

### `updater.py` — `UpdaterRuntime(QObject)`
- Signals: `update_check_finished(object result, str error, bool manual)`,
  `update_download_progress(object attempt, str phase, int, int)`, `update_download_finished(object attempt, object handoff, str error)`.
- `UPDATE_RECHECK_FLOOR_MS = 3_600_000`; `schedule_check(launch)` single-shot `UPDATE_CHECK_DELAY_MS=8000`;
  launch check uses interval 0; afterwards `seconds_until_auto_check()` (a16) decides; `None` = off.
- Checks on `executor`; downloads on `component_executor` under `_download_lock`; each attempt has a fresh
  cancel Event and an identity token; stale results discarded and their handoff deleted (205-232).

### `walkthrough.py` — `WalkthroughRuntime(QObject)`, `shots_label`
- Private signals: `_click_received(object session, int x, int y, float at)` and
  `_mark_shortcut_received(object session, str action)`, both QueuedConnection (hooks fire off-thread).
- Constants: `RECORDING_HINT="Recording continues — talk while you draw"`, `TRAY_SNAP_DELAY_MS=300`,
  `FINISH_RECORDING_FIRST`, `WAIT_FOR_WRITE_UP`, `COUNTDOWN_S=3`, `AUTO_SETTLE_MS=350`,
  `_AUTO_MARK_HOTKEYS={"keep_marks":"f8","clear":"f9"}`, family `"walkthrough_marks"`.
- Timers: countdown 1000 ms; `_auto_progress` 33 ms (countdown ring repaint); `_auto_timer` single-shot
  PreciseTimer 350 ms or `capture_delay*1000`; tray timer 300 ms.
- State: `_snap`/`_snap_for` (open SnapSession), `_finish_after_snap`, `_active`, `_out_of_screenshots`,
  `_countdown_left`, `_panel`, `_source`, Auto Mode: `_auto_hook`, `_live_drawing`, `_auto_snap`,
  `_auto_drawing_capture`, `_snap_after_drawing`, `_auto_position`, `_auto_draw_pending`, `_auto_draw_next`,
  `_auto_deadline`, `_auto_delay`, `_auto_after` (ignore clicks queued before a pause/manual snap).
- Panel signals it answers (a23 `walkthrough_panel`): `snap_requested`, `snap_mode_chosen(str)`,
  `finish_requested`, `cancel_requested`, `undo_requested`, `move_requested(int,int)`,
  `remove_requested(int)`, `note_held(bool)`, `pause_requested(bool)`; calls `set_snap_mode`,
  `set_notes_on`, `set_auto_mode`, `set_countdown`, `add_shot(number, image, kept_at_s, source_rect)`,
  `move_shot`, `remove_shot`, `set_paused`. Connected once across walkthroughs (178-193).
- Live drawing signals (a22 `ui_qt/snap/live.py`): `arming`, `capture_changed`, `drawn(screen, marks)`,
  `capture_delay_changed(int)`, `capture_cancelled`, `interaction_changed(bool)`, `enabled_changed(bool)`;
  methods `arm`, `set_enabled`, `set_busy`, `set_capture_pending`, `set_capture_countdown`,
  `capture_region`, `pinned_at`, `pinned_on`, `handle_mark_shortcut`, properties `interacting`, `armed`.
- **Edge cases**: Done keeps the screenshot being drawn then stops (208-229); Done doesn't wait out a
  capture delay for a pending click (221-227); never capture mid-stroke (344-351); a drawing on another
  monitor gets its own screenshot (391-397, 457-465); auto region crop + marks rendered at device
  resolution via `ui_qt.snap.export.render` (443-447); snap failure on a closed session goes to clipboard
  (628-631); `ScreenshotNotSaved` keeps recording (643-646); cancel vs stop decided via
  `recorder.capture_canceled` or `transcription_runtime.canceling_recording` (771-779).

## Data and formats

**Settings keys read/written here** (owned by a10 `services/settings.py`; JSON `openwhisper_settings.json`):

| Key (JSON name) | Type | Default | Where |
|---|---|---|---|
| `auto_paste` | bool | True | transcription 626, 2676, 2443; hotkeys 497 |
| `copy_clipboard` | bool | True | transcription 2675 |
| `restore_clipboard` | bool | False | transcription 626, 2729 |
| `transcript_cleanup_enabled` | bool | False | transcription 1550 |
| cleanup provider/model/reasoning/rules (resolve_*) | str | a10/a12 | transcription 1556-1572 |
| `streaming_enabled` | bool | False | streaming 227; models 545 |
| `streaming_chunk_duration` | float s | 3.0 | streaming 301 |
| recording trigger mode (resolve) | "toggle"/"push_hold" | "toggle" | hotkeys 285, command 246 |
| hands-free latch (resolve) | bool | a10 | hotkeys 436 |
| voice routing enabled / on dictation | bool | a10 | transcription 541-543 |
| command mode insert without selection | bool | a10 | command 462 |
| app context enabled / read text | bool | a10 | command 230, 355, 397 |
| `macos_accessibility_intro_seen` | bool | False | hotkeys 501 |
| hotkeys (`load_hotkey_settings`/`save_hotkey_settings`) | dict name→combo | `config.DEFAULT_HOTKEYS` merged | hotkeys 282, 530 |
| `whisper_device` | str | "auto" | models 400-406, 471; components 208-214; remote 497 |
| `whisper_model`, `whisper_compute_type` | str | a10 | models 382, 473; remote 497, 516 |
| `whisper_gpu_offer_declined` | bool | False | models 418, 456 |
| `local_asr_models`, `local_asr_devices` | dict family→value | {} | remote 500-513 |
| HF access policy | choice | a10 | models 341 |
| model selection (`save_model_selection`) | str backend id | `DEFAULT_BACKEND` | transcription 3013 |
| `meeting_cloud_consent_given` | bool | False | meeting 259, 1724 |
| `meeting_cloud_last_enabled` | bool | False | meeting 826, 905, 1700 |
| meeting language, whisper model, asr source, speaker id backend, audio upload consent, server bind/port, end redecode/polish/report, redecode coverage guard, insight review, report views, backup agent, agent model | various | a10 | meeting `_build_options` 1074 |
| walkthrough snap mode `walkthrough_snap_mode` | choice | "region" | walkthrough 552, 564 |
| walkthrough auto mode, capture delay (s), write-up (+agent, instructions, notes), copy format | various | a10 | walkthrough 165-175, 317; transcription 1769-1780, 2429, 2441 |

**Config constants used** (config.py): `RECORD_MIN_HOLD_MS=250`, `RECORD_LATCH_WINDOW_MS=400`,
`HOTKEY_WATCHDOG_INTERVAL_MS=10000`, `HOTKEY_SLEEP_GAP_THRESHOLD_SEC=30`, `HOTKEY_HOOK_REFRESH_INTERVAL_MS=300000`,
`RECORDING_TRIGGER_MODE="toggle"`, `SAMPLE_RATE=44100` (preview start), `WHISPER_TARGET_SAMPLE_RATE=16000`,
`STREAMING_OVERLAP_SEC=0.75`, `STREAMING_NATIVE_UPDATE_SEC=0.75`, `CONTEXT_CAPTURE_DEADLINE_S=0.4`,
`COMMAND_SELECTION_TIMEOUT_MS=700`, `TRANSCRIPT_BATCH_CLEANUP_TIMEOUT_S=120`, `UPDATE_CHECK_DELAY_MS=8000`,
`MODEL_VALUE_MAP` (display name → backend id: "Local Whisper"→local_whisper, "API"→api, "Parakeet"→parakeet,
"Qwen3-ASR", "Nemotron Streaming"→nemotron, "Moonshine", "Parakeet MLX", "Parakeet Unified", "Remote computer"→remote),
`DEFAULT_HOTKEYS` (Windows: record `ctrl+alt+r`, cancel `ctrl+alt+escape`, enable/disable `ctrl+alt+shift+r`,
minimize `ctrl+alt+m`, meeting "", paste walkthrough `ctrl+shift+f12`, plus unbound others),
`synthetic_keys.MODIFIER_WAIT_S=0.8`.

**Files on disk** (user data dir): `recorded_audio.wav` (`config.RECORDED_AUDIO_FILE`, the live dictation
WAV, acknowledged/removed after retention), `recordings/` (failed-audio retention, recovered recordings),
`meetings/` (meeting spools), walkthrough captures root (`services.walkthrough.storage.captures_root()`),
screenshot files (`new_screenshot_path(now)`), recording journal spool (`recorder._audio_spool.directory`),
`openwhisper.log` (named in a GPU message).

**Database**: via `history_manager` (a09): `add_entry(text, model, source_audio_path, transcription_time,
audio_duration, file_size, raw_text, cleanup_provider, cleanup_model, source_name, entry_kind, + history
context fields)`, `complete_failed_recording(id, **fields)`, `failed_recording_for_path(path)`,
`preserve_recording(path) -> name`; stats via `dictation_pipeline.record_stats`. Meetings via
`SqlMeetingRepository` (a28): `list_meetings`, `get_meeting`, `get_segments`, `rename_meeting`, `persist_state`.

**History `source_name` values** (user-visible in History): "Quick Record", "Quick Record · {profile}",
"Command Mode", "Action · {name}" / "Action", "Walkthrough", "Transform · {name}", file basename for uploads,
`batch_source_name([...])` for combined batches.

**Wire / IPC contracts**:
- Automation events (a19 `event_catalog`): `recording.started`, `recording.stopped(duration)`,
  `recording.canceled`, `transcription.preview(text, final)` (replay=False), `transcription.completed(text,
  raw_text, fields, cleanup, audio_s)`, `transcription.failed(category ∈ no_speech|canceled|refused|failed,
  message)`, `history.saved(job_id, record_id, entry_kind)`. Consumed by the local API/MCP (a20).
- Meeting dashboard URLs: `{scheme}://{host}:{port}/<token path>?history={meeting_id}&view=report`.
- Meeting state dict emitted to the Meeting tab: keys `active, paused, status, cloud_enabled, finalization,
  meeting_id, dashboard_available, background_available, background_message, elapsed_s, elapsed_at, speech,
  title, display_title, started_at, ended_at, paused_total_s, insights_pill, insights_tone`; finalization
  carries `content_summary` (+ `meeting_status`, `speaker_rerun_offered`, `speaker_rerun_reason`).
- Engine event kinds (meeting engine → runtime): `status, server_started{host_url,guest_url}, error{message},
  background_ready, asr_released, ended{status,capture_error}, intelligence{online}, voice_feedback, segments`;
  internal `retry_finalization`, `retry_status` carry `_retry_generation` and `_finalization_claim`.

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| PyQt6 QObject/pyqtSignal | cross-thread signals | QObject + signals, `Qt::QueuedConnection` |
| QTimer (CoarseTimer, VeryCoarseTimer, PreciseTimer, singleShot) | watchdog, retries, countdowns | QTimer with same `Qt::TimerType` |
| QEvent/QApplication event filter | pynput focused-window hotkeys | `QObject::eventFilter` (other-platform) |
| QRect | crop rect | QRect |
| threading.Lock/RLock/Event/Thread | job slot, cancel tokens, workers | `std::mutex`, `std::recursive_mutex`, a cancel token (`std::atomic<bool>` + `std::condition_variable` or `std::stop_source`), `std::jthread`/QThread |
| concurrent.futures ThreadPoolExecutor, Future | executors; selection future | `QThreadPool` per role (sizes 2/1/1/1) or own pool; `std::promise/std::shared_future` or `QFuture`/`QPromise` |
| weakref WeakKeyDictionary/WeakSet | once-only selection reads | `std::set` keyed by shared_ptr state, cleared on job end |
| dataclasses.replace | immutable job updates | value types + copy-with |
| itertools.count | recording end counter | `std::atomic<uint64_t>` |
| time.monotonic/time.time | timing | `std::chrono::steady_clock` / `system_clock` |
| os.path, os.getsize | file checks | `std::filesystem` (UTF-8 via `path_from_utf8`) |
| wave | walkthrough recovery duration | OpenWhisper_CPP `engine/wav.h` reader |
| webbrowser | open dashboard | `QDesktopServices::openUrl` |
| urllib.parse | URL query edits/redaction | `QUrl`, `QUrlQuery` |
| socket.gethostname | host name in remote messages | `QSysInfo::machineHostName()` |
| sys._current_frames/traceback | log stuck shutdown | omit or log thread id |
| numpy | warmup silence | `std::vector<float>(8000)` |
| logging | logs | spdlog or Qt logging categories (match `openwhisper.log` naming only if kept) |

## Platform-specific code

- **Windows**: global hooks swallow hotkeys; no focused-window filter (hotkeys.py:56-60, 623-641).
  Auto Mode uses `MouseButtonHook` low-level mouse hook (walkthrough.py:246). Component activation
  relies on Windows DLL search path registration (components.py:154-158). Watchdog runs (non-Wayland).
- **macOS**: pynput/Carbon backend → `ActiveWindowHotkeyFilter` with native modifier bits;
  Accessibility check before auto-paste, degrade to copy with Cmd+V message (transcription.py:2685-2709;
  hotkeys.py:504-524); Auto Mode via `MouseClickMonitor`; updater prunes Mac transactions (updater.py:250-259).
- **Linux X11**: pynput filter; keypad names "kp x" (hotkeys.py:133-138); watchdog runs.
- **Linux Wayland / Hyprland**: no watchdog/rehook (hotkeys.py:592, 693); Omarchy controls on
  Omarchy UI (hotkeys.py:310-317); walkthrough panel absent on Wayland (walkthrough.py:93-94 comment);
  meeting start may fail with `LINUX_SYSTEM_AUDIO_REQUIRED:` prefix (meeting.py:1007-1011).
- Auto Mode only on `win32`/`darwin` (walkthrough.py:174).

## Engine-specific code

**Drop (exists only for non-Parakeet local engines or the Python worker plumbing)**:
- streaming.py `_load_dedicated_preview_backend`, `_warmup_streaming_backend` (tiny.en faster-whisper
  preview), the `LocalWhisperBackend` branch of `_preview_backend` (249-254), Parakeet Unified/
  Parakeet MLX entries in preview lists and status strings.
- transcription.py `on_model_changed` Whisper branches (3016-3042), `_model_info_for_history`
  "local_whisper (...)" text (2642-2651), `isolated.cleanup_orphaned_preview_audio` (427-429, worker
  process plumbing).
- models.py GPU fallback/"Use this GPU" (389-503), declined-download Whisper revert (356-385), tiny.en
  reconfigure (540-546), CTranslate2 memory-map delete guard rationale.
- components.py GPU_ACCEL CUDA-for-Whisper handling (203-226); `LocalSpeechBackend.runtime_component`
  per-engine runtimes (Moonshine, Qwen3, ...).
- remote.py `_whisper_already_serves`, Whisper branches of `_save_engine_model/_save_engine_device`.

**Adapt to in-process Parakeet** (OpenWhisper_CPP `Recognizer` on `EngineWorker`):
- `current_backend` becomes the Parakeet engine facade on the EngineWorker thread; `is_transcribing`,
  `cancel_transcription`, `is_available`, `is_model_missing`, `device_info` must exist on it. Cancel of
  an in-flight `transcribe` needs a runtime-level abort or a chunked decode with a cancel check (NeMo C
  ABI has no cancel today — risk).
- Live preview = window preview: decode overlapping windows with `Recognizer::transcribe` (CLAUDE.md:
  TDT is offline-only); the "shares the loaded engine" path. Must serialize with final pass on the one
  engine thread; the kept tail / `finalize_preview` logic stays.
- `IncrementalDictation` early decoding of completed windows (a05) shares the same engine queue.
- Model download/consent (models.py) → Parakeet model file(s) download with the same consent UX.
- Component runtime installs → NeMo-Speech.cpp CPU vs GPU runtime downloads; "activate without restart"
  maps to `NemoApi::get(path)` loading a new runtime (never unloading the old one — CLAUDE.md).
- Meeting model lease (`release_local_engine`/`restore_local_engine`): with one shared Recognizer, the
  meeting can borrow the dictation recognizer instead of releasing/reloading weights.

**Undecided backends**: Nemotron native stream (`NativeStreamingTranscriber`, warmup, 0.75 s updates),
OpenAI API (prepare thread `prepare_for_transcription`, large-file split announce), Remote engine
(transcription.py reserve/release recording, reconnect, `RemoteDecodeNotStarted`, timing marks,
`prepare_host_update`; all of remote.py; meeting `asr_remote`).

## Dependencies on other areas

**Calls out**:
- a03 ApplicationController / UIController: controller signals and state (`_pending_*`, `meeting_active`,
  `executor`s, `_engine_lock`, `reload_whisper_model`, `release/restore_local_engine`,
  `transcription_readiness_message`, `local_whisper_loading_message`, `_selected_remote`,
  `_refuse_dictation_during_meeting`, `toggle_*_recording`, `minimize_to_tray`); `OverlayState`
  (NONE, RECORDING, COMMAND_LISTENING, PROCESSING, TRANSCRIBING, CLEANING, REWRITING, CANCELING,
  STT_ENABLED, STT_DISABLED). UI controller methods: `set_status`, `set_ambient_status`, `show_notice`,
  `set_transcript`, `set_transcription_stats`, `clear_transcription_stats`, `refresh_history`,
  clipboard (`prefetch_clipboard_snapshot`, `discard_clipboard_prefetch`, `stage_transcript_for_paste`,
  `commit_transcript_clipboard`, `schedule_clipboard_restore`, `copy_to_clipboard`, `copy_files_to_clipboard`,
  `capture_selection`), `insert_into_scratchpad`, walkthrough UI (`set_walkthrough_active`,
  `walkthrough_panel`, `show_walkthrough_ready`, `paste_walkthrough`, `dismiss_walkthrough_panel`,
  `keep_overlay_out_of_screenshots`, `start_snap`, `snap_support`, `request_snap_access`,
  `show_overlay_caption`, `start/stop_walkthrough_drawing`), meeting dialogs, model/component/update UI
  callbacks, `show_hf_consent_dialog`, `show_use_gpu_dialog`, `show_gpu_runtime_dialog`,
  `show_required_runtime_dialog`, `open_downloads`, `open_settings_dialog`, remote UI setters.
- a05: `recorder` (start/stop/cancel/pause/resume, post-roll wait, save, durations, journal, acknowledge,
  callbacks), `audio_player.stop_playback`, `dictation_pipeline` (DictationJob, JobMode, begin_job,
  prepare_text, finish_text, compose_cleanup_prompt, paste_target, text_for_paste, html_for_paste,
  after_paste, history_fields, record_stats, recognition_for), `incremental_dictation`,
  `streaming_transcriber`, `recording_journal.recover_recordings`.
- a06: `hotkey_manager` (HotkeyManager, send_paste, is_accessibility_trusted, format_hotkey_display,
  set_dynamic_hotkeys, rehook, keyboard_hold_active), `_hotkey_common.OrderedDispatcher`, `hotkey_conflicts`,
  `synthetic_keys`, `focus_context`, mouse hooks, `omarchy_controls`.
- a08: `batch_upload` types/formatters. a09: `history_manager`, `services.models.FAILED_RECORDING_KIND`.
- a10: `settings_manager`, SettingsKey, resolve_* functions.
- a12: `transcript_cleanup.TranscriptCleanup`, `cleanup_profiles`, `text_rewrite`, `text_transforms`,
  `recognition_context`.
- a14: `hf_access`, `components`, `component_runtime`, `local_asr.catalog`, `gpu_setup`,
  `transcriber.optional_backend.LocalSpeechBackend`, `isolated`.
- a16: `app_update`, `app_update_apply`, `app_update_macos`, `setup_update`.
- a17: `remote_asr.*`, `remote_records.sync.record_sync`. a19: `actions.model/routing/runtime`,
  `automation.events/catalog/filters/models/control/permissions/meeting_events`. a20: `agent_mcp.controls.ControlError`.
- a22: `walkthrough.session/finish/write_up/library/storage`, `ui_qt.snap.export`.
- a24-a31: `meeting.engine`, `meeting.recovery`, `meeting.refinalize`, `meeting.stored`,
  `meeting.state.schema.FinalizationState`, `meeting.content`, `meeting.persist.*`, `meeting.export.transcript_txt`,
  `meeting.web.archive/server`, `meeting.asr.remote`, `services.meeting_agent_route`, `services.meeting_rerun`,
  `services.audio_devices.load_priority`.
- a01: `services.desktop_session`, `services.diagnostics.record_metrics`, `config`.

**Called by**: a03 controller constructs every runtime (application_controller.py:255-266) and connects
their signals; UI (a08 record/upload tabs, a09 History retry, a15 downloads, a18 remote UI, a23
walkthrough panel, a31 meeting tab, tray) calls their public methods; a19/a20 automation API calls
`run_action_text`, `cancel`, `paste_text_now`, `retitle_saved_meeting`, `meeting_busy`; a17 host server
calls `switch_engine_for_client`, `configure_engine_for_client`, `prepare_host_update`, `on_host_event`.

## Tests

| Test file | Pins |
|---|---|
| tests/test_push_hold_recording.py | push-hold press/release, toggle mode, long hold stops, short tap cancels, refused start, press during post-roll ignored, release before stream open, release after cancel hotkey |
| tests/test_personalize_fix_f1_latch_no_data.py | latch before any audio, lone tap cancels after window, long hold w/o audio stops |
| tests/test_hook_refresh_waits_for_hold.py | periodic rehook deferred while held |
| tests/test_personalize_fix2_g1_rehook_hold.py, test_hotkey_headless.py | lost release after rehook; headless hotkeys |
| tests/test_cancel_after_stop.py | cancel in post-roll / before decode / during decode / during cleanup / between emit and slot; stop after cancel; preview can't start → no overlay |
| tests/test_automation_r4_cancel_runtime.py, test_automation_r4_cancel_uploads.py | quiet repeat cancel; cancel results for uploads/batches/combined |
| tests/test_paste_latency.py | paste before history; status ordering; prefetch rules; restore clipboard |
| tests/test_preview_lazy_tail.py | fast stop, kept tail finalize, empty dictation fallback, uploads never finalize |
| tests/test_streaming_transcriber.py, test_incremental_dictation.py | preview transcriber + runtime glue |
| tests/test_personalize_s3_command_runtime.py, s3_command_delivery.py, s3_controller.py | Command Mode and transforms end-to-end |
| tests/test_personalize_fix_f2_selection_*.py, fix_f3_console_terminal.py | selection refusal, keys held, terminals |
| tests/test_dictation_runtime_exception_handling.py | `_resolve`/`_snapshot` robustness; filters/routing never log words |
| tests/test_automation_actions_*.py, fix_pipeline_shaping.py, foundation_pipeline.py, hooks_pipeline.py | routing/filters/shaping/delivery |
| tests/test_cleanup_deadline.py | cancel during stalled cleanup frees dictation |
| tests/test_walkthrough_runtime.py | walkthrough job sections, copy formats, write-up, cancel, recovery |
| tests/test_walkthrough_shortcuts.py | walkthrough runtime shortcuts, snap modes, countdown, Auto Mode, panel ops |
| tests/test_walkthrough_paste.py | agent auto-paste of walkthrough |
| tests/test_meeting_runtime.py (2149 lines) | start/consent/failure, finalization card, retry generations/claims, background handoff, recovery, past meetings |
| tests/test_meeting_archive_dashboard.py, test_meeting_title_consistency.py, test_meeting_start_readiness_seams.py | archive dashboard, retitle, readiness |
| tests/test_use_gpu_offer.py | GPU offer (drop) |
| tests/test_update_resilience.py (runtime parts: stale attempt, queued success after cancel) | updater attempt tokens |
| tests/test_remote_engine.py, test_host_mode.py | remote runtime (undecided) |
| tests/test_controller_thread_hops.py | worker signals delivered on Qt thread |
| tests/test_strict_switch_reads.py, test_typed_settings.py | strict bool reads of cleanup/auto-paste switches |
| tests/test_application_controller.py | controller-level integration |

**Golden-vector candidates** (pure, deterministic):
- `transcription._batch_line`, `_batch_display`, `_batch_status` (+ a08 `format_batch_transcript`).
- `transcription._recording_destination`, `_recording_source_name` logic, `_delivery_notice` / status
  string builders, `_copy_walkthrough` status composition (pure given inputs).
- `command._looks_like_copied_line`, `_refusal`, `_known_selection`.
- `meeting.redact_meeting_url`, `_with_history_target`, `_ended_message`, `_recovered_follow_up_step`,
  `_background_message`.
- `walkthrough.shots_label`.
- `streaming.preview_unavailable_reason`.
- `hotkeys` push-and-hold state machine as an event-sequence table (press/release timestamps,
  recording-ended events → actions stop/cancel/latch/no-op).
- `remote` backoff sequence and `updater` wait computation (`max(wait, 1h)`).
- `_qt_event_key_name` mapping table (other-platform).

## Porting notes

**Decomposition**
- Qt-free core (`src/core/runtime/`, namespace `fv`, no Qt):
  - `JobSlot` (claim/settle/finish, per-job cancel token, flags) and job state.
  - `HoldStateMachine` (push-and-hold + latch, pure, driven by timestamps; deadline callback injected).
  - Status/notice string builders (batch, delivery, walkthrough copy, meeting messages, refusals).
  - `SelectionDecision` (refusal/known/terminal/copy-line rules) given a focus snapshot.
  - Backoff/schedule helpers (remote retry, updater recheck).
  - URL helpers (redact, history target).
- Qt layer (`src/app/runtime/`): `TranscriptionRuntime`, `CommandRuntime`, `HotkeyRuntime`,
  `StreamingRuntime`, `ModelRuntime`, `ComponentRuntime`, `UpdaterRuntime`, `WalkthroughRuntime`,
  `MeetingRuntime`, (`RemoteRuntime` if kept) as QObjects owned by the controller; worker pools as
  `QThreadPool` instances sized 2 (jobs), 1 (persistence), 1 (components/updates), 1 (walkthrough library).
  The engine itself stays on `EngineWorker`'s thread and is reached through queued calls (CLAUDE.md);
  the job worker posts decode requests to it and waits on a future.

**Order of work**
1. Job slot + cancel tokens + status builders (core, golden-tested).
2. TranscriptionRuntime dictation path: start/stop/post-roll/save/decode/deliver/history/failure, with
   paste/copy (needs a05 recorder, a06 paste, a09 history). Upload + batch next.
3. HotkeyRuntime + push-and-hold + watchdog (needs a06 hooks).
4. StreamingRuntime window preview over Parakeet + preview fallback.
5. Cleanup/filters/routing/shaping/Command Mode/transforms (needs a12, a19, a06 focus).
6. ModelRuntime/ComponentRuntime adapted to Parakeet model + NeMo runtime downloads.
7. WalkthroughRuntime + walkthrough job (needs a22/a23).
8. MeetingRuntime (needs a24-a31; largest dependency fan-out).
9. UpdaterRuntime (needs a16). RemoteRuntime only if decided.

**Risks**
- Cancel of an in-flight Parakeet decode: the C ABI has no abort; long files/meeting redecode can't be
  interrupted mid-call. Mitigate with chunked decoding and cancel checks between chunks.
- The engine is single-threaded: preview windows, incremental early decoding, final pass and meeting
  ASR all compete for one Recognizer; ordering/latency must match Python's "final pass starts at once,
  preview tail decoded only if empty" behavior.
- Subtle threading invariants (settle under lock, new cancel Event per job, capture lock non-blocking
  for rewrites, generation/claim filtering of meeting retry events) are easy to lose; port the tests.
- Large dependency fan-out: this area cannot be finished before a05, a06, a09, a12, a19, a22, a24+ exist;
  stub interfaces early.
- Many exact user strings; product name "OpenWhisper" appears in messages (decide OpenWhisper_CPP wording).

**Rough C++ size**: ~8,500 lines (transcription ~3,000; meeting ~2,000; walkthrough ~800; hotkeys ~600;
models+components ~700 after drops; command ~450; streaming ~250; updater ~250; remote ~450 if kept;
core helpers + tests extra).

**Native gains**: no Python worker-process isolation or model warmups for tiny.en; in-process engine
removes IPC and startup cost; shared Recognizer lets meetings borrow dictation weights instead of
release/reload; lower latency from stop to paste (no GIL contention between preview worker and final
pass); precise timers for the hold state machine without the 10 ms polling loop (hotkeys.py:424-426 can
become a wait on a recorder "stream open" condition).

**UI note**: this area has no widgets. The only UI-adjacent contract is the dynamic property
`openwhisperHotkeyWindow` (bool, on top-level windows) and the overlay/caption/notice strings listed
above; layout and styling belong to a02/a07/a08/a23/a31.
