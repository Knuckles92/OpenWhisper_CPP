# Walkthroughs: session, documents (PDF/Word/Markdown), storage, screenshot capture and annotation

Area `a22-walkthrough-services-snap`. Source root `D:\coding\whisper_local`. 28 files, about 8.2k
lines (`services/walkthrough/*`, `ui_qt/snap/*`, `ui_qt/walkthrough_paste.py`). All files were
read in full. Line numbers below are from the checkout as read.

Not in this area (other maps own them, contracts stated in "Dependencies on other areas"): the
walkthrough column/panel, Review/storyboard tab, ready card, library dialog, Snap menu, capture
delay chip (`ui_qt/widgets/capture_delay.py`), write-up prompt/style dialogs, the runtime that
drives a walkthrough (`services/runtime/walkthrough.py`, `services/runtime/transcription.py`), the
installed-agent runner (`meeting/agent/installed/*`), settings storage, palette.

## What the user gets

1. **Walkthrough recording model.** One continuous recording; each kept screenshot closes a
   "section" (words said since the previous screenshot, then the screenshot). Speech after the last
   screenshot is a trailing section. While recording, screenshots can be removed (their words join
   the next section), moved up/down (their words go with them), undone (newest still on the board).
   "Hold for note" stretches are cut out of the document and sent only to an AI write-up.
2. **Section-by-section transcription.** The WAV is cut at each screenshot moment, nudged up to
   0.75 s to the quietest 0.1 s window so words are not split, and each clip is decoded on its own
   (no engine timestamps needed).
3. **Documents written into a capture folder** `captures/YYYY-MM-DD_HH-MM-SS[-N]/`:
   `walkthrough.md` (relative image links, LF, UTF-8), `walkthrough.pdf` (paginated, each
   screenshot section kept on one page, lossless images, hairline frames), `walkthrough.docx`
   (same layout via Word keep-with-next), `walkthrough.json` (manifest v1), plus `shot-N.png`.
   Heading, details line, "about" note, captions and paper size are configurable.
4. **AI write-up** (opt-in): Claude Code / Codex / OpenCode turns the walkthrough into a
   Markdown document; rendered as `write-up.md/.pdf/.docx` in the same look; optional colour/font
   "look" block; earlier write-ups kept as `write-up-N.*`. Named write-up styles (built-in
   "Walkthrough" + 4 starters, up to 30 user styles).
5. **Library services**: reload past walkthroughs, Review (edit words, omit/restore/reorder
   screenshots; keeps `walkthrough.original.json`), edit and save a write-up, retry/redo a
   write-up, crash recovery (`draft.json` + recovered journal WAV), opt-in retention pruning
   (`retention.json`, 0-3650 days), safe delete.
6. **Snap**: hotkey/button screen capture that freezes every screen under a smoked overlay; drag a
   region (with size readout, magnetic snapping to window and screen edges), resize by corners and
   edges, annotate with arrow, box, ellipse, pen, highlighter, text label, numbered step, redact; 4
   colours; undo/redo; Enter keeps, Esc cancels. Also "whole screen" and "window under pointer"
   instant modes, and "draw" mode (whole screen with pen ready).
7. **Live annotations** during a walkthrough: a floating always-on-top toolbar and transparent
   per-screen canvases to draw on the live desktop; marks fade after 1.5 s unless "Keep marks" or
   Ctrl/Cmd-pinned; region selection for automatic captures; screenshot delay chip.
8. **Auto-paste to an agent**: two-step clipboard paste (PDF file, then 800 ms later the agent
   task prompt) into the focused app.

## Feature inventory

Status: **port** unless stated. "undecided backend" is used only where an undecided backend is
involved; the write-up agents are a separate area's backend (installed CLI agents), marked port
with a dependency.

| ID | Feature | Exact user-visible behaviour | Source | Status |
|---|---|---|---|---|
| A22-01 | Capture folder naming | `captures/<YYYY-MM-DD_HH-MM-SS>` under data root, first free `-2`, `-3`...; claimed by `mkdir` (never shared). Created lazily on first screenshot or checkpoint. | `storage.py:new_capture_folder,_names,_claimed`; `session.py:_folder_locked` | port |
| A22-02 | Loose screenshot paths | `captures/screenshots/<stamp>.png`, then `-2.png`...; folder created, file not. | `storage.py:new_screenshot_path` | port |
| A22-03 | Session screenshot keep | `add_shot(image, kept_at_s)` numbers shots 1..N as `shot-N.png`, kept_at clamped >=0; PNG written on a single background writer thread; returns None once closed; raises `ScreenshotNotSaved` if folder cannot be made (recording continues). | `session.py:WalkthroughSession.add_shot` | port |
| A22-04 | Failed PNG write | Partial file deleted; `on_save_failed(number)` callback on writer thread; shot leaves the board like a removed one; `lost_shots` counts them. | `session.py:_write_png,_write_shot,lost_shots,storyboard` | port |
| A22-05 | Board edits while recording | `remove_shot(n)` (words prepend to next section, file deleted after its write), `move_shot(n, ±1)` (trailing section always last), `undo_last()` (largest kept index still on board). All false/None once closed. | `session.py:remove_shot,move_shot,undo_last,_remove` | port |
| A22-06 | Storyboard | Cuts only where adjacent pieces in a section are not consecutive; sections as piece tuples; `section_at(t)`. Exact vectors pinned by tests. | `session.py:_storyboard,Storyboard` | port |
| A22-07 | Hold for note | `hold_note(t)`/`release_note(t)`; zero/negative-length released stretch dropped; still held at close runs to `inf`; ignored after close. | `session.py:hold_note,release_note,close` | port |
| A22-08 | Crash checkpoint | `draft.json` (v1, started_at, journal_id, on-board shots in kept order, notes_s with `null` for open end) rewritten atomically after every shot write/delete/note change/preserve_audio. | `session.py:_checkpoint,checkpoint_recording` | port |
| A22-09 | Preserve audio | Copies the recording WAV to `<folder>/audio.wav` via `.audio.tmp` + atomic replace. | `session.py:preserve_audio` | port |
| A22-10 | Discard / end | `discard()` (cancel during record/transcribe) deletes shots and every known doc name, rmdir if empty; `end()` lets writer finish, keeps files. | `session.py:discard,end` | port |
| A22-11 | Cut recording into section clips | REACH 0.75 s, window 0.1 s step 0.05 s, move only if window energy <= 0.5 x energy at mark; cuts never decrease nor pass next mark; clips < 0.25 s are not decoded; 16-bit PCM only (else ValueError). Held stretches removed from sections and cut exactly as their own clips. | `sections.py:cut_frames,_quietest,split_recording,clip_stretches,_without` | port |
| A22-12 | Section transcription | One decode per clip, then per note clip; texts joined by space per section's pieces; notes attributed via `section_at(start)`; on cut failure the whole WAV is decoded into section 0 and the rest "" (no notes). `before_each(i,total)` progress/cancel hook. | `finish.py:transcribe_sections` | port (adapt to Parakeet, see Engine) |
| A22-13 | History text | Sections joined with blank lines and `[Screenshot N]` markers; raw (pre-cleanup) kept only if different (only `build_documents` computes this; `library.load_result` always returns `history_raw=None`, and its markers are emitted after each section that has a shot, so omitted shots get none). | `finish.py:marked_text,build_documents`; `library.py:load_result` | port |
| A22-14 | Build documents | Writes md, pdf, docx, manifest; deletes `draft.json`; clipboard text = Markdown with absolute image paths. | `finish.py:build_documents` | port |
| A22-15 | Error status suffix | `" — screenshots kept in <folder>"` when a failed walkthrough kept shots. | `finish.py:kept_note` | port |
| A22-16 | Markdown document | `# <title>`, then `_<details>. <about>_`, then each section's words (block markers backslash-escaped) and `![Screenshot N (m:ss)](shot-N.png)`. Empty sections dropped. Details: `Tue, Oct 20, 2026, 2:32 PM · 2:16 of speech · 3 screenshots` ("no screenshots", "1 screenshot"). About: "Spoken notes with screenshots, transcribed by OpenWhisper." + (if shots) " Each screenshot comes right after the words that describe it." | `document.py:markdown,_meta,_about,_escape_markers,_image_target,clock,_when` | port (wording: brand name, see open questions) |
| A22-17 | Manifest v1 | `walkthrough.json` sections incl. `omitted_shot`, `notes`; `started_at` ISO with UTC offset; atomic write. | `document.py:write_manifest` | port |
| A22-18 | PDF of a walkthrough | Letter (US locale) else A4; margins 0.75/0.7 in; fonts Segoe UI / Noto Sans / DejaVu Sans; title 22 pt bold; meta 9.5 pt #6b645c; words 11 pt 145 % line height; captions 8.5 pt #8a8278 centred `Screenshot 2 · 1:12`; images max 62 % of text height, 1 px = 1/96 in; hairline frame #dcd5cb 0.6 pt; section kept on one page (move to new page, else shrink image to >= 50 %); first section stays with the title; PDF title `<title> · <when>`, creator "OpenWhisper"; lossless images; text drawn line by line (no duplicated text across page breaks). | `document.py:write_pdf,_Composer,_Pages,_paint_pages,_paint_page` | port |
| A22-19 | PDF of any Markdown (write-up) | GitHub Markdown, no HTML; list markers written as text with hanging indent 16 pt; code shading #f5f1eb pad 4 pt; quote bar 9 pt left; HR rule; table rules (header rule doubled); heading spacing H1 0/H2 18/H3 14/others 12 pt, gap 5 pt below; body 140 %; images only from folder, missing ones removed; lone image centred with 12 pt above/below; a screenshot keeps the paragraph + heading before it on its page; Look colours/fonts. | `document.py:write_markdown_pdf,_MarkdownComposer,_paint_marks,_paint_table_rules` | port |
| A22-20 | Word document | Same composer output copied to .docx: page size/margins in pt, Normal font = resolved default family/size, space-after 0, single spacing; keep-with-next for section blocks and headings; outline levels for headings; HR bottom border, code shading, quote left border, table borders; images PNG at PDF size with #DCD5CB 0.6 pt outline and alt text; hyperlinks; core props title, empty author, comment "Made by OpenWhisper". | `word.py:write_docx,write_markdown_docx,_write,_Writer` | port |
| A22-21 | Document format settings | Title (default "Walkthrough", "" omits), details, about, captions (bools default true), page size auto/letter/a4. | `finish.py:document_settings`; `document.py:DocumentFormat` | port |
| A22-22 | AI write-up | Agent per setting (Claude Code reads PDF fenced to folder; Codex/OpenCode get Markdown + attached images, no files); timeout 300 s, stall 150 s; charter + instructions; held notes appended; reply unfenced, look block parsed, images restricted to our shot names; failures raise `WriteUpFailed` with message ("canceled", "<Agent> failed", "<Agent> sent back no document", "could not write its documents: ..."). | `write_up.py:write_up,document_text,_split_look,_notes,_reading_prompt,_viewing_prompt` | port (depends on installed-agent area) |
| A22-23 | Write-up versions | New write-up moves existing `write-up.*` to `write-up-<n+1>.*` (copy2 keeps mtime) in the same atomic publish; earlier versions listed and editable in place. | `write_up.py:_write`; `library.py:earlier_write_ups,write_up_names,save_write_up` | port |
| A22-24 | Write-up styles | Built-in "Walkthrough" (DEFAULT_INSTRUCTIONS) first; starters "Step-by-step guide", "Bug report", "Design feedback", "Task for an agent" until the list is saved; max name 60, max 30 styles; errors "Give the style a name.", "Use a name with 60 characters or fewer.", "Add the instructions for the write-up you want.", "\"Walkthrough\" is the built-in style. Choose another name.", "You can keep up to 30 styles. Delete one first."; case-insensitive name replace; id = 12 hex chars of uuid4. | `styles.py` | port |
| A22-25 | Load past walkthrough | Manifest v1 validated (shot names `shot-[1-9]\d*.png`, regular local files, no symlinks, metadata <= 8 MiB, finite non-negative times); markdown image paths rebuilt for this machine; unknown images removed. | `library.py:load_document,load_result,read_json,local_file,_shot_file` | port |
| A22-26 | Review save | Edit texts, keep/omit each screenshot, reorder (`order`); stale shape -> "The document changed; reopen Review"; renders all four docs in a staging dir, keeps first `walkthrough.original.json`, publishes atomically with rollback; write-ups untouched. | `library.py:save_review,publish_files` | port |
| A22-27 | Write-up part editing | Split write-up Markdown at standalone image lines and rejoin in new order; identify which line is one of our shots. | `library.py:write_up_parts,join_write_up,write_up_shot` | port |
| A22-28 | Retry write-up | From the as-spoken doc with given or settings instructions, current agent and notes setting, held notes from manifest. | `library.py:retry_write_up` | port |
| A22-29 | Crash recovery | At startup, join drafts with `recording_*-recovered-<32hex journal>.wav` (exactly one match) as `audio.wav`; rebuild a closed session from `draft.json` (only readable PNGs; times monotone; notes ordered). | `library.py:attach_recovered_audio,recovery_session` | port |
| A22-30 | Retention | `retention.json {"days": int}`, 0 = forever, 0-3650 ("Keep captures for 0–3650 days; 0 means forever"); prunes only finished folders (manifest, no draft) whose manifest mtime is older than cutoff. | `library.py:retention_days,set_retention,expired_entries,prune` | port |
| A22-31 | Safe delete | Refuses symlinks and non-matching folder names ("Not a walkthrough folder"), and any unexpected entry, including any sub-directory such as a leftover `.previous` or staging dir, or a file not in the fixed set / `shot-N.png` / `write-up-N.*` ("Folder contains extra files; remove it in your file manager"). Fixed set: draft.json, audio.wav, walkthrough.{json,md,pdf,docx,original.json}, write-up.{md,pdf,docx}. | `library.py:delete_entry` | port |
| A22-32 | Clipboard formats | `pdf` (file), `word` (docx file), `both` (pdf file + Markdown text), `agent` (Markdown text, legacy), `agent_task` (no files; fixed prompt + "PDF file (if the attachment is unavailable): <abs path>"). | `session.py:WalkthroughResult.clipboard` | port |
| A22-33 | Two-step auto-paste | Preconditions with report strings (Accessibility, focus target, PDF exists, modifiers held); writes PDF with a unique marker, sends paste, after 800 ms checks focus, clipboard marker, modifiers, then pastes the prompt; "PDF and instructions pasted; review them before sending". | `walkthrough_paste.py:WalkthroughPaste` | port |
| A22-34 | Latest completed walkthrough | Newest folder with a manifest (drafts skipped). | `walkthrough_paste.py:latest_result` | port |
| A22-35 | Snap session | `start_snap(keep_out, hint, float_above, parent, take, front, click_at, screen)`; Qt thread only; exactly one of kept/dropped/failed, always after return; failures "Couldn't capture the screen. Try again in a moment." / "There's no screen to capture." / support reasons. | `snap/session.py` | port |
| A22-36 | Multi-screen freeze | Grab every screen (pointer's first) before showing UI; overlay per screen; selecting on one clears others; Esc anywhere drops all; screens that fail to grab are left uncovered. Hyprland: focused screen only. | `snap/session.py:_start,_capture,_open,_on_selecting`; `capture.py:screens_to_cover` | port (Hyprland: other-platform only) |
| A22-37 | Hiding our windows | Hide `keep_out` top-levels (not already excluded, visible, not minimised), wait settle time (max(2 frames, 34 ms; Hyprland 200 ms; macOS 300 ms)), grab, restore without activating. Parks `float_above` windows in a corner of the overlay screen. | `capture.py:hide_windows,restore_windows,settle_ms`; `snap/session.py:Parkable` | port |
| A22-38 | Instant modes | `screen` keeps pointer's screen; `window` crops the window under pointer (or foreground with `front` / pointer on our window); `draw` opens overlay with pen on whole screen. | `snap/session.py:_captured`; `pointed.py:window_under_pointer` | port (window: Windows/macOS only) |
| A22-39 | Overlay selection | Smoked dim `rgba(18,12,9,150)`; drag >= 6 px both ways; double-click with no selection selects whole screen; size badge `W × H` in device pixels while selecting/resizing; corners/edges grab (10 px outside, 4 in, 16 corner reach) with resize cursors; flips past far side; min 6 px; marks pinned to screen, not region; drag outside an unmarked selection restarts; hint pill top-centre else bottom-centre. | `overlay.py:SnapOverlay`; `geometry.py` | port |
| A22-40 | Edge snapping | 10 px magnet to screen and visible window frames (corner favoured); toggled with S / toolbar, persisted `snap_to_edges` (default true); applies to selection and box/ellipse/arrow points. | `geometry.py:snap_point`; `pointed.py:window_targets` | port |
| A22-41 | Annotation tools | Arrow (4 px, bowed 10 % max 26 px, upward/left), Box (3 px, radius 6, optional 24-alpha infill), Ellipse, Pen (4 px, smoothed, jitter < 1.5 px skipped, click = dot), Highlighter (18 px, alpha 120), Text label (16 px DemiBold tag, pad 8x3, radius 6), Step (radius 13 + 2 white ring, auto-number), Redact (#181A20 opaque, pixel-aligned, ignores colour). Shift: squares/circles, 45° arrows, clamped to bounds. Shadow `rgba(0,0,0,70)` offset 1.2 px. | `annotations.py` | port |
| A22-42 | Colours | Coral #FF4F3A, Amber #F5A524, Blue #3A7BFD, Green #1FA463; ink white unless luminance >= 0.3 then #2A1A06. | `annotations.py:SWATCHES,ink_for` | port |
| A22-43 | Undo/redo | Stack; new mark clears redo; step numbers come back on undo; undo while typing discards the label; redo blocked while typing. | `annotations.py:AnnotationStack`; `overlay.py:undo,redo` | port |
| A22-44 | Keyboard | A B O P H T N R tools; 1-4 colours; S snap; Enter keep; Esc drop; Ctrl+Z undo; Ctrl+Y / Ctrl+Shift+Z redo; any other modifier disables letter shortcuts. Label editor swallows Enter (commit), Esc (cancel label only), Tab. | `overlay.py:_shortcut,LabelEditor.keyPressEvent` | port |
| A22-45 | Kept image | Device-pixel crop of the frozen image (edges rounded outward), marks painted with painter scaled by DPR and clipped to selection; result carries the screen DPR; no subpixel text AA. | `export.py:render`; `geometry.py:device_rect`; `annotations.py:grayscale` | port |
| A22-46 | Snap toolbar | Painted single widget; button 34, swatch 26, icon 20, pad 6, separator 13, radius 12; wraps whole groups on narrow screens; tooltips (see Modules); disabled states 0.32 opacity. | `toolbar.py:SnapToolbar,_layout,_wrap` | port |
| A22-47 | Vector icons | 20x20 grid glyphs: arrow, box, ellipse, redact, pen, highlighter, text, step, undo, redo, keep (check), drop (cross), snap/region, clear. | `icons.py` | port |
| A22-48 | Overlay chrome | Hint pill (caller hint elided + "Enter to keep · Esc to cancel" keycaps; "S toggles snapping" appended), frame with viewfinder corners, size badge, soft 5-step shadow. | `chrome.py` | port |
| A22-49 | Live annotation toolbar | Floating no-focus toolbar at top centre (8 px) of pointer's screen; tools toggle on/off; "Infill", "Snap", "Region", delay chip, "Keep marks", "Clear marks", × (disabled with explanatory tooltip when nothing to drop); kept out of captures. | `live.py:LiveAnnotations`; `toolbar.py` (live) | port |
| A22-50 | Live canvases | Per-screen transparent click-through windows; armed = input, 1-alpha fill on Windows; marks expire after 1.5 s (PreciseTimer) unless pinned (Ctrl/Cmd) or Keep marks; expiry paused while a delayed capture is pending; captured clears only that screen's temporary marks; C clear, K keep marks, S snap, Esc cancel. | `live.py:_Canvas` | port |
| A22-51 | Auto-capture region | "Drag a capture region · Esc to cancel" badge; region >= 4 px; click-through frame; 24x24 × control above its top-right ("Close region · capture the whole screen"); cleared on screen geometry change or removal. | `live.py:set_region,clear_region,_RegionClose` | port |
| A22-52 | Pinned marks for auto-captures | `pinned_at(click)` / `pinned_on(screen)` returns the canvas's pinned marks for baking into the shot. | `live.py:pinned_at,pinned_on` | port |
| A22-53 | Capture exclusion (Windows) | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE=0x11)`; remembers HWND in a property so a recreated handle reads as not excluded. | `affinity.py` | other-platform only (Windows) |
| A22-54 | Foreground forcing (Windows) | AttachThreadInput + SetForegroundWindow so the overlay gets keys; returns foreground to previous app after live drawing. | `affinity.py:bring_to_front,_force_foreground,return_foreground` | other-platform only (Windows) |
| A22-55 | macOS window plumbing | Screen Recording grant check/request (once per run), settings URL, NSWindow level 26 cover / 27 parked, all-Spaces, nonactivating panels, no animation, focus return (cooperative activation 14+). | `macos.py`; `support.py:request_access` | other-platform only (macOS) |
| A22-56 | macOS filtered capture | ScreenCaptureKit (macOS 14+) excludes our windows natively for automatic SCREEN captures; 5 s timeout; callbacks marshalled to GUI thread; late results ignored. | `macos_capture.py` | other-platform only (macOS) |
| A22-57 | Hyprland capture | `grim -o <output> -` with 2 s timeout, output found via hyprctl monitors (name, else position, else focused). | `grim.py`; `capture.py:_grab_screen` | other-platform only (Linux Hyprland) |
| A22-58 | Support check | Windows/X11 yes; macOS needs grant (long MAC_REASON); Hyprland needs grim (GRIM_REASON); other Wayland no (WAYLAND_REASON); else OTHER_REASON. | `support.py:snap_support` | port |

### Added by check (Feature inventory)

- **Session result carrier.** `WalkthroughSession` also carries `result`, `write_up_input` (screenshot paths + held notes for a later write-up) and `document_error` (why only words were kept), set by the runtime's job worker; `on_save_failed` is public. `WalkthroughResult.name`/`docx` and `WalkthroughSession.open`/`shot_count`/`saved_shots(timeout)` (board-order list of shots whose PNG was written; waits up to `SAVE_WAIT_S`=30 s per save and per delete, a timeout counts as unwritten) are public API used by the runtime.
- **Checkpoint triggers.** `hold_note`/`release_note` queue a checkpoint (only once a writer exists); `checkpoint_recording(journal_id)` creates the folder and writer at recording start and links the draft to the recorder journal; `preserve_audio` skips the copy when source resolves to the destination.
- **discard() scope.** Deletes the fixed names only; earlier write-up versions `write-up-N.*` and temp files are not touched, so the folder is kept (rmdir fails quietly) if any remain.
- **Write-up details.** The viewing (Codex/OpenCode) path reads `walkthrough.md` and fails with `could not read walkthrough.md: <err>` if unreadable; prompt shapes: reading = instructions, blank, `The walkthrough: <realpath pdf>`, optional `Its screenshots, beside it: a, b`, `It is <clock> of speech.`; viewing = instructions, blank, optional `The screenshots attached, in order: ...`, duration line, blank, `<walkthrough>` markdown `</walkthrough>`. `save_write_up` always rebuilds with an empty `Look` (the agent's look is not persisted) and the edited text trimmed; an earlier version is only saved in place if it is listed by `earlier_write_ups`. `agent_name(agent)` resolves the display name from the installed-agents table. A successful `_write` resets `write_up_error` and sets `written_up`.
- **Version rule.** Only `write_up()` (agent output) passes `keep_latest`; versions are numbered `max(earlier)+1` (or 1) and only the files that exist are copied.
- **Style helpers.** `load_styles` (default + saved), `style_named` (case-insensitive, trimmed), `to_dict`; a style with any blank field is skipped on load; the starter list is returned (not persisted) until the key exists, and `delete_style` on the starters materialises the remaining ones.
- **Recovery errors.** `recovery_session` requires `audio.wav` to exist as a safe local file and `version == 1` ("Unsupported draft version"); screenshot entries whose PNG is missing/unreadable are skipped (not errors); notes must be non-overlapping and ordered ("Unordered note times"), screenshot times non-decreasing ("Unordered screenshot times"). `attach_recovered_audio` skips drafts that already have a manifest or `audio.wav`, requires a 32-hex lowercase `journal_id`, and ignores per-folder errors; it returns the count attached.
- **Load past walkthrough errors.** `ValueError` messages: "Unsupported walkthrough version", "Invalid screenshot name", "Invalid walkthrough notes", "Invalid walkthrough time", "Invalid walkthrough filename", "Missing or unsafe walkthrough file: <name>", "Unsafe or oversized walkthrough metadata", "Walkthrough metadata must be an object". An omitted shot whose PNG was deleted by hand loads as no omitted shot (not an error); a section that has both `shot` and `omitted_shot` keeps only `shot`.
- **Retention preference.** `retention_days` treats any non-int (including bool) or out-of-range value, or an unreadable file, as 0 (keep forever); `set_retention` creates the captures root if missing.
- **Review save details.** Copies each included or omitted PNG into the staging dir so the documents render from there; `publish_files` backs up targets (md, pdf, docx, manifest) to `.previous` and restores them on `OSError`; the return value is `load_result(spoken=True)`.
- **Auto-paste ownership.** `WalkthroughPaste` always sends the `AGENT_TASK` prompt regardless of the saved copy format; refuses when the target `is_self`; `cancel()` is public and resets token/target/prompt; the marker check compares clipboard MIME bytes to the token. `latest_result()` returns `library.load_result(folder)` (so the latest write-up if there is one) and lets load errors propagate ("report broken exports").
- **Live annotation extras.** `LiveAnnotations(parent, panel, mark_shortcuts={"keep_marks":..., "clear":...})` takes the user's shortcut strings for the toolbar tooltips; the delay chip is created only on the live toolbar. No undo/redo in live mode; Step numbers are per-canvas (`_next_step`) and reset by Clear; Clear and Esc both emit `capture_cancelled` (cancelling a pending delayed capture); `drawn(screen, marks)` emits all marks of that canvas; snapping in live applies only to Arrow/Box/Ellipse points; marks are clamped to the canvas. When a delayed capture is cancelled or fails, temporary marks get a fresh 1.5 s lifetime (not cleared); a successful capture drops that screen's temporary marks only. `set_enabled(True)` re-reads `snap_to_edges`; `_sync_toolbar` re-reads `snap_box_infill` each time; the toolbar is placed top-centre on the screen under the pointer when shown and re-placed if that screen is removed.
- **Snap session extras.** `active` property; `capture_rect` global logical rect; `take=window` with no window found keeps the whole screen; `draw`/`screen`/`window` use only the first screen; Enter on the overlay includes keypad Enter; `snap_to_edges` is persisted by the session when toggled and `snap_box_infill` by the overlay; failures are emitted from a 0 ms timer; a session object deleted mid-snap restores hidden windows and parks.

## Modules

### services/walkthrough/__init__.py
Docstring only. C++: none.

### services/walkthrough/storage.py
- Responsibility: path names. Constants `CAPTURES_FOLDER="captures"`, `SCREENSHOTS_FOLDER="screenshots"`, `MARKDOWN_NAME="walkthrough.md"`, `PDF_NAME="walkthrough.pdf"`, `DOCX_NAME="walkthrough.docx"`, `MANIFEST_NAME="walkthrough.json"`.
- `captures_root()` = `config.data_root()/captures`. `new_capture_folder(started_at)` claims by `mkdir` (race-safe, :66-72). `new_screenshot_path(taken_at)` does not create the file (:46-55); time format `%Y-%m-%d_%H-%M-%S` local time.

### services/walkthrough/session.py
- Public: `ScreenshotNotSaved(OSError)`, `Shot(number, path, kept_at_s)`, `Storyboard(shots, cuts_s, sections, notes_s, section_at())`, `Note(text, section)`, `WalkthroughResult` (folder, pdf, markdown, clipboard_text, history_text, history_raw, shots, duration_s, first_shot, written_up, write_up_error, lost_shots; `name` -> "Write-up"/"Walkthrough"; `docx`; `clipboard(format)`), `WalkthroughSession`, `SAVE_WAIT_S=30.0`.
- Threading: one `threading.Lock` guards everything; Qt thread calls `add_shot` / board edits / notes; one-thread `ThreadPoolExecutor("walkthrough-shots")` writes PNGs, deletes removed shots and writes checkpoints in FIFO order (a delete is queued after its own write, :318-319); job worker calls `storyboard()` which waits on each save future with timeout.
- Board model (`_Section(pieces, shot)`): lazily materialised (`_board_locked`, :295-300) as one section per shot plus trailing. `add_shot` after the board exists closes the trailing section and appends a new one (:368-370). `_remove` prepends removed pieces to the next section. `move_shot` refuses moving into/over the trailing section (:340).
- `_storyboard` (:102-127): a cut index i is dropped when some section has pieces i, i+1 adjacent; pieces renumbered via `bisect_left(cuts, clip)` and de-duplicated per section. Tests pin exact vectors (see Tests).
- `add_shot` copies the QImage before handing it to the writer (:373). Folder creation failure maps to `ScreenshotNotSaved(errno, strerror)` (:360-364).
- `_checkpoint` (:385-404): draft JSON v1 `{version, started_at (naive isoformat), journal_id, shots:[{shot,kept_at_s}] sorted by kept index of on-board shots, notes_s:[[start, end|null]]}`; held-but-open note written with null; failures logged, not raised.
- `discard()` (:477-502) deletes shots + names {walkthrough.md/.pdf/.json/.docx, write-up.md/.pdf/.docx, draft.json, audio.wav, walkthrough.original.json}, then `rmdir` (kept if not empty). Waits for the writer (`shutdown(wait=True)`). `end()` does `shutdown(wait=False)`.
- `clipboard()` agent prompt text (:178-188) is a user-visible string; port verbatim (with brand replaced if decided).

### services/walkthrough/sections.py
- Pure numeric. Constants `REACH_S=0.75`, `FRAME_S=0.1`, `QUIET_RATIO=0.5`, `MIN_CLIP_S=0.25`, 16-bit only.
- `cut_frames(samples, rate, marks_s)` (:43-64): target = clamp(round(mark*rate), floor, total); ceiling = next mark's target (not below this target); search `[max(floor,target-reach), min(ceiling,target+reach))`; floor = previous cut.
- `_quietest` (:67-86): window width `int(0.1*rate)`, step `width//2`, energy = sum of squared int16 (summed over channels) via cumulative sums; pick min energy, tie -> nearest middle to target (`lexsort((distance, energy))`); keep target unless min energy <= 0.5 x energy of the window nearest target. If range < width -> target clamped.
- `split_recording` returns N+1 clips named `section-<i>.wav`, each `[edge_i, edge_i+1)` minus note gaps (`_without`); `clip_stretches` one per stretch `note-<i>.wav`, frames from `round(s*rate)` clamped, `inf` -> end. Clip shorter than 0.25 s -> `Clip(None, duration)` (no file).
- Raises ValueError for non-16-bit (message "expected 16-bit audio, got N-bit").

### services/walkthrough/finish.py
- `transcribe_sections(audio_path, board, transcribe, before_each)` (:27-63): temp dir prefix `openwhisper-walkthrough-`; fallback on `(OSError, ValueError, wave.Error)` -> one decode of the whole file, `before_each(1,1)`; else total = clips + note clips, `before_each(i,total)` before each (may raise to cancel); None-path clips decode to "". Section text = space-join of non-empty piece texts in board order.
- `marked_text`, `build_documents` (order: md, pdf, docx, manifest, delete draft), `document_settings(settings=None)` -> `(DocumentFormat, QPageSize|None)`, `kept_note`.

### services/walkthrough/document.py
- Public: `TITLE="Walkthrough"`, `MANIFEST_VERSION=1`, `Section(text, shot, kept_at_s, omitted, notes)`, `Walkthrough(started_at, duration_s, sections)`, `DocumentFormat(title, details, about, captions)`, `Look(text_color, heading_color, font)`, `clock`, `markdown`, `write_markdown`, `write_manifest`, `write_pdf`, `write_markdown_pdf`, `page_layout`. Private helpers reused by `word.py`: `_Composer`, `_MarkdownComposer`, `_Group`, `_layout_dpi`, `_text_area`, `_when`, `_release`, colour/size constants.
- Threading: must run on a worker thread with only `QGuiApplication` existing; uses QImage/QPainter/QTextDocument/QPdfWriter only, never QPixmap/widgets (:8-14). `_release` (:516-538) moves a worker-built QTextDocument to the main thread for deletion: a PyQt wrapper issue; in C++ it is unnecessary (delete where built), but keep the rule "no QPixmap, no widgets".
- Text helpers: `_tidy` normalises CR/CRLF to LF and strips; `_paragraphs` splits on blank lines; a single LF inside a paragraph becomes U+2028 in PDF (:797). `clock` floors and clamps negatives to 0, `h:mm:ss` from 1 h. `_when` = `%a, %b D, YYYY, h:MM AM` (English month/day names; strftime is locale-dependent in Python but C locale on Windows gives English; port with fixed English names).
- `_BLOCK_MARKER` regex `^([ \t]*)([#>+*-]|\d{1,9}[.)](?=\s|$))` multiline; escape inserts `\` before the last char of the marker (:281-289).
- `_image_target` (:292-303): relative if inside folder (POSIX slashes), wrapped in `<>` if it contains whitespace or parens; absolute always `<abs path>` with native separators.
- Markdown header: `_<details>. <about>_` joined by ". " (:271); output ends with a single `\n`.
- Manifest (:323-354): `started_at` = `astimezone().isoformat(timespec="seconds")` (local with offset), `duration_s` float, section dict `{text, shot (relative name or abs path), kept_at_s, [omitted_shot], [notes]}`; keeps sections with shot, omitted, text or notes. Written with `library.atomic_json` (indent 2, ensure_ascii False, allow_nan False).
- PDF layout constants (:365-413): margins in inches `QMarginsF(0.75, 0.7, 0.75, 0.7)`; `_MAX_IMAGE_HEIGHT=0.62`, `_MIN_IMAGE_SCALE=0.5`, frame `#dcd5cb` 0.6 pt, `_SLACK=0.5` layout units, list hang 16 pt, code fill `#f5f1eb`, code pad 4 pt, quote gap 9 pt, bullets disc U+2022 / circle U+25E6 / square U+25AA; ink `#1d1b19`, muted `#6b645c`, caption `#8a8278`. Styles: title 22 bold; meta 9.5 space-before 3; about 9.5 space-before 1; words 11 space-before 16 line-height 145 %; more-words 6; image space 6 centred, lone image 16; caption 8.5 space 3 centred.
- `write_pdf`: `QPdfWriter`, page size explicit or `_local_page_size()` (Letter if `QLocale::system().measurementSystem()==ImperialUSSystem`, else A4), title `"<title or Walkthrough> · <when>"`, creator "OpenWhisper". Layout DPI = `QImage(1,1).logicalDpiY()` (96 Win/Linux, 72 mac). Text area floored to whole layout units (:551-558). `setUseDesignMetrics(true)`, document margin 0.
- Painting (:561-707): `LosslessImageRendering`; scale resolution/dpi; per page: paint marks (HR line across, code fill, quote bar, table rules with the 2nd edge doubled), then every `QTextLine` whose top lies on the page (not `drawContents`, to avoid duplicated text in the PDF), then image frames. `painter.begin` failure -> `OSError("Could not write <path>")`.
- `_shown_size` (:710-721): natural = pixels x unit x 72/96; scale = min(1, width/natural.w, height x 0.62/natural.h).
- `_Composer` (:742-861) and `_Pages` pagination (:871-1038): per block in order, `_flush_top` trims a top margin that straddles a page break; for each screenshot group: keep if on one page; else (unless group starts at a page top) try a forced page break with top margin 0 plus shrink, revert if it still does not fit; else shrink image to fit below words on their page (only if 0.5 <= scale < 1; never for a lone image). Group for the section under the header starts at block 0 (title). Unreadable image: logged, words and caption still added, no group if no caption (:805-806). `_layout` raises RuntimeError on an empty layout (process crash guard).
- `_MarkdownComposer` (:1088-1275): `setMarkdown(GitHub | NoHTML)`; `_drop_blank_cell_lines`; `_write_list_markers`; `_style_block` (headings, lone images, body 140 %), `_color_words` (skip runs with own foreground, i.e. links); `_lead_in` grouping; `_size_images` loads `folder/name`, removes missing ones, width limited by indent and table column share.

### services/walkthrough/word.py
- `write_docx`, `write_markdown_docx`; `_write` sets section page size/margins in points from `page_layout`, Normal style font from `QFontInfo(default).family()`, size 11, space after 0, line spacing 1.0; core props (title, author "" , last_modified_by "", comments "Made by OpenWhisper").
- `_Writer`: paragraph space-before/after = block margins / unit; proportional line height -> multiple; left indent = leftMargin + indent x indentWidth; negative text indent -> first-line indent (hanging list marker, tab char kept); alignment centre/justify/right; keep_with_next for headings and all blocks of a group except its last; pPr children must follow `_PARAGRAPH_ORDER` (Word rejects otherwise, :50-61); HR -> bottom border sz=`max(2, round(0.6*8))`=5; code -> `w:shd fill F5F1EB`; quote -> left border space 9; heading -> `w:outlineLvl` = min(level,9)-1. Runs: drop U+FFFC, U+2028 -> line break; size only if != default; bold if weight >= 600; italic/underline/strike; colour if set; font name only if fixed-pitch or families set and different from default; hyperlinks as external relationships. Tables: borders top/bottom/insideH hairline, row 0 bottom border doubled, cell paragraphs 2 pt before/after, the default empty paragraph removed. Pictures: PNG bytes, size pt = layout units / unit, alt text from `ImageAltText`, `a:ln w=7620` (0.6 pt x 12700) solid `DCD5CB`.

### services/walkthrough/write_up.py
- Constants `MARKDOWN_NAME="write-up.md"`, `PDF_NAME`, `DOCX_NAME`, `AGENTS = MeetingAgentCore.INSTALLED` (claude_code, codex, opencode cli), `DEFAULT_INSTRUCTIONS`, `_CHARTER`, `_SPEECH`, `_NOTE_LIMIT`, `_SPEECH_WITH_NOTES`, `_HELD_NOTES`, `_READ_SOURCE`, `_VIEW_SOURCE` (all prompt text; port verbatim), `_TIMEOUT_S=300`, `_STALL_S=150`, `WRITE_UP_AGENT_SETTING="Settings → Dictation → Walkthroughs"`.
- Regexes: `_IMAGE` `!\[([^\]\n]*)\]\(\s*(<[^>\n]*>|[^\s)]+)(?:\s+"[^"\n]*")?\s*\)`; `_FENCE` whole reply in ``` fence; `_HEADING` first `# ` heading -> PDF title; `_LOOK` front block of `key: value` lines between `---` lines; keys normalised (lowercase, `[\s_-]+`->space, colour->color) to text color / heading(s) color / font.
- `write_up()` flow (:135-203): speech rule chosen by `follow_notes`; held notes add `_HELD_NOTES` (+ `_NOTE_LIMIT` when not following spoken notes); Claude Code -> `run_reading_task(folder=...)`, others -> `run_viewing_task(images=[realpath...])`; `AgentUnavailable` message has the meeting setting path replaced with the walkthrough one. `_notes` lists "About screenshot N (shot-N.png)", "After the last screenshot", or "In the walkthrough".
- `document_text` keeps only images whose basename (after `\`->`/`) is one of our shot names, rewrites to bare name; returns "" unless the text has a word character.
- `_write` (:305-356): stage in `.write-up-*` temp dir inside folder; optional version shift; write md (+"\n"), PDF, DOCX; `publish_files`; clipboard text with absolute image targets; `OSError/ValueError` -> `WriteUpFailed("could not write its documents: ...")`.

### services/walkthrough/styles.py
- `WriteUpStyle(id, name, instructions)`, `DEFAULT_STYLE("walkthrough","Walkthrough",DEFAULT_INSTRUCTIONS)`, `STARTER_STYLES` (4, ids `step-by-step`, `bug-report`, `design-feedback`, `agent-task`; text verbatim :37-66), `MAX_NAME=60`, `MAX_STYLES=30`.
- `saved_styles` returns starters when the setting is absent (None), skips malformed entries, duplicate ids and the default id, trims, caps at 30. `style_for` matches instructions exactly ("" = default). `save_style`/`delete_style` mutate the settings dict (caller wraps in `mutate_settings`).

### services/walkthrough/library.py
- Constants: `DRAFT_NAME`, `AUDIO_NAME`, `PREFERENCES_NAME="retention.json"`, `_FOLDER` `\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}(?:-\d+)?`, `_SHOT` `shot-[1-9]\d*\.png`, `_EARLIER` `write-up-([1-9]\d*)\.(md|pdf|docx)`, `_FIXED` names allowed in a folder.
- `atomic_json` (mkstemp `.checkpoint-` in same dir, fsync, `os.replace`); `publish_files(stage, folder, names, remove)` backs up existing targets to `stage/.previous`, replaces, rolls back on OSError (:51-79).
- Security posture (port it): `local_file` rejects names with separators, symlinked folder or file, non-regular files; `read_json` rejects symlinks and > 8 MiB, requires an object.
- `load_document` (only place that imports the Qt renderer; startup recovery must not, :121-123), `held_notes`, `written_up` (both write-up.pdf and .md), `write_up_names`, `earlier_write_ups`, `load_result(spoken, version)` (history text uses unstripped `section.text.strip()` and `[Screenshot N]`), `entries(root)` (newest first by name; folder with manifest or draft), `save_review`, `write_up_parts`/`join_write_up`/`write_up_shot`, `save_write_up` ("This walkthrough has no write-up to save"), `retry_write_up`, `attach_recovered_audio`, `recovery_session`, retention functions, `delete_entry`, `prune`.

### ui_qt/walkthrough_paste.py
- `PDF_READ_DELAY_MS=800`. `WalkthroughPaste(clipboard, writer, identity, send_paste, report)`: single-flight (`busy`), random 16-byte token as clipboard marker (`AUTO_PASTE_MARKER_FORMAT` from `ui_qt/clipboard.py`), `writer.write_files([pdf], marker=)`, `writer.write_text(prompt)`. Report strings (verbatim): "Walkthrough not pasted: enable Accessibility permission to auto-paste", "Walkthrough not pasted: focus changed or the destination is unavailable", "Walkthrough not pasted: the PDF is missing", "Walkthrough not pasted: modifier keys are held", "Couldn't paste the walkthrough PDF: {exc}", "PDF paste sent; instructions stopped because focus changed", "...because the clipboard changed", "...because modifier keys are held", "PDF and instructions pasted; review them before sending", "PDF paste sent; couldn't paste instructions: {exc}".

### ui_qt/snap/__init__.py
Re-exports `SnapSession`, `start_snap`, `snap_support`, `keep_out_of_screenshots`.

### ui_qt/snap/support.py
`_platform()` seam; `uses_grim()`; `snap_support() -> (bool, reason)`; `request_access()` asks macOS once per run. Reason strings verbatim (:20-32), MAC_REASON names "OpenWhisper".

### ui_qt/snap/capture.py
`screen_under_cursor` (grim focused screen on Hyprland, else `screenAt(QCursor::pos())` or primary); `screen_at_windows_point` (MonitorFromPoint + origin match: Qt keeps native origins); `screen_at_click`; `screens_to_cover`; `grab` (format coerced to ARGB32_Premultiplied if not RGB32/ARGB32/ARGB32_Premul; DPR = image width / logical width); `settle_ms`; `hide_windows` uses base `QWidget::hide` (subclass hide of the recording overlay would end its session, :141-143); `restore_windows` shows with `WA_ShowWithoutActivating` temporarily set.

### ui_qt/snap/session.py (SnapSession)
- Signals `kept(QImage)`, `dropped()`, `failed(QString)`. Constants `CAPTURE_FAILED`, `NO_SCREEN`, modes `region/screen/window/draw`. Module-level `_live` set keeps sessions alive until outcome.
- State machine: Pending (settle timer / native capture) -> Open (overlays) -> Decided. `keep()` while pending sets `_keep_when_captured`; `drop()` while pending stops timer, cancels native capture, restores windows. Failure is reported via a 0 ms timer (never inside `start_snap`). `_finish` hides overlays, unparks, returns macOS focus, emits, deletes overlays.
- `_start`: `processEvents(ExcludeUserInput)` only for manual snaps; settle timer `settle_ms(screen)` if windows were hidden, 0 otherwise; synchronous capture when nothing hidden and not instant.
- `_open`: hint = caller hint + " · S toggles snapping"; overlays presented in reverse so the pointer's screen gets focus last; `destroyed` of session deletes overlays and restores hidden windows / unparks.
- `capture_rect` (global logical rect of what was kept) is read by callers.
- `Parkable` protocol: `park(area)`, `unpark()`.

### ui_qt/snap/overlay.py (SnapOverlay, LabelEditor)
- Window: `window_flags()` Wayland `Window|Frameless`; Windows/mac `Tool|Frameless|StaysOnTop`; X11 `Window|Frameless|StaysOnTop`. objectName `snapOverlay`, title "OpenWhisper Snap", `WA_OpaquePaintEvent`, `WA_NoSystemBackground`, `WA_QuitOnClose=false`, `WA_MacAlwaysShowToolWindow`, dynamic property `openwhisperHotkeyWindow=true` (hotkey service keys on it), StrongFocus, mouse tracking, CrossCursor. `present()`: macOS show + `macos.cover`, else `showFullScreen`; raise, activate, focus, `affinity.bring_to_front`.
- Phases SELECT/ANNOTATE. `MIN_SELECTION=6`, hint edge 14 px. Signals `kept`, `dropped`, `selecting`, `snap_toggled(bool)`, `box_infill_toggled(bool)`. `keep()` commits label and draft first; whole canvas if no selection. `closeEvent` = drop.
- Painting: black fill if canvas smaller than widget, dimmed image, clear frozen image inside selection with marks (annotate phase), frame, badge (select phase or while resizing), toolbar shadow (painted by the overlay so clicks beside the panel reach the screenshot), hint pill (not over selection or toolbar+8). Partial updates via `_decor_region` and mark bounds.
- `LabelEditor(QLineEdit)`: objectName `snapLabelEditor`; stylesheet `QLineEdit#snapLabelEditor { background: <fill>; color: <ink>; border: none; border-radius: 6px; padding: 3px 6px; font-size: 16px; font-weight: 600; selection-background-color: rgba(<wash>, 0.35); selection-color: <ink>; }` (wash white if ink light else black); placeholder "Label" at 60 % ink alpha; max length 200; width = text advance + 2x8 + 8, min 48, capped by parent right edge - 8; height = font height + 2x3 + 2; `focusNextPrevChild` returns false. `label_origin` centres vertically on the click inside the area.
- Mouse: editor open -> click commits; grab -> resize; `_may_select` -> new selection; else annotation (text opens editor; step adds immediately; others start a draft). Double-click: whole screen if nothing selected, else treated as a press. Cursor shapes per grab (`_RESIZE_CURSORS`), IBeam for text tool inside selection.
- Snapping applies to selection points and to box/ellipse/arrow annotation points (targets include selection); redact points clamped into the selection.

### ui_qt/snap/toolbar.py (SnapToolbar)
- objectName `snapToolbar`; NoFocus; mouse tracking; live variant translucent background. Constants `BUTTON=34, SWATCH=26, ICON=20, PAD=6, SEPARATOR=13, RADIUS=12.0, LABEL_PAD=6, LABEL_GAP=4`; label font = chrome font 12 px.
- Layout order (overlay): 8 tools | Undo, Redo | 4 swatches | Infill (labelled), Snap | Cancel (×), Keep (✓, accent fill, white icon). Live: 8 tools | swatches | Infill, Snap, Region (labelled) | delay chip (width max(148, advance("Delay · Instant")+61)) | Keep marks, Clear marks (labelled), ×.
- Tooltips (verbatim): tools "Arrow (A) · Shift to align to 45° angles", "Box (B) · Shift for a square", "Ellipse (O) · Shift for a circle", "Pen (P)", "Highlighter (H)", "Text (T)", "Step number (N)", "Redact (R) · Drag an opaque cover over private details" (+ " · Hold Ctrl to pin" / "Cmd" on mac in live); "Undo (Ctrl+Z)", "Redo (Ctrl+Shift+Z)", "<Swatch> (n)", infill "Fill new boxes and ellipses with a light tint; off leaves only the outline. Remembered for future screenshots.", "Snap to corners and window edges (S)", region "Select an area for automatic screenshots; close its × to capture the whole screen", "Screenshot delay", "Keep marks (<shortcut or 'K while drawing'>)", "Clear marks (<shortcut or 'C while drawing'>)", live × "Put the tool down and return to the desktop (Esc)", overlay "Cancel (Esc)", "Keep (Enter)"; redact-swatch "Redactions always use an opaque cover."; disabled × "No tool is selected, so there is nothing to put down. Cancel or Done on the walkthrough column ends the walkthrough."
- Enable rules: undo/redo by stack; infill only with box/ellipse; colours disabled with redact; × per `set_drop_enabled`. Activation on release over the same button; double-click = press.
- Paint: panel (overlay-bg opaque, overlay-border 1 px, radius 12, no shadow); separators overlay-border from top+8 to top+26; button bg rounded 8 inset 2; toggled (snap/infill/keep_marks/region) bg `overlay-rgb` alpha 36 + accent icon; active tool tint = swatch colour alpha 34 (46 hover), icon in swatch colour (text colour for redact); hover bg `overlay-rgb` alpha 24; disabled icon text alpha 0.32; labelled checkbox buttons draw a 1.3 px rounded box (inset 3, r 3) with a check when on; swatch: ring r 11 2 px (selected text colour alpha 1.0, hover 0.35), fill r 8 with `rgba(0,0,0,60)` 1 px outline.
- `_wrap`: groups split where the gap >= SEPARATOR; next row at top + 34 + 4.

### ui_qt/snap/annotations.py
Pure vector model plus QPainter paint. See A22-41/42 for constants. Key math: `arrow_shapes` (:165-192: bulge direction rule `_bulge`, control point at midpoint + normal x min(0.1 x length, 26), head = min(4 + width x 3.2, 0.6 x length), half-width 0.62 x head, notch at 0.78 x head, shaft ends at 0.6 x head before tip), `arrow_outline` (stroker union), `smooth_path` (quads through midpoints), `meaningful()` thresholds (MIN_DRAG 6: arrow length >= 6; box/ellipse w,h >= 3 and diagonal >= 6; redact w,h >= 3; stroke always; label non-blank; step always), `bounds()` paddings (used for partial repaint and pinned in tests), `extend_draft` Shift constraints with bounds clamp (:486-510), `AnnotationStack` (`next_step_number` = 1 + count of steps). Fonts: label 16 px DemiBold; step 14 px Bold (11 px from 100). Fonts strip subpixel AA (`grayscale`).

### ui_qt/snap/geometry.py
Pure: `TOOLBAR_GAP=10`, `EDGE_MARGIN=8`, `GRAB_OUTSIDE=10`, `GRAB_INSIDE=4`, `GRAB_CORNER=16`, `SNAP_DISTANCE=10`; `span`, `snap_point` (rank = (axes near, -distance)), `_side`/`grab_at`, `resized`, `device_rect` (floor/ceil with 1e-6 epsilon, intersect image), `place_toolbar` (below, else above, else inside bottom), `badge_rect` (above top-left gap 6, else inside; x clamped 4 px).

### ui_qt/snap/export.py
`logical_rect(image)`; `render(frozen, selection, items)` as A22-45.

### ui_qt/snap/chrome.py
`DIM=rgba(18,12,9,150)`, frame white alpha 235 with black alpha 90 shadow 1 px outside, viewfinder corners 3 px round-cap length min(11, w/2, h/2). `chrome_font(base, px, weight)` = px x UI font scale, min 8. `paint_shadow` 5 rings alpha (34,22,14,8,4) x strength, offset y +2.5. `paint_panel` opaque overlay-bg + overlay-border. `badge_size` = advance + 18, height + 8; badge radius 7 shadow 0.6. `HintPill`: pad 14, gap 7, key pad 6, text font 12.5 px, key font 11.5 DemiBold, height max(text h, key h + 4) + 12, radius height/2, shadow 0.8; keycaps radius 5 with `overlay-rgb` alpha 26 wash; dot in `text-secondary`; hint elided right, dropped if room < 40 px.

### ui_qt/snap/icons.py
`paint_icon(painter, name, rect, color)`; glyph geometry on a 20 grid (:38-142), line width 1.7 default. "region" reuses the snap glyph.

### ui_qt/snap/live.py (LiveAnnotations, _Canvas, _RegionClose)
- `LiveAnnotations(QObject)` signals: `arming`, `drawn(QScreen, tuple)`, `enabled_changed(bool)`, `capture_changed`, `capture_delay_changed(int)`, `capture_cancelled`, `interaction_changed(bool)`. Public: `armed`, `busy`, `tool`, `color`, `box_infill`, `snap_enabled`, `selecting_region`, `capture_region`, `windows()`, `interacting`, `set_capture_countdown(remaining,total)`, `set_capture_pending(pending, captured, screen)`, `cancel_capture`, `set_enabled`, `toggle_tool`, `select_region`, `arm`, `disarm`, `set_color`, `toggle_snap`, `toggle_box_infill`, `toggle_keep_marks`, `handle_mark_shortcut("keep_marks"|"clear")`, `set_busy`, `set_region`, `clear_region`, `check_focus`, `complete`, `pinned_at`, `pinned_on`, `clear`, `close`. `MARK_LIFETIME_S=1.5`.
- Window flags: toolbar `Tool|Frameless|StaysOnTop|WindowDoesNotAcceptFocus`, title "OpenWhisper Walkthrough Drawing", `WA_ShowWithoutActivating`, `WA_AlwaysShowToolTips`; canvas title "OpenWhisper Walkthrough Canvas", unarmed adds `WindowTransparentForInput`, translucent, property `openwhisperHotkeyWindow`; region close 24x24, title "Close automatic capture region", accessible name "Close region and capture the whole screen", tooltip "Close region · capture the whole screen", pointing-hand cursor, panel radius 6 + "drop" icon 20 px.
- Arming remembers the foreground window (restored on disarm), parks the side panel level on macOS, creates canvases for new screens and reads window targets before canvases take focus. `check_focus` disarms when focus leaves the canvases (deferred 0 ms from `changeEvent`). `set_busy(false)` re-arms when a capture hid the focused canvas. Screen removal disarms, clears region on that screen, deletes canvas, re-shows toolbar.
- Canvas keys (no modifiers): Esc cancel capture, C clear, K toggle keep marks (non-repeat), S snap, tool letters (if no draft), 1-4 colours. Ctrl at press/commit pins a mark. Label Esc = cancel capture.

### ui_qt/snap/pointed.py
`supported()` Windows/cocoa; `window_under_pointer(screen, ours, front)`; `window_targets(screen, ours)` (EnumWindows visible, not iconic, not cloaked (DWMWA 14), frame via DWMWA_EXTENDED_FRAME_BOUNDS (9) else GetWindowRect, physical->logical by DPR; macOS CGWindowList layer 0, not own pid, alpha > 0); frames < 24 px skipped; only frames with an uncovered visible part kept (front-to-back occlusion).

### ui_qt/snap/affinity.py, macos.py, macos_capture.py, grim.py
See Platform-specific code.

## Data and formats

### Settings keys (read here)
| Key | Type | Default | Read in |
|---|---|---|---|
| `walkthrough_title` | str (trimmed) | "Walkthrough" | `finish.document_settings` |
| `walkthrough_show_details` | bool | true | same |
| `walkthrough_show_about` | bool | true | same |
| `walkthrough_show_captions` | bool | true | same |
| `walkthrough_page_size` | choice auto/letter/a4 | auto | same |
| `walkthrough_copy_format` | choice pdf/agent_task/both/agent/word | pdf | `WalkthroughResult.clipboard` caller (runtime) |
| `walkthrough_write_up_agent` | choice claude_code/codex/opencode | claude code | `library.retry_write_up`, runtime |
| `walkthrough_write_up_instructions` | str (trimmed) | "" (= default) | same |
| `walkthrough_write_up_notes` | bool | false | same |
| `walkthrough_write_up_styles` | optional list of {id,name,instructions} | absent (= starters) | `styles.py` |
| `walkthrough_keep_marks` | bool | false | `live.py` (read and saved) |
| `snap_to_edges` | bool | true | `snap/session.py`, `live.py` (read and saved) |
| `snap_box_infill` | bool | false | `overlay.py`, `live.py` (read and saved) |
Also defined near these but read by other areas: `walkthrough_write_up` (bool false), `walkthrough_snap_mode`, `walkthrough_auto_mode`, `walkthrough_capture_delay`, `walkthrough_closed`, intro/minimize flags. Exact key names are an existing-install contract.

### Files on disk (contract with existing installs)
- Root: `<data_root>/captures/` (`OPENWHISPER_DATA_DIR` override, else per-user dir). FioriVoce must decide whether to read OpenWhisper's folder (see open questions).
- Per walkthrough `captures/YYYY-MM-DD_HH-MM-SS[-N]/`: `shot-N.png`, `walkthrough.md`, `walkthrough.pdf`, `walkthrough.docx`, `walkthrough.json`, `walkthrough.original.json` (first Review), `write-up.md/.pdf/.docx`, `write-up-N.md/.pdf/.docx`, `draft.json` (while recording / unrecovered), `audio.wav`. Temp names: `.checkpoint-*`, `.audio.tmp`, `.review-*`, `.write-up-*`, `.previous/`.
- `captures/retention.json` `{"days": N}`; `captures/screenshots/<stamp>[-N].png`.
- Recovered recordings searched as `<recordings>/recording_*-recovered-<journal32hex>.wav`.

`walkthrough.json` v1:
```json
{"version":1,"started_at":"2026-10-20T14:32:05-07:00","duration_s":136.4,
 "sections":[{"text":"...","shot":"shot-1.png","kept_at_s":3.0,
              "omitted_shot":"shot-2.png","notes":["..."]}]}
```
`omitted_shot` and `notes` only when present; `shot` and `kept_at_s` are both written as `null` for trailing speech (the example omits them); `text` is whitespace-tidied; `duration_s` and `kept_at_s` are Python floats; a section with no shot, no omitted shot, no words and no notes is dropped; a shot outside the folder is stored as an absolute path (only loadable if it matches `shot-N.png` locally, so effectively rejected).

`draft.json` v1: `{"version":1,"started_at":"<naive iso>","journal_id":"<32 hex or ''>","shots":[{"shot":"shot-1.png","kept_at_s":2.0}],"notes_s":[[2.5,3.5],[7.0,null]]}`.

### Wire protocols
- Write-up agent prompts (system = charter; user = instructions + paths/Markdown + notes). Reply contract: Markdown only, images as `![alt](shot-N.png)`, optional look front matter.
- grim: `grim [-o OUTPUT] -` PNG on stdout. hyprctl monitors JSON (`name`, `x`, `y`, `focused`).
- Clipboard: file list (PDF/DOCX) + optional text; custom marker MIME format (`AUTO_PASTE_MARKER_FORMAT`).

## External dependencies

| Python | Used for | C++ / Qt 6 |
|---|---|---|
| PyQt6 QtGui text (QTextDocument, QTextCursor, QTextLayout, formats, Markdown reader) | PDF/DOCX layout | Same classes in Qt6::Gui; `QTextDocument::setMarkdown` with `MarkdownDialectGitHub | MarkdownNoHTML` |
| QPdfWriter, QPainter, QPageSize/Layout | PDF | Same |
| python-docx (+ lxml) | .docx | No Qt equivalent. Hand-write OOXML parts (document.xml, styles.xml, core.xml, rels, media) with `QXmlStreamWriter` or plain strings and zip with miniz/libzip (vcpkg) or `QZipWriter` (private Qt API, avoid). Must reproduce python-docx's default template styles (Normal etc.) closely enough that Word opens it; capture a python-docx output as reference. |
| numpy | energy search, int16 frames | `std::vector<int16_t>`, prefix sums in `double` |
| wave | read/write 16-bit PCM WAV | `fv::read_wav` exists but converts to mono float; add an int16 frame reader (keep channels) and writer, or skip writing clips (see Engine) |
| json, tempfile, os.replace, fsync | atomic metadata | QSaveFile (does temp+rename, fsync on commit) or std::filesystem + platform fsync; nlohmann/json or QJsonDocument (beware QJson double formatting vs Python repr) |
| shutil.copyfile/copy2 | copies keeping mtime | `std::filesystem::copy_file` + `last_write_time` |
| uuid | style ids, paste token | `QUuid::createUuid()` |
| concurrent.futures ThreadPoolExecutor | PNG writer | one `QThread`/`std::jthread` with a FIFO task queue and futures |
| ctypes user32/kernel32/dwmapi, pywin32 (win32gui, win32api) | affinity, foreground, enum windows, monitors | Direct Win32 calls |
| pyobjc AppKit/Quartz/ScreenCaptureKit | mac plumbing | Objective-C++ (.mm) |
| subprocess (grim) | Hyprland capture | `QProcess` with 2 s timeout |
| services.hyprland | hyprctl JSON | port of that module (other area) |

## Platform-specific code

- **Windows**: capture exclusion `SetWindowDisplayAffinity(hwnd, 0x11)` with stored-HWND staleness check (`affinity.py:66-100`); `bring_to_front` via AttachThreadInput/SetForegroundWindow (:103-155); foreground restore after live drawing; monitor lookup in native coords (`capture.py:45-60`); window-under-pointer and window edges via WindowFromPoint/GetAncestor(GA_ROOT)/GetForegroundWindow, DwmGetWindowAttribute 9/14 (`pointed.py`); overlay is a `Tool` stay-on-top window (no taskbar entry); live canvases need 1-alpha fill to receive mouse on layered windows (`live.py:619-621`); `click_at` native coords for automatic SCREEN captures.
- **macOS**: Screen Recording grant (Quartz preflight/request, settings URL), overlay shown at screen geometry and lifted to level 26 (not fullscreen to avoid a new Space), parked level 27, CanJoinAllSpaces + FullScreenAuxiliary (never with MoveToActiveSpace), nonactivating panels, disable window animation, focus owner remembered and returned (`yieldActivationToApplication` on 14+), ScreenCaptureKit filtered capture for automatic shots, settle 300 ms, "Cmd" in pin tooltip, quartz window list for targets.
- **Linux X11**: supported via `QScreen::grabWindow(0)`; overlay is a normal `Window|Frameless|StaysOnTop` (WMs only fullscreen normal windows); no window targets (snap to screen edges only), no window mode.
- **Wayland (non-Hyprland)**: unsupported with WAYLAND_REASON; detection by Qt platform or session env (XWayland counts as Wayland).
- **Hyprland**: grim + hyprctl; single focused screen only; settle 200 ms; overlay plain `Window|Frameless` and fullscreen.

## Engine-specific code

- Nothing in this area exists only for a non-Parakeet engine. The decode seam is `finish.transcribe_sections(..., transcribe: Callable[[str], str])`, called from the runtime with `recognition_context.transcribe(incremental, backend, path, recognition)` (`services/runtime/transcription.py:1721-1725`); the worker-process / backend plumbing on that side is dropped with the other engines.
- Adapt for in-process Parakeet: there is no need to write `section-N.wav` / `note-N.wav` temp files. Compute frame spans with the same algorithm, convert each span (minus note gaps) to mono float and call `Recognizer::transcribe(span, rate)` on `EngineWorker`'s thread, one call per clip, in order. Keep the 0.25 s minimum (skip decode, empty text) and the "decode whole file into section 0" fallback when the audio cannot be read/cut. Cancellation via the `before_each` equivalent between clips.
- The cut search must run on the int16 samples as recorded (energy summed across channels); computing it on `fv::read_wav`'s averaged float would change stereo results. The recorder writes 16-bit PCM, so feed the int16 buffer.
- If FioriVoce keeps cloud/remote engines (undecided), the same path-based seam is needed for them; design the C++ seam as "decode these sample spans" with an adapter that writes a WAV only when a backend needs a file.

## Dependencies on other areas

Calls out:
- `config.data_root()` (app paths) -> captures root.
- `services.settings` (SettingsKey, resolve_* functions, `settings_manager.load_all_settings/save_setting/mutate_settings`, `WalkthroughCopyFormat`, `WalkthroughPageSize`, `MeetingAgentCore`) -> settings area.
- `meeting.agent.installed.core.run_reading_task / run_viewing_task / MEETING_AGENT_SETTING`, `drivers.AgentUnavailable`, `services.installed_agents.AGENT_SPECS[agent].name` -> installed-agent (meeting intelligence) area. Contract: `run_*_task(agent_id, model="", system_prompt, user_prompt, folder|images, timeout_s, stall_s, cancel_event) -> PassOutcome{ok, canceled, error, text}`.
- `ui_qt.utils.palette.current_palette()` roles `overlay-bg`, `overlay-border`, `overlay-text`, `overlay-rgb` (with alpha), `accent`, `accent-hover`, `text-secondary`; `ui_qt.utils.font_scale.current_ui_font_scale()`; `ui_qt.utils.desktop.is_wayland()`; `services.desktop_session.is_wayland_session()`; `services.hyprland.available()/query()`.
- `ui_qt.widgets.capture_delay.CaptureDelay` (embedded in live toolbar: signals `changed(int)`, `cancelled`, methods `set_countdown`, `popup_windows`).
- `walkthrough_paste`: `services.focus_context.same_target`, `services.hotkey_manager.is_accessibility_trusted`, `services.synthetic_keys.modifiers_held`, `ui_qt.clipboard.AUTO_PASTE_MARKER_FORMAT` and a clipboard writer with `write_files(paths, marker)` / `write_text`.

Called in by:
- `services/runtime/walkthrough.py` (session lifecycle, snaps, `export.render` for pinned marks, `new_screenshot_path`), `services/runtime/transcription.py` (startup `attach_recovered_audio` + `prune`; recovery `recovery_session`; `finish.transcribe_sections/build_documents`; `write_up.write_up`; `kept_note`).
- `ui_qt/ui_controller.py` (snap support/request access, LiveAnnotations, start_snap, keep_out_of_screenshots, WalkthroughPaste, load_result).
- Widgets/dialogs: `walkthrough_tab.py`, `walkthrough_review.py`, `walkthrough_ready.py`, `walkthrough_panel.py`, `past_walkthroughs.py`, `walkthrough_library.py`, `walkthrough_snap_menu.py`, `screenshot_flight.py`, `capture_delay.py`, `write_up_prompt_dialog.py`, `new_style_dialog.py`, `settings_dialog.py` (agents list, default instructions, agent_name), `settings_backup.py` (captures_root), `overlays/waveform_overlay.py` (macos levels).
- Scripts `scripts/check_walkthrough_desktop.py`, `scripts/check_walkthrough_paste.py` (manual desktop checks; worth porting as dev tools).

## Tests

Pure logic, good golden-vector sources:
- `tests/test_walkthrough_sections.py` (15): cut placement in pauses, no move without a pause or with only a softer stretch (0.8 amplitude), reach limit, monotone cuts not passing next mark, marks past end, clip count/naming, short sections not written, stereo whole frames, 16-bit only, held stretches removed/cut, held across a screenshot, held to end, too-short note.
- `tests/test_walkthrough_session.py` (22): background numbered writes, lazy folder, closed session, failed writes left out and reported with no partial file, discard/end semantics, and exact storyboard vectors (e.g. moving shot 3 up twice -> shots [3,1,2], sections ((2,),(0,),(1,),(3,)); remove 2 -> cuts (2,6), sections ((0,),(1,),(2,)); move then remove -> ((1,0),(2,),(3,)); undo after move -> ((0,),(2,1,3)); add after undo -> cuts (2,4,8)); `section_at`; notes_s ((2.5,3.5),(7,inf)).
- `tests/test_walkthrough_document.py` (29): markdown text exactly (meta line, clock table 0->0:00, 9.99->0:09, 136.4->2:16, 3599.9->59:59, 3600->1:00:00, 3725->1:02:05, -1->0:00), escaping table, link bracketing, LF/UTF-8, manifest content, PDF text/metadata, pagination behaviours (section moves to next page, lone screenshot never shrunk, first section stays with title, long section flows), worker-thread safety, unreadable screenshot, unwritable PDF raises, locale paper, header/caption toggles, settings -> format.
- `tests/test_walkthrough_write_up.py` (23): prompts per agent (golden strings), instructions override, write-up becomes document, failure messages, missing agent setting path rewrite, unfencing, image filtering, notes rule, look block parsing, leading rule kept, look-only reply = no document, colour on words not links, invalid colour ignored, purple in PDF, lists/bullets in PDF text, image column in lists, table placement, missing image + HTML as text, keep heading with screenshot, held notes listing.
- `tests/test_walkthrough_library.py` (32): round-trip paths, Review edits/omit/restore/reorder, rollback on failure, write-up preservation and versioning, validation (other files, invalid times, symlinks, object metadata), held notes in manifest and through Review, retry, checkpoint/recovery (hard exit, journal join idempotent, truncated PNG, interrupted write), retention defaults/refusals, startup recovery does not import the Qt renderer.
- `tests/test_walkthrough_styles.py` (26; first ~9 pure: defaults, delete-all, add/replace, starter rewrite, error messages, limit, broken entries, default matching), rest UI (dialogs, menus).
- `tests/test_walkthrough_storage.py` (7): naming, collisions, file-with-name never written into.
- `tests/test_walkthrough_word.py` (6): docx parsed back for text, keep-with-next per section, format settings, write-up structure/links/look, sizes in points, agent copy includes PDF + instructions.
- `tests/test_walkthrough_document_threads.py` (4): PyQt-specific deletion on main thread (drop as such; keep "renders on a worker" test).
- `tests/test_walkthrough_paste.py` (12): two-step paste sequencing, Accessibility gate, interrupted paste never sends instructions, single-flight, preflight and second-write failures, shortcut dispatch leaves Ctrl+V alone, latest result skips drafts (some tests touch the hotkey area).
- `tests/test_snap_geometry.py` (21): device_rect rounding, span, toolbar placement, badge, grab_at, resized, render crop and device-scale painting, clipping, redaction covering all pixels.
- `tests/test_snap_annotations.py` (24): stack, step numbering, infill, redaction, drafts, meaningful thresholds, highlighter width, jitter, Shift constraints and bounds, arrow head/bow/zero length, ink colours, paint inside bounds.
UI/integration (replay as Qt tests in `tests/app`, offscreen):
- `tests/test_snap_overlay.py` (46), `tests/test_snap_session.py` (67, includes support reasons, grim command/timeout/output resolution, settle times, affinity), `tests/test_snap_live.py` (18), `tests/test_snap_macos_focus.py` (8), `tests/test_snap_macos_capture.py` (2), `tests/test_snap_click_screens.py` (2), `tests/test_capture_delay.py` (4, toolbar wrap), `tests/test_walkthrough_storyboard.py` (40, mostly the Review tab; first 5 are library logic), `tests/test_walkthrough_runtime.py`, `test_walkthrough_shortcuts.py`, `test_walkthrough_buttons.py`, `test_walkthrough_panel.py`, `test_past_walkthroughs.py`, `test_walkthrough_library_ui.py`, `test_walkthrough_write_up_notice.py` (other areas' UI driving this one).

Golden vector plan: generate from Python with a script under FioriVoce `tests/golden/` (never writing in the OpenWhisper checkout): (1) `cut_frames` on synthetic int16 signals; (2) `_storyboard` from scripted board edits; (3) `markdown()` + `write_manifest` for fixed `Walkthrough` inputs; (4) `clock`, `_when`, `_escape_markers`, `_image_target`; (5) `document_text`/`_split_look`/`_notes`/prompts; (6) `write_up_parts`/`join_write_up`; (7) geometry functions (`snap_point`, `grab_at`, `resized`, `device_rect`, `place_toolbar`, `badge_rect`, `_wrap` layouts); (8) annotation `bounds()`, `meaningful()`, `extend_draft`, `arrow_shapes` points, `ink_for`. PDF output compared structurally (page count, text per page via a PDF text extractor, image frame positions) rather than byte-equal.

## Porting notes

Decomposition:
- `src/engine` (Qt-free) is the wrong home for most of this because the document renderer needs QtGui. Suggested libraries:
  - `fv_walkthrough_core` (no Qt): storage naming, session/board/storyboard model, section cutting (int16), notes, manifest/draft JSON, library validation/retention/delete, write-up text processing (regexes, prompts, look parsing, notes), styles, markdown generator, clock/when. Pure and unit-testable with golden vectors.
  - `fv_documents` (Qt6::Gui only, no Widgets): PDF composer + pagination + painter, Markdown composer, DOCX writer (OOXML + zip). Must run on a worker thread with a `QGuiApplication` present.
  - `fv_snap` (Qt Widgets, in `fv_app`): annotations (QPainter), geometry (could be Qt-free with own rect types, but Qt's QRect off-by-one semantics (`right() = x+w-1`) are baked into the math; keep QRect), overlay, toolbar, icons, chrome, session, live, capture, platform files (`affinity_win.cpp`, `pointed_win.cpp`, `macos.mm`, `macos_capture.mm`, `grim.cpp`).
  - `WalkthroughPaste` in `fv_app`.
- Threading: PNG writer = single worker with FIFO queue + per-task future; session mutex. Document build and write-up on a worker; Recognizer calls only through `EngineWorker` queued signals (CLAUDE.md rule), so section decoding is a sequence of queued requests or one "transcribe spans" job executed on the engine thread.

Order of work: (1) storage + session/storyboard + sections (pure, golden-tested); (2) markdown + manifest + library load/validate; (3) PDF composer/pagination (largest risk); (4) DOCX writer; (5) finish pipeline wired to Parakeet; (6) geometry + annotations + export (pure-ish, golden); (7) toolbar/icons/chrome; (8) overlay + session on Windows; (9) live annotations; (10) review save, write-up, styles, recovery, retention, paste; (11) macOS and Hyprland backends.

Risks:
- PDF pagination depends on QTextDocument layout internals (lazy layout, block top-margin across page breaks, line-by-line painting, `_SLACK` rounding); C++ uses the same Qt so behaviour should carry, but the workarounds (full layout first, `_extent` not `blockBoundingRect`, empty-layout crash guard) must be ported exactly.
- DOCX without python-docx: must emit a valid package Word accepts (pPr element order, relationships, inline picture XML with `a:ln`). Budget time for validation in Word and LibreOffice.
- Markdown reading of agent replies relies on Qt's md4c-based reader; same in C++ Qt 6, but Qt version differences (e.g. blank first cell line bug) need the same fixes.
- Platform window plumbing (Win32 affinity/foreground, macOS levels/Spaces/ScreenCaptureKit) is fiddly and only testable on real desktops.
- JSON compatibility with existing installs (float formatting of `kept_at_s`, `null` ends, ISO times with offset) if FioriVoce reads OpenWhisper captures.
- Write-up depends on the installed-agent runner area being ported first.

Rough C++ size: core ~1.6k, documents ~2.2k (PDF 1.1k, DOCX 0.9k incl. OOXML templates), snap UI ~3.0k (overlay 0.6k, live 0.6k, toolbar 0.4k, annotations 0.5k, chrome/icons/geometry/export 0.6k, session/capture 0.4k), platform ~0.7k, paste 0.15k; total about 7.6k lines plus tests.

Native gains: PNG encode on the writer thread without the GIL (could also use a faster zlib level); section decoding straight from memory with no temp WAVs; energy search in a single pass; the overlay can keep the dimmed image as a QPixmap on the GUI thread for faster blits; window enumeration without ctypes overhead makes arming faster; no PyQt main-thread deletion dance for worker-built documents.
