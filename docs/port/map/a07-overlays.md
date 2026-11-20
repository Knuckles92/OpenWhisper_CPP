# Recording overlays and waveform looks

Area a07-overlays. Source root `D:\coding\whisper_local`, all paths below relative to it. About 4370 lines in 11 files, all read in full. Everything is custom `QPainter` drawing: there are no child widgets, no stylesheet, no objectNames inside the overlay itself. Looks are matched by reproducing the draw code, not by QSS.

## What the user gets

A small floating pill ("the overlay", 300x80 px, rounded 12 px) that opens next to the mouse pointer when dictation starts and shows what the app is doing:

- **Recording / Streaming / Command Mode**: a live "look" that moves with the voice (11 choices, default "Bars"), a label `● Recording 0:04` (dot, words and clock each switchable), and optional live-preview words above or beside it.
- **Finish**: when recording ends the look morphs into its own Processing then Transcribing animation (`Processing...`, `Transcribing...`, a speed-up between them), then `Cleaning up...` or `Rewriting...` (scribbled lines combed straight by a purple caret), then a fade out when the text lands.
- **One-shot moments**: `Enabled`, `Disabled`, `Copied!`, `Canceled`, a finished AI command ("done" check with sparkles), a problem notice (amber "!" with head-shake), a language-switch notice (accent chip pops in), and a large-file `Splitting (48.2 MB)...` blade animation.
- **Badges and chips**: `Hands-free` (lock icon), `Command Mode`, a destination badge such as `Walkthrough`, a language chip (`EN`, `AUTO`) that can be clicked to cycle, and `Snap` / `Done` buttons while a walkthrough records.
- **Caption**: a 2.5 s warning-coloured line (for example `Switched to Laptop mic`) that replaces the label.
- The pill can be dragged aside, is kept on screen, never takes focus, and on Wayland draws embedded inside the main window instead of as its own window.
- Settings gets a live preview of any look (sample voice loop, optional "Try with my microphone"), and the tray / shortcut / chip share a dictation-language menu.

## Feature inventory

Disposition column: port / drop (engine-specific) / undecided backend / other-platform only.

| ID | Feature | Exact behavior | Source | Disposition |
|---|---|---|---|---|
| A07-01 | Overlay window shell | Frameless, always-on-top, `Tool`, `WindowDoesNotAcceptFocus`, `WA_TranslucentBackground`, `WA_ShowWithoutActivating`, NoFocus, mouse tracking. Fixed size 300x80 (`config.WAVEFORM_OVERLAY_WIDTH/HEIGHT`). Grows to `WAVEFORM_STREAMING_MAX_HEIGHT` 400. | waveform_overlay.py:127-183; config.py:334-337 | port |
| A07-02 | Three overlay modes | Standalone floating window (default), `embedded` (parent given, Wayland: draws as an inert child of main window at bottom-right, 16 px right / 64 px bottom margin, square 2 px badge corners, no flashes), `preview` (child widget for Settings, never moved or hidden by parent). | waveform_overlay.py:127-178, 665-674 | port (embedded = other-platform only: Wayland) |
| A07-03 | State machine, 15 states | idle, recording, streaming, command_listening, processing, transcribing, cleaning, rewriting, canceling, stt_enable, stt_disable, copied, language, notice, large_file_splitting. Emits `state_changed(str)`. | waveform_overlay.py:93-125, 1357-1396 | port |
| A07-04 | Frame timer | `QTimer` at `1000 // WAVEFORM_FRAME_RATE` = 33 ms (30 fps) while not idle; dt clamped to 0.1 s; stops on idle. Particle physics substeps at fixed 1/120 s inside dt. | waveform_overlay.py:1316-1355, 1409-1427; particle_style.py:109-129 | port |
| A07-05 | Placement near pointer | `preferred_overlay_position`: below-right by 10 px, flip left/up if it overflows, clamp to screen availableGeometry with 8 px margin. Re-anchored on every `show_at_cursor`. | waveform_overlay.py:665-701; utils/overlay_position.py | port |
| A07-06 | Drag to move | Left press on a non-control area drags (OpenHand / ClosedHand cursor), clamped to the pointer's screen with 8 px margin. Once dragged ("placed") it stays through growth (only nudged on-screen), until hidden; next show opens at the pointer again. | waveform_overlay.py:1187-1218; utils/drag_move.py | port |
| A07-07 | Height growth for preview text | With live words the height is `clamp(above + textHeight + below, base, effective_max)`; effective max = config 400, limited by room below/above the anchor (or below a placed overlay minus 8 px; embedded: max(base, parentHeight/3)). | waveform_overlay.py:703-744 | port |
| A07-08 | Live preview text | `update_streaming_text(text)` strips, grows overlay, draws bottom-aligned word-wrapped text (Segoe UI, size from setting 10-48, default 16) so newest words stay visible; embedded path lays out with `QTextLayout` and caches by (text, width, size). | waveform_overlay.py:598-654 | port |
| A07-09 | Preview layout: header or footer | Setting `streaming_overlay_layout` (`header` default, `footer`). Header: slim 36 px row (badge row) with `● 0:04` + small copy of the look, divider, words below. Footer: words on top, divider, full 80 px look band at the bottom. | waveform_overlay.py:323-350, 405-516 | port |
| A07-10 | Header line-of-its-own fallback | If badge, chip, or Snap/Done leave less than label width + 40 px in the top row, `● 0:04` and small look move to their own 28 px row (header height 36+28). If the clock does not fit, only the dot shows. | waveform_overlay.py:405-434, 498-509 | port |
| A07-11 | Small look in header | Each Visual declares `small` = SQUASH (scale y only), SHRINK (uniform scale), FIT (whole overlay drawing scaled to fit). Header look area 24 px tall, min 40 px wide; particles left out for live looks; Classic shrinks the whole band. | waveform_overlay.py:436-467; recording_looks.py:61-67 | port |
| A07-12 | Look selection, 11 looks | ribbon, bars (default), wave ("Mirror wave"), aurora, pulse, dots, baitball ("Bait ball"), fish ("Real fish"), glow ("Deep-sea glow"), fizz, legacy ("Classic"). Aliases `live`->ribbon, `orb`->pulse, `bubbles`->baitball; unknown -> bars. Labels and one-line details live in settings.py:RecordingOverlayLook (see Data). | recording_looks.py:39-73; services/settings.py:205-248 | port |
| A07-13 | VoiceRibbon loudness meter | RMS -> dB scale (floor -60 dB, ceiling -12 dB) -> 0-1; instant attack, release time constant 0.06 s, peak hold between frames; history of 100 samples over 2.2 s (newest right); 14 px edge. | live_recording.py:53-104; utils/loudness.py | port (golden vectors) |
| A07-14 | Look: Ribbon | Glowing band tracing last 2.2 s; particles leave its crest; finish rolls the ribbon into a spinning loop with a head dot. | recording_looks.py:166-206 | port |
| A07-15 | Look: Bars | Scrolling mirrored rounded bars (pitch 5, width 3, half height 18), fade in over left 25 %; finish: bars fly into a ring of 28 spokes with a travelling highlight. Default look. | recording_looks.py:209-261 | port |
| A07-16 | Look: Mirror wave | Mirrored outline shape (amplitude 18) with gradient fill+edge, particles rise off its top; finish: shrinks to a pulsing capsule. | recording_looks.py:264-331 | port |
| A07-17 | Look: Aurora | Three sine waves (freq/phase/pen table) driven by smoothed level (tau 0.05 s), pace 2+7*level; finish: braid that spins with hue drift. | recording_looks.py:334-392 | port |
| A07-18 | Look: Pulse | Orb: core + ring that follow level; ripple spawned when level>0.5, rises >0.12 and 0.16 s since last (0.8 s life); finish: ring narrows to a spinning comet arc (QConicalGradient). | recording_looks.py:395-473 | port |
| A07-19 | Look: Dots | Five dots (spacing 20, lag 5 samples), pop in staggered; finish: row bouncing like a typing indicator. | recording_looks.py:476-519 | port |
| A07-20 | Looks: Bait ball / Real fish / Deep-sea glow | Flocking school (56 / 44 / 56 fish) under 5 spring-simulated swell layers (48 points); voice sets lift and speed; each has its own finish (ball spins faster under a dome, wheel + caustic shimmer, whirl + sonar pings). Fixed RNG seed 5. | ocean_looks.py:33-522 | port (largest sim) |
| A07-21 | Look: Fizz | 9 glasses; bubbles spawn at 10+230*level^1.5 per s; surface hits kick level; leak drain; finish: levels roll as a wave. Seed 11. | ocean_looks.py:525-657 | port |
| A07-22 | Classic look (legacy) | The original `ParticleStyle` particles fed raw RMS (audio_response 1.5 from config), status text drawn by the style; its own vortex (Processing) and converge (Transcribing). | particle_style.py; waveform_overlay.py:362-375, 518-532 | port |
| A07-23 | Particle system | Config: max 150, emission 30/s, gravity 20, damping 0.98, wind 5, turbulence 10, color shift 50, glow on. Live looks use `audio_response` 2.6 and emit from the look's `emitter(x)` curve. | particle_style.py; config.py:828-841 | port |
| A07-24 | Live label | `● Recording 0:04`: dot pops with ring then pulses every 1.4 s; words and clock optional (settings `recording_overlay_dot/text/clock`, default all on); text elided; clock `m:ss`; rises in under 0.06 s delay. Dot is red ("danger") or accent for a command/destination. | live_recording.py:143-223; waveform_overlay.py:377-403 | port |
| A07-25 | Entry flash | Edge flash on pill when recording starts (not embedded, not command mode); look grows in over 0.4 s after 0.05 s. | waveform_overlay.py:382-395 | port |
| A07-26 | Processing / Transcribing | `_work_rate` eases 1.0 -> 1.7 (tau 0.3 s) from processing to transcribing; `_work_clock` accumulates dt*rate; morph from recording snapshot over `MORPH_S` 0.5 s; label crossfades 0.28 s (old lifts 6 px as new rises). Custom label via `set_processing_label` (e.g. `Retrying connection (2/3)...`). | waveform_overlay.py:518-555, 1316-1335, 1398-1407 | port |
| A07-27 | Cleaning up / Rewriting | `tidy_lines`: 3 lines of scribbled word strokes, caret combs them straight (period 2.1 s) in a loop; previous look's finish fades out over 0.45 s underneath; purple label rises under a glint. Rewriting (command mode) has nothing to hand off. | tidy_lines.py; waveform_overlay.py:768-792 | port |
| A07-28 | Fade-out on hide | If hidden while in processing/transcribing/cleaning/rewriting, opacity fades over 0.25 s then hides; any new state or notice cancels the fade. | waveform_overlay.py:1642-1655, 266-272, 1333-1335 | port |
| A07-29 | Moments choreography | `moments.py`: disc pop (0.24 s, ease_out_back), two rings (0.6 s), edge flash, glyph that draws itself (check, cross, bang, clip), confetti, label rise + glint, afterglow; per-moment `Moment` records (done, enabled, disabled, copied, CANCELED). | moments.py | port |
| A07-30 | Canceled | 1.2 s (`CANCELLATION_ANIMATION_DURATION_MS`): snapshot of live particles implodes into core, disc slams in from oversize with shake, whole pill flashes red, 72 glowing bubbles burst, label `Canceled`. Returns to idle itself, also from hidden. | waveform_overlay.py:812-833, 1337-1346, 1414-1419 | port |
| A07-31 | Enabled / Disabled / Copied! | Visible `OVERLAY_HIDE_DELAY_MS` 1500 ms, fades out in the last 0.3 s; "Disabled" lets confetti fall (gravity 260). | waveform_overlay.py:835-861; moments.py:97-106 | port |
| A07-32 | Notices | `show_notice(text, done=False)`: if something is on screen (not transient, not idle, not fading) it becomes a caption instead; otherwise a notice shows at the pointer for `2500 + 25 ms * len(text)`. `done=True` plays the "done" moment; problem = amber disc + "!" + head shake, text word-wrapped, shortened with ellipsis if too tall. | waveform_overlay.py:1542-1614 | port |
| A07-33 | Caption | `show_caption(text)` only while visible; warning-text colour, fades in 0.15 s, out in last 0.35 s, auto-clears after 2500 ms and when listening ends; takes the label's place (or the header's look room). | waveform_overlay.py:1616-1640, 972-976 | port |
| A07-34 | Mode badge | Pill at (9, 9), 18 px tall, Segoe UI 8 DemiBold. Text `Command Mode`, else destination text, else `Hands-free`; hands-free adds a drawn padlock. accent tint on opaque overlay-bg. Pill ring accent when command mode or destination set. | waveform_overlay.py:866-927, 747-766 | port |
| A07-35 | Hands-free latch | `set_hands_free(bool)`: kept even while hidden (Omarchy bar reads `overlay.hands_free`); cleared on hide, end of listening, next recording start. | waveform_overlay.py:1489-1498 | port |
| A07-36 | Destination badge | `set_recording_destination(text)` persists through transcription/cleanup states; cleared by non-listening/non-fading states and on hide. | waveform_overlay.py:1504-1508, 1376-1379 | port |
| A07-37 | Language chip | Top-right chip with short label (`EN`, `AUTO`) when `language` is set and >=2 choices; flashes accent for 0.6 s on change while visible; hover and click (emits `language_cycle_requested`) only on standalone overlays where `_platform_takes_overlay_clicks()` (windows, xcb, cocoa). 4 px hit slop. | waveform_overlay.py:929-970, 1023-1040, 1510-1540 | port |
| A07-38 | Language notice | `show_language_notice()`: new language chip (13 pt bold) pops at top, ring following chip shape, label with the full language name; repeat press replays. | waveform_overlay.py:978-1021, 1520-1527 | port |
| A07-39 | Walkthrough Snap / Done controls | `set_walkthrough_controls(on)`: two pills left of the chip: `Snap` (accent fill, camera icon), `Done` (check icon, ghost). Drop words for icons (26 px wide) if the badge needs room. Hover shows hotkey pill via `show_hotkey_hint`. Emit `walkthrough_snap_requested` / `walkthrough_finish_requested`. | waveform_overlay.py:1042-1185 | port |
| A07-40 | Corner parking | Walkthrough sends overlay to top-left of the screen's available area + 16 px until dragged or hidden; `park(area)` / `unpark()` for a snap cover (macOS raises window level). | waveform_overlay.py:684-687, 1220-1245 | port (macOS level = other-platform only) |
| A07-41 | Stand-in | `set_stand_in(callable)`: while the walkthrough column shows the recording, overlay shows only notices/moments, hides listening/work states. | waveform_overlay.py:1062-1073, 1357-1362, 1702-1703 | port |
| A07-42 | Large-file splitting | Amber bar of 5 chunks, a blade sweeps 1.4 s per cycle, cuts spring open with sparkles; text `Splitting (X.X MB)...`. | waveform_overlay.py:1273-1314 | port |
| A07-43 | Error fallback frame | Any paint exception logged once, then pill with `Error`. | waveform_overlay.py:280-292 | port |
| A07-44 | Theme awareness | All colours from palette tokens; dark vs light branches inside every hue helper. | utils/palette.py; hue helpers | port |
| A07-45 | Recording look preview (Settings) | `RecordingLookPreview` widget: real overlay (preview mode) looping 9.4 s sample voice: recording 0, processing 4.8, transcribing 6.6, done 8.4 (fade). Caption `Playing a sample voice.`; toggle `Try with my microphone` / `Stop microphone`; `Listening to <mic>. Nothing is recorded.`; failure `Couldn't open the microphone: <reason>.`. Mic opens only while toggled and while shown. | widgets/recording_look_preview.py | port |
| A07-46 | Dictation language menu | Tray "Language" submenu and shortcut share `populate`/`select`/`cycle`; checkable language list; with one language, the current language as a checked disabled row, or the single-language reason as a disabled row, or `No other languages` if neither; separator, `Choose languages…` opens Settings Voice model; status `Dictation language: <name>`; failure `Couldn't switch the dictation language`; `Add another language in Settings → Voice model to switch`. | widgets/language_menu.py | port (single-language reasons are engine-specific text: see Engine-specific) |
| A07-47 | Hotkey hint on control hover | Pill above hovered Snap/Done (or Qt tooltip `<hotkey> · desktop shortcut / while OpenWhisper is focused` on compositor-managed Wayland). | waveform_overlay.py:1255-1271; widgets/buttons.py:158-190 | port (hint widget belongs to another area) |

## Modules

### ui_qt/overlays/__init__.py (7 lines)
Re-exports `WaveformOverlay` only.

### ui_qt/overlays/waveform_overlay.py (1733 lines): `WaveformOverlay(QWidget)`
- **Responsibility**: the window, state machine, layout of badge row / header / band / status, all non-look drawing, timers, placement, click handling, glue between the app and the looks.
- **Signals**: `state_changed(str)`, `language_cycle_requested()`, `walkthrough_snap_requested()`, `walkthrough_finish_requested()`.
- **Public surface** (callers in ui_controller.py, application_controller.py:1268, omarchy_controls.py:209):
  `show_at_cursor(state=None)`, `hide()`, `set_state(state)`, `update_audio_levels(levels)`, `update_streaming_text(text, is_final)`, `clear_streaming_text()`, `set_processing_label(text)`, `show_notice(text, *, done)`, `show_caption(text)`, `show_language_notice()`, `set_language(code, choices)`, `set_hands_free(bool)` / `.hands_free`, `set_recording_destination(str)`, `set_recording_look(look)` / `.recording_look`, `set_label_parts(dot, text, clock)` / `.label_parts`, `refresh_recording_look()`, `refresh_streaming_font_size()`, `refresh_streaming_layout()`, `set_large_file_info(mb)`, `set_walkthrough_controls(bool)`, `set_control_hotkeys(snap, finish)`, `set_stand_in(callable)`, `park(area)`, `unpark()`, `.current_state`, `.timer`, `.isWindow()`, `.close()`.
- **Constants** (px unless stated): `CAPTION_MS` 2500; `_NOTICE_MS_PER_CHAR` 25; `_LANGUAGE_FLASH_S` 0.6; `_BADGE_HEIGHT` 18; `_BADGE_MARGIN` 9; `_CONTROL_GAP` 5; `_CONTROL_ICON` 12; `_PARK_MARGIN` 16; `_WORK_RATES` processing 1.0 / transcribing 1.7; `_WORK_EASE_S` 0.3; `_LABEL_CROSSFADE_S` 0.28; `_FADE_OUT_S` 0.25; `_HANDOFF_S` 0.45; `_LOOK_HEIGHT` 56; `_HEADER_HEIGHT` 36; `_HEADER_LOOK_HEIGHT` 24; `_HEADER_LOOK_MIN_WIDTH` 40; `_TEXT_MARGIN` 12; `_TOP_ROW` = 9+18 = 27; `_LIVE_ROW` 28.
- **Threading**: GUI thread only. `update_audio_levels` is called from the controller (already marshalled by signal). Preview feeds from a recorder thread through a queued signal (see recording_look_preview).
- **State sets**: `LISTENING_STATES` {recording, streaming, command_listening}; `_PREVIEW_STATES` {streaming, command_listening}; `_TRANSIENT_STATES` {stt_enable, stt_disable, copied, language, notice} start `hidden_timer` (single shot) for `OVERLAY_HIDE_DELAY_MS` 1500 (notice: `2500 + 25*len`); `_WORK_STATES` {processing, transcribing}; `_FADING_STATES` {processing, transcribing, cleaning, rewriting}; `_STYLE_STATES` maps command_listening->recording and rewriting->cleaning for the particle physics; `_STOOD_IN_STATES` = listening | fading.
- **State machine rules**:
  - `set_state` (1357): stood-in states hide an active recording and return; any pending fade is cancelled; same-state is a no-op; entering a work state calls `_begin_work_stage` (first work state: `_work_started=now`, rate 1, clock 0, `visual.begin_work()` snapshot; second work state: `_label_from` = old label, `_label_changed=now`); entering a fading state from work captures `_label_from`; `_recording_destination` survives only listening (not command) and fading states; leaving preview states clears preview text; first entry into listening resets hands-free, re-reads language (`_refresh_language`), resets ribbon and visual, restarts the clock (`_listen_started`); leaving listening clears hands-free, language flash, chip/control rects, hover, caption; transient states start `hidden_timer`, any other state stops it (so a recording starting during a notice is not hidden).
  - `_restart_animation`: resets `animation_time`, `cancel_progress`, `stt_particles`, `_burst_fired`; canceling snapshots `(x, y, qcolor, size*life)` of style particles with life>0.05; idle stops the timer else starts it.
  - `_update_animation` (1316): per tick advances `animation_time`, `ParticleStyle.advance`, then for listening + live look `ribbon.advance` and `visual.advance`; for work states eases `_work_rate`, advances `_work_clock`, `visual.advance_work`; finishes a fade; canceling fires the bubble burst once when `animation_time >= pop_at` (0.16) and returns to idle at 1.2 s; moments fire confetti once at `pop_at`.
  - `hide()` (1642): fading states start the fade, everything else `_finish_hide` which resets state to idle, clears destination, notice, language, caption, anchor, parked/cornered/drag flags, hover, and restores base height. `hideEvent`/`leaveEvent` clear hover.
  - `show_at_cursor(state)` (1689): embedded + parent hidden -> return; stood-in -> return; sets anchor to `QCursor.pos()`, positions, shows, raises if embedded, applies state, re-clamps (height may have changed).
- **Layout rules** (the porter must keep these exactly, tests pin them):
  - `_top_row_in_use` = badge shown OR (listening and chip visible) OR controls clickable; `_band_top()` = 27 if so else 0; overlay height with no words = `80 + band_top` (so a walkthrough overlay is 107 tall).
  - Status row: `QRect(0, band_top + base_height - 25, width, 20)` for the compact case; footer: `(0, height-25, width, 20)`; header with words: `header_room` widened 12 px each side.
  - Preview margins: footer `above = 9+18+8 = 35` if the top row is in use else 10, `below = base_height + 4`; header `(header_height + 6, 12)`.
  - Preview text rect `(12, above, width-24, max(20, height-above-below))`.
  - Divider: 1 px `overlay-border` line at `y+0.5` from x 12 to width-12; header at `_header_height()`, footer at `height - base_height`.
  - Particles are clipped to the compact band and translated by `top` so nothing rises into the words.
  - Hit tests: chip hit rect inflated 4 px; controls inflated (-2, -4, +2, +4).
- **Drawing**: `paintEvent` order = background, state painter, listening extras (badge, chip, controls, caption, records `_badge_rect/_chip_rect/_control_rects` for hit testing). Fonts are all `Segoe UI`: 10 bold (status/labels), 8 DemiBold (badge/controls), 8 Bold (chip), 13 bold (language notice), 10 DemiBold (notice text), preview size from settings.
- **Widget/paint geometry detail** (see UI section below).

### ui_qt/overlays/live_recording.py (229): `VoiceRibbon`, live label
- `VoiceRibbon`: `hear(level)` -> `loudness = level_to_height(level)`, `_peak = max`; `advance(dt)`: target = max(peak, loudness); rise instant, fall exponential `1-exp(-dt/0.06)`; samples a slot peak into a 100-slot deque every `2.2/100` s; `history` list oldest first; `height_at(x, width)` linear interpolation between slots over `[14, width-14]`; `crest(x, width, baseline)` = `baseline - 22*height`; `draw(...)` (band + edge, 81 points, gradient stops `(0,0,0) (0.12,40,150) (0.88,60,230) (1,0,0)` alpha fill/edge, hues drift `+60*stop`, pen 1.6 round).
- Constants: `BASELINE_FROM_BOTTOM` 30, `RISE` 22, `HISTORY_S` 2.2, `_SAMPLES` 100, `RELEASE_S` 0.06, `EDGE` 14, `AUDIO_RESPONSE` 2.6, `_PULSE_S` 1.4, `_DOT_D` 8, `_GAP` 7.
- `draw_live_label`: parts dot / text / clock in a centred row separated by 7 px; text elided to room minus dot, minus `"00:00"` clock width, minus gaps; text rises 6 px with `ease_out_cubic(phase(elapsed, 0.06, LABEL_S))`; clock alpha 150 beside text, 230 alone; `_draw_live_dot`: ring (`reach` 10), pop `ease_out_back`, after `POP_S+0.4` a pulse ring 6 px reach every 1.4 s alpha `110*(1-pulse)`.
- `wordless_label_width`, `format_clock` (`m:ss`, floors, clamps negatives), `particle_hue(style)` = `(style.animation_time * color_shift_speed) % 360`, `_ribbon_color` HSV 190/245 dark, 235/205 light.

### ui_qt/overlays/recording_looks.py (532): the Visual base and 6 looks
- Constants: `LOOKS` order (also the Settings segmented bar order), `DEFAULT = config.RECORDING_OVERLAY_LOOK` ("bars"), `_ALIASES`, `CENTER_Y` 29, `MORPH_S` 0.5, `SQUASH/SHRINK/FIT`.
- Helpers: `normalize_look`, `smooth` (smoothstep, clamped), `hue_color(hue, alpha, bright=True)` (dark HSV s190 or 150 / v245, light s235 / v205), `lerp_point`.
- `Visual` contract: class attrs `particles` (draw recording particles too), `emission_rate`, `band_width`, `small`; methods `reset()`, `emitter()`, `advance(dt)`, `draw(painter, grow)`, `begin_work()` (snapshot = ribbon.history), `advance_work(dt, rate)`, `draw_work(painter, morph, clock)`; helpers `ribbon`, `width` (band_width or overlay width), `hue()`, `snap_at(f)`.
- Looks with particles: Ribbon, MirrorWave (emitter from crest). Particle-less: Bars, Aurora, Pulse, Dots, ocean looks, Fizz.
- `make_visual(look, overlay)` returns None for Classic.
- Edge handling: `Pulse.draw` uses `ease_out_back(grow)` only while `grow<1`; `Dots` appear staggered `smooth(grow*1.6 - k*0.15)`; `Bars` index math guards `max(0, index)`; `snap_at` clamps f.

### ui_qt/overlays/ocean_looks.py (657): `_School`, `BaitBall`, `RealFish`, `DeepGlow`, `Fizz`
- `_School(Visual)` constants: FISH 56, BASE 50, TOP 6, LAYERS 5, POINTS 48, BOTTOM 40, GAP 5.5, GAP_MIN 2.6, SWELL 1.8, RANGE 26, COHESION 1.4, ALIGN 1.6, SEPARATE 260, STEER 15, ATTACK_S 0.04, RELEASE_S 0.35, VOICE_FLOOR 0.15, spring accel `-26*u + 700/9*(l+r-2u) - 3.2*v`, damping `0.94**(dt*30)` x / `0.9**(dt*30)` y; two physics sub-steps per tick; fish speed cap `speed*1.6+50` (recording) or `140*rate` (work).
- RNG: `random.Random(5)` for school (Fizz 11). A C++ port using a different PRNG cannot match frame-for-frame; behaviour is statistical (tests only assert thresholds).
- Quirks to keep: fish update in sequence (later fish flock to updated positions); `_school_tops` pushes bottom layer; `_clamp_column` keeps layer order; `u >= -4`; BaitBall `_depth` mean y feeds a depth pull; Fish draw rotates per velocity; DeepGlow trail length 7, ping 0.9 s; Fizz lanes 9, MAX_H 38, LEAK 6, SLOSH_S 0.35.

### ui_qt/overlays/moments.py (451): one-shot choreography
Constants `RADIUS 13, POP_S .24, RING_S .6, GLYPH_DELAY_S .1, GLYPH_S .26, LABEL_DELAY_S .08, LABEL_S .3, GLINT_DELAY_S .25, GLINT_S .8, FADE_S .3`; `MOMENTS` dict done/enabled/disabled/copied and `CANCELED`; `SPARKLES`, `_AFTERGLOW`, glyph polylines (`check`, `cross`, `bang`, `clip`) with pen widths; pure helpers `phase`, `ease_out_cubic`, `ease_out_back`, `exit_opacity`, `shake_offset`, `glyph_path`; draw helpers for sparkles, converge, collapse, pill flash, core flash, slam disc, rings, edge flash, disc, glyph, afterglow, label (glint gradient 28 px band), confetti (40 particles, speed 170-280, y squashed 0.35, life decay 0.6/s, damping 0.92), bubbles (72, speed 190-360, y 0.6, life decay 0.35/s, damping 0.93). The glyph is "cut out" by drawing in the pill's own `overlay-bg` colour at full alpha.

### ui_qt/overlays/tidy_lines.py (91): Cleaning up / Rewriting art
`PERIOD_S` 2.1, `COMB_S` 1.35, `LOOSEN_AT_S` 1.75, `LOOSEN_S` .35, `APPEAR_S` .35, `HALF_SPAN` 66, `LINE_PITCH` 9, `WORD_GAP` 5, `STROKE` 3; word layout from `random.Random(7)` (words 8-24 long, dy +-3.5, dx +-4, phase) built at import; `draw(painter, cx, cy, now)`.

### ui_qt/waveform_styles/__init__.py (3) and particle_style.py (343)
- `round_pen`, `Particle` (life decays 0.5/s, damping `damping ** (dt*30)`, `get_qcolor` for the dark/light branches, `get_fading_color`), `ParticleStyle` (fixed 1/120 s sub-stepping, emission remainders, `emitter` hook, states recording/streaming, processing vortex with 4 arms 120/s, transcribing converge 90/s, drift step with turbulence, wind, wall bounce 0.8, floor at `height-25` bounce 0.6, max particles trimming, glow ring alpha 100 when life>0.5, built-in status text at `(0, h-25, w, 20)`).
- Used by moments (`Particle`, `round_pen`), by the overlay (`ParticleStyle`) and by Classic.

### ui_qt/widgets/language_menu.py (83)
Module functions `populate(menu, ui)`, `select(ui, code)`, `cycle(ui)`, `_switch`. Uses `services.dictation_language` and `settings_manager.mutate_settings`. Contract with `ui`: `ui.overlay`, `ui.set_status(str)`, `ui.open_settings_destination(VOICE_MODEL)`. `notice=True` (shortcut/chip/bar) shows `overlay.show_language_notice()` only if the overlay is hidden or already in language state; tray selection never shows the notice.

### ui_qt/widgets/recording_look_preview.py (241): `RecordingLookPreview(QWidget)`
- objectName `recordingLookPreview`; children: `overlay` (preview-mode WaveformOverlay, hidden at construction), `source_label` (`WrappedLabel`, objectName `infoLabel`), `mic_button` (`Button`, `neutral_button`, checkable, left-aligned, hidden when no recorder factory).
- Layout: VBox margins 0, spacing 6: [stage HBox: margins (0,6,0,6), stretch, overlay, stretch, strut = overlay.height()], source_label, mic_button. Size policy Expanding/Preferred.
- Timer 33 ms; stages `(recording 0.0, processing 4.8, transcribing 6.6, done 8.4)`, loop 9.4 s; sample voice: two phrases (0.5-2.1 s and 2.9-4.6 s) of words (0.14-0.32 s long, peak RMS 0.04-0.16, gaps 0.03-0.12 s, `random.Random(3)`), `sample_level(t) = 0.0025 + 0.001*sin(37t)` floor, words `peak * sin(pi*(t-start)/length)**0.7`.
- Behaviour: `showEvent` restarts and starts timer; `hideEvent` stops timer first, unchecks the mic (which closes the recorder), hides the overlay; `set_look(look)` replays; microphone level arrives by queued signal `_heard(float)` from the audio thread; `_emit_heard` swallows `RuntimeError` after deletion; mic failure unchecks toggle without signals and shows `Couldn't open the microphone: <reason>.`.
- `preview_path()` = `%TEMP%/openwhisper_overlay_preview.wav` (throwaway take spool).

## UI details for the overlay and its parts

There is no layout tree or QSS in the overlay; everything is painted. The look depends on the following values, all in pixels in the overlay's 300-wide coordinate space (palette tokens in parentheses; values for dark / light from `ui_qt/utils/palette.py`).

**Pill**
- Standalone: rounded rect inset 0.5, radius 12; fill `overlay-bg` (dark rgba(28,28,30,.93), light rgba(252,252,253,.96)); 1 px `overlay-border` (dark rgba(84,84,86,.67), light rgba(0,0,0,.14)); if accented (command mode or destination) an extra 1.6 px `accent` alpha 210 ring at inset 1, radius 11.5.
- Embedded: plain rect fill `bg`, 1 px border `accent` when accented else `border`.

**Badge row** (top, y 9, height 18, side margin 9, radius 9 standalone / 2 embedded)
- Badge: x 9, width = text advance + 18 (+13 with lock); Segoe UI 8 DemiBold; fill opaque `overlay-bg` then `accent` alpha 46; text and lock colour `accent-soft`; lock = 8x6 body radius 1.5 at (x, cy-1), shackle arc 4.2 wide, pen 1.4 round; text starts x+9 (+13 with lock).
- Language chip: right edge at width-9, width `max(30, advance + 18)`, Segoe UI 8 Bold; layers: `overlay-bg`, `overlay-rgb` alpha 22 (38 hovered), `accent` flash alpha `170*flash`; 1 px edge `overlay-border` (accent alpha 220 when hover or flash); text `overlay-text` (`on-accent` when flash>0.5); pointing-hand cursor on hover.
- Snap / Done: each width `12 + 16 + 4 + textAdvance` with label, or 26 icon-only; gap 5; placed right to left, left of the chip (chip width + 5); labelled only if both fit between badge (width + 5) and the right limit. Snap: `accent` fill alpha 215 (255 hover), `on-accent` ink, camera icon (12x8 rounded body radius 2, bump, 2.2 lens, pen 1.3). Done: `overlay-bg` + `overlay-rgb` alpha 22/38 + 1 px edge (`overlay-border`, `accent` alpha 220 hover), `overlay-text` ink, check icon (pen 1.6). Icon centre at left+8+6; text at icon+6+4. Cursor pointing hand when hovering a control.

**Compact band** (80 px tall, starting at `band_top`): look drawn around `CENTER_Y` 29 within the 56 px look height; label row at `band_top + 80 - 25`, 20 px tall, side inset 12.

**Fonts**: all Segoe UI (Qt substitutes on Linux/macOS: the port must pick a UI family and keep the sizes in points).

**Animations (durations)**: look grow-in 0.4 s (delay 0.05, ease_out_cubic); label rise 0.3 s (6 px, ease_out_cubic); disc pop 0.24 s overshoot 2.2; rings 0.6 s (second ring +0.09 s); edge flash 0.6 s alpha `170*(1-t)^2`; pill flash 0.45 s; glint 0.8 s after 0.25 s; stage crossfade 0.28 s; fade out 0.25 s; exit fade 0.3 s before hide; chip flash 0.6 s; caption fade in 0.15 / out 0.35; head shake `amp*sin(2*pi*wiggles*t)*(1-t)` (Canceled 4 px 3.5 wiggles over 0.4 s; problem notice 2.6 px 3 wiggles over 0.36 s starting 0.2 s).

**Shortcuts**: none inside the overlay; Snap/Done show the Screenshot and Walkthrough hotkeys (`set_control_hotkeys`) in a hover pill. **Tooltips**: none on the overlay itself.

**Visible strings** (exact): `Recording...`, `Listening...`, `Listening for an edit...`, `Processing...`, `Transcribing...`, `Cleaning up...`, `Rewriting...`, `Canceled`, `Enabled`, `Disabled`, `Copied!`, `Command Mode`, `Hands-free`, `Snap`, `Done`, `Splitting (%.1f MB)...`, `Error`; when the live label is used the trailing `...` is stripped (`Recording`, `Listening`, `Listening for an edit`). Settings strings (look labels/details, `Show on the overlay`, `Live dot`, `“Recording” label`, `Elapsed time`, tile title `Recording overlay` and its description) live in settings.py / settings_dialog.py:1985-2030 (area owning Settings).

### Added by check (overlay window, preview widget, language menu)
- `update_streaming_text(text, is_final)`: `is_final` is accepted and ignored; text is stripped, then height reapplied and repaint. `clear_streaming_text()` also reapplies height.
- Embedded mode extras: the overlay is constructed hidden and installs an event filter on the parent: parent Resize -> `_apply_height` + reposition near anchor; parent Hide -> hide the overlay. Embedded/preview overlays are `WA_TransparentForMouseEvents` and opaque (no `WA_TranslucentBackground`); standalone is translucent. Preview/embedded use `Qt::Widget` window type.
- `closeEvent` stops the frame timer, `hidden_timer` and caption timer.
- `hidden_timer` timeout calls `hide()` (so work/fade states still fade); caption timer is its own single-shot (2500 ms).
- `show_notice` when already in notice state and visible: replays (resets `animation_time`, particles, burst flag, restarts the timer with the new text length) instead of re-showing. Empty text is ignored.
- `_set_hover`: cursor is PointingHand over chip/Snap/Done, OpenHand when draggable, else default; hover on Snap/Done shows the hotkey pill only if a hotkey is set for it.
- Paint error path: logs "Error drawing waveform frame" once, then fills `overlay-bg` and draws centred `Error` (Segoe UI 10 bold).
- `update_audio_levels(levels)` keeps the first 20 levels; with a live look the ribbon hears `max(levels)` and the particle style is fed `[ribbon.loudness]*n`; Classic feeds the raw list and its emission uses the mean (`audio_energy`) times `audio_response`.
- `set_recording_look` rewires the particle style: `emitter` = look's emitter or None, `audio_response` = 2.6 live / config (1.5) Classic, `emission_rate` = look's rate or the default 30.
- `RecordingLookPreview(recorder_factory=None, microphone_name=None, parent=None)`: `microphone_name` callable names the mic for `Listening to <mic>. Nothing is recorded.` (falls back to `your microphone`); missing failure reason gives plain `Couldn't open the microphone.`; mic toggle label `Try with my microphone` / `Stop microphone`; `set_label_parts(dot, text, clock)` passes through to the overlay.
- `language_menu.populate` on a settings error logs and shows `No other languages`-style disabled row; `Choose languages…` is always last.

## Data and formats

**Settings keys** (all in `services/settings.py`, read via `settings_manager.load_all_settings()` + `resolve_*`):

| Key | Type | Default | Read in |
|---|---|---|---|
| `recording_overlay_look` | choice of 11 + aliases | `"bars"` (`config.RECORDING_OVERLAY_LOOK`) | `refresh_recording_look` |
| `recording_overlay_dot` | bool | true | same |
| `recording_overlay_text` | bool | true | same |
| `recording_overlay_clock` | bool | true | same |
| `streaming_overlay_font_size` | int clamp 10-48 | 16 | `__init__`, `refresh_streaming_font_size` |
| `streaming_overlay_layout` | `header` / `footer` | `header` | `__init__`, `refresh_streaming_layout` |
| `dictation_languages`, `dictation_active_language` | list / str | (language area) | via `services.dictation_language` |

**Config constants** (`config.py`): `WAVEFORM_OVERLAY_WIDTH` 300, `WAVEFORM_OVERLAY_HEIGHT` 80, `WAVEFORM_STREAMING_MAX_HEIGHT` 400, `WAVEFORM_FRAME_RATE` 30, `WAVEFORM_LEVEL_SMOOTHING` 0.3 (unused in this area), `OVERLAY_HIDE_DELAY_MS` 1500, `CANCELLATION_ANIMATION_DURATION_MS` 1200, `WAVEFORM_STYLE_CONFIGS['particle']` (max_particles 150, emission_rate 30, particle_life 2.0 unused here, gravity 20, damping 0.98, wind_strength 5, audio_response 1.5, glow_effect, turbulence_strength 10, color_shift_speed 50). `STREAMING_OVERLAY_*`, `RECORDING_OVERLAY_LOOK` defaults as above.

**Existing installs depend on**: the look value strings and aliases (`live`, `orb`, `bubbles`), the setting key names, and the temp preview path name `openwhisper_overlay_preview.wav`. No database tables, no wire protocols. The overlay's hands-free state is read by Omarchy controls over D-Bus (`omarchy_controls.py:209`, `getattr(overlay, "hands_free", False) is True`).

**Palette tokens used** (both themes, defined in `ui_qt/utils/palette.py`): `bg`, `border`, `accent`, `accent-soft`, `accent-cyan`, `on-accent`, `success`, `danger`, `warning`, `purple`, `overlay-bg`, `overlay-border`, `overlay-text`, `overlay-rgb`, `warning-text`, `warning-text-strong`, `danger-text`, `success-text`, `purple-text`. Dark: accent-soft #6fb1ff, accent-cyan #64d2ff, danger-text #ff453a, success-text #30d158, warning-text #ff9f0a, warning-text-strong #ffd08a, purple #bf5af2, purple-text #d29bf5, overlay-text #f5f5f7. Light: accent-soft #0066cc, accent-cyan #0b7fcf, danger-text #d70015, success-text #1e8e3e, warning-text #b25e00, warning-text-strong #8a4b00, purple #af52de, purple-text #8e3fc4, overlay-text #1d1d1f. (`is_dark` property picks the look's colour branch.) `token_color(token, alpha)` returns the token with alpha replaced.

## External dependencies

| Python / OS API | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| PyQt6 `QPainter`, `QPainterPath`, `QLinearGradient`, `QConicalGradient`, `QPen`, `QFontMetrics`, `QTextLayout` | all drawing | identical Qt 6 classes (near 1:1 translation); `QPainterPath::arcTo`, `quadTo` exist |
| `QTimer`, `QElapsedTimer`, `time.monotonic` | frame loop, durations | `QTimer`, `QElapsedTimer` / `std::chrono::steady_clock` |
| `QCursor.pos`, `QApplication.screenAt` | pointer placement | `QCursor::pos`, `QGuiApplication::screenAt` |
| `random.Random(seed)`, `random.uniform/randint` | deterministic scribble, school, sample voice; random confetti and particles | `std::mt19937` (+ `std::uniform_real_distribution`); seeds cannot reproduce Python's Mersenne output streams exactly unless the init is ported, so accept statistical equivalence |
| `itertools.pairwise`, `deque(maxlen)` | glyph path, history | loops, ring buffer (`std::array<float,100>` + index) |
| `ctypes`/AppKit via `ui_qt.snap.macos` (`make_nonactivating`, `set_level`, `PARKED_LEVEL` 27) | non-activating panel and window level on macOS | Objective-C++ shim (`NSPanel` style mask, `setLevel`), other-platform only |
| `WS_EX_NOACTIVATE` via Qt `WindowDoesNotAcceptFocus` + `WA_ShowWithoutActivating` | focus safety on Windows | same Qt flags |
| `QToolTip`, `HotkeyHoverHint` (ui_qt/widgets/buttons.py) | hotkey pill on control hover | another area's widget; port with Buttons area |
| `AudioRecorder` (`set_audio_level_callback`, `start_recording`, `cancel_recording`, `cleanup`, `last_start_error`) | Settings preview mic | OpenWhisper_CPP audio capture class (other area) |
| `services.dictation_language`, `settings_manager` | language lists, labels, settings | C++ settings store and a small language module |

## Platform-specific code

- **Windows**: standalone overlay uses frameless Tool + `WindowDoesNotAcceptFocus` + `WA_ShowWithoutActivating` (maps to `WS_EX_NOACTIVATE`). Clicks on chip, Snap, Done work (platform name `windows`). A test verifies the ex-style bit.
- **X11 (`xcb`)**: same flags; clicks allowed (input hint).
- **macOS (`cocoa`)**: `WA_MacAlwaysShowToolWindow` so the Tool window survives app deactivation; clicks are accepted only after `macos.make_nonactivating(self)` succeeds (NSPanel non-activating); `park()` raises window level to `PARKED_LEVEL` (27, above the snap cover at 26) and `unpark`/`_finish_hide` restore it. These calls do nothing off Cocoa.
- **Wayland / Hyprland (Omarchy)**: windows cannot position themselves, so `ui_controller.py:266` constructs `WaveformOverlay(main_window)` when `is_wayland()`: embedded mode (inert child, bottom-right, no clicks, no edge flashes, square corners, hidden whenever parent hides, max height = parent height / 3). Hotkey hint falls back to `QToolTip` (`compositor_managed()`). Hands-free state is read by the Omarchy bar even while hidden. Chip/Snap/Done are display-only on any platform other than windows, xcb, cocoa.
- Theme: `current_palette().is_dark` is evaluated on every colour call (theme can change at runtime).

## Engine-specific code

- Nothing in the drawing or looks is engine-specific: levels come in as RMS 0-1 floats (`update_audio_levels(list[20])`, overlay uses `max(levels[:20])`; Classic uses the mean via `ParticleStyle`).
- `language_menu.py` calls `languages.single_language_reason(settings)`, which in `services/dictation_language.py:93-110` returns per-engine text (Moonshine, Parakeet Unified, Parakeet MLX, English-only Whisper): **drop** those branches. For Parakeet on NeMo the reason text must be rewritten to whatever NeMo-Speech.cpp supports (Parakeet TDT v3 is multilingual with automatic language detection; the "AUTO" chip label exists for that). Whether language choice is exposed at all for Parakeet is an owner decision (see open questions).
- The overlay's `show_caption("Switched to <mic>")`, `set_processing_label("Retrying connection (n/3)...")` and `set_recording_destination` are fed by cloud/remote engine and walkthrough flows: **undecided backend** for the retry label (cloud API / remote computer), port the API regardless (it is a string setter).
- In-process Parakeet: the engine thread is separate from the GUI thread; the overlay must keep receiving `update_audio_levels` from the recorder via queued signals; no engine call is made from the overlay.
- Streaming preview text (`update_streaming_text`) is fed by streaming transcription. Parakeet TDT is offline-only (CLAUDE.md): live preview means windowed `Recognizer::transcribe`, so words arrive in bursts (replace-whole-text semantics already match: the overlay takes "full preview transcript so far"). **Nemotron streaming** (undecided) would produce true incremental text through the same API.

## Dependencies on other areas

**Imports out**: `config` (constants), `services.settings` (resolvers, `StreamingOverlayLayout`, `settings_manager`), `services.dictation_language`, `ui_qt.utils.palette` (`token_color`, `current_palette`, `is_dark`), `ui_qt.utils.loudness`, `ui_qt.utils.overlay_position`, `ui_qt.utils.drag_move`, `ui_qt.snap.macos`, `ui_qt.widgets.buttons` (`show_hotkey_hint`, `hide_hotkey_hint`, `Button`, `neutral_button`), `ui_qt.widgets.wrapped_label`, `ui_qt.dialogs.settings_destinations.VOICE_MODEL`.

**Called from** (contract = the public surface list in the waveform_overlay module section):
- `ui_qt/ui_controller.py`: construction (`WaveformOverlay(main_window if is_wayland() else None)`, line 266), signal hookups (498-500), recording/processing/streaming/cancel/copied/large-file flows (674-1000, 1438-1467), `set_walkthrough_controls` (1139), `set_stand_in` (1167), `show_caption` (1212, 1372), `set_hands_free` (1367), `set_control_hotkeys` (2537), Settings callbacks `on_streaming_font_changed`, `on_streaming_layout_changed`, `on_recording_look_changed` (1617-1619), `language_menu.cycle/populate` (1352-1413), `overlay.close()` on exit (2912).
- `services/application_controller.py:1268`: `set_recording_destination`.
- `services/omarchy_controls.py:209`: reads `overlay.hands_free`.
- `ui_qt/dialogs/settings_dialog.py:1985-2125, 4134`: `RecordingLookPreview`, label switches, look bar; `ui_qt/system_tray.py:34, 81, 158`: `language_menu_requested` signal and "Language" submenu.
- Settings dialog toggles `recording_parts.setEnabled(look != legacy)` (Classic has no live label).

## Tests

Pure logic (golden-vector candidates) is listed at the end of this section.

| Test file | Covers |
|---|---|
| tests/test_recording_look.py (583) | setting defaults/aliases/fallback, look list sync with settings, VoiceRibbon behaviour, level mapping (live vs legacy), particles leave from ribbon, all looks x states x layouts paint in dark and light, new recording resets ribbon and clock, `format_clock`, `refresh_recording_look`, label-part combinations, preview widget lifecycle (timer only while shown, mic open/close, failure message, no toggle without factory), Settings load/save/push of look, layout and label switches, pulse ripple trigger, school heaves swell and keeps layer order, bait ball swing >12 px between talking and pause, Fizz fills with loudness (<0.45 quiet, >0.6 normal) and drains, every look's finish, work-rate ease 1.0 -> 1.7 without clock jump, label crossfade, cleaning hands off from every finish, fade-out then hide, new recording cancels the fade, Classic vortex, preview loop stages |
| tests/test_overlay_moments.py (146) | each toggle/copy moment, confetti waits for disc, Cancel implodes then slams then bursts (>40 bubbles) then goes idle (also from hidden), Disabled confetti falls, `exit_opacity` bounds, problem notice never celebrates, reconnect label persists and notice replaces fade, language notice and splitting paint, glyph path lengths 0 < half < whole |
| tests/test_personalize_s8_overlay.py (288) | focus flags, embedded ignores mouse, WS_EX_NOACTIVATE on Windows, chip visibility rules, click emits signal only on the chip, display-only where clicks could steal focus or when embedded, hover cursor, chip flash on language change, quiet language change while hidden, notice then recording keeps overlay, language notice timer and reset, hands-free latch lifecycle, caption lifecycle (2500 ms, cleared on hide/transcribing, ignored while hidden), command listening and rewriting states, all states in both themes, chip text from language module |
| tests/test_streaming_overlay_layout.py (173) | layout default/fallback, header clear of badge/chip/words, header shorter than footer, footer geometry, caption takes header room, Snap/Done push clock and look to their own line, compact look sits below controls (`height == base + top`), work stages shifted by `_TOP_ROW` (27), drop back to compact when badge row empties, layout change reflows, every look draws small and leaves `band_width` None |
| tests/test_drag_to_move.py (235) | open by pointer, drag aside, drag survives recording/growth/`show_at_cursor`, next recording re-anchors, cannot be dragged off-screen (clamps to screen top-left + 8), Snap click does not drag, preview/embedded have no drag, walkthrough corner (+16), park/unpark, hidden overlay not parked |
| tests/test_personalize_fix3_h1_outcome_notices.py (350) | notice shows while hidden, long text stays longer, "done" confetti after sparkle gather and replays, problem notice no confetti, ellipsis shortening `_fit_notice` (240x64 rect), recording/jobs keep overlay and show caption, plus controller-level notice wording (other area) |
| tests/test_personalize_s8_language_menu.py (150) | `populate` texts/check state, `select`, `cycle`, notice rules, single-language wording, failure status |
| tests/test_walkthrough_buttons.py (779) | Snap/Done visibility, click, hover cursor and hotkey hint, label dropping beside the chip, no controls where clicks could take focus or when embedded, capture leaves overlay as it was, stand-in behaviour |
| tests/test_omarchy_ui.py, tests/test_application_controller.py, test_desktop_performance.py, test_personalize_foundation_ui_shell.py, test_agent_recording_indicator.py, test_automation_r3/r4_* | touch the overlay API indirectly (controller flows, Omarchy bar, performance budgets). Not read in full: other areas own them |

**Golden vector candidates** (pure functions from Python replayed against C++):
1. `loudness.level_to_height(level)` for a sweep of levels (-80 to 0 dBFS).
2. `VoiceRibbon` fed a recorded sequence of `(level, dt)`: `loudness`, `shown`, `history`, `height_at`, `crest`.
3. `moments.phase`, `ease_out_cubic`, `ease_out_back` (several overshoots), `exit_opacity`, `shake_offset`, `smooth`.
4. `moments.glyph_path` polyline lengths at several `amount` values (and the exact vertices).
5. `live_recording.format_clock`, `wordless_label_width` is font-dependent (do not vector).
6. `overlay_position.preferred_overlay_position`, `clamp_rect_to_available`, `max_height_for_anchor`.
7. `recording_looks.normalize_look` and `smooth`, `hue_color` HSV math for both themes.
8. `Visual` simulations with injected RNG: Aurora, Pulse ripple trigger, Fizz (`h` after N ticks), `_School` spring and layer-ordering invariants (use thresholds, since RNG differs).
9. `RecordingLookPreview.sample_level(t)` and `_sample_words(3)` word table (requires porting Python's `random.Random(3)` or dumping the table as data).
10. `tidy_lines._scribble(7)` word table (dump as constant data) and `draw` geometry numbers.
11. `ParticleStyle.advance` outputs with a fixed RNG stream.

## Porting notes

**Decomposition**
- Qt-free core (fits `src/engine`-style rule? No: the overlay is UI. Put it in `src/app/overlay/` but keep a Qt-free-ish math layer testable via `QtGui` only, or a pure `src/core` with plain structs): `loudness`, `VoiceRibbon`, easing/phase math, `Moment` table, glyph tables, `tidy_lines` data, `ParticleStyle`/`Particle` simulation (using a small `Color`/`Point` struct), overlay placement math, school/Fizz simulations, sample-voice generator, a `LookState` machine (`OverlayState`, timers, work rate, label crossfade) that is independent of painting.
- Qt layer: `WaveformOverlay` widget (paintEvent, mouse, timers, window flags), one `Visual` subclass per look with `draw(QPainter&, double grow)` and `drawWork(...)`, `RecordingLookPreview` widget, `languageMenu` functions.
- Keep `Visual` as an abstract class with the same virtuals; the 11 looks are table-driven by id; `make_visual` factory.

**Order of work**: 1) palette + token bridge, loudness, easing, VoiceRibbon, overlay_position, drag move (and golden vectors); 2) `WaveformOverlay` shell: window flags, placement, state machine, timers, background, Classic look with `ParticleStyle`, status text; 3) Bars (default) + live label + entry flash; 4) work states + tidy_lines + fade-out; 5) moments (disc/rings/glyph/confetti/Canceled), notices, caption; 6) badge, chip, header/footer preview text layout, controls, hover; 7) other looks (Ribbon, Wave, Aurora, Pulse, Dots), then the school looks and Fizz; 8) preview widget and language menu; 9) Wayland embedded mode and macOS shims last.

**Risks**
- Frame-exact visual parity is judged by eye: use Python screenshots of every look at fixed seeds/times (`grab()` offscreen in tests) as references; the RNG differences make the school and confetti non-reproducible, so match statistics and thresholds from tests instead.
- Font: Segoe UI exists only on Windows; metrics-dependent layout (chip width, badge width, label elision, header fit) shifts with a substitute font. Pin a font family fallback list and test with it.
- `QTextLayout`-based embedded preview and `QFontMetrics::boundingRect` word wrap semantics must be preserved (height growth depends on them).
- Paint cost: school looks do per-frame path building for 5 layers x 48 points and O(n^2) flocking on 56 fish; Python manages it, C++ will be far faster but keep dt clamped to 0.1 s and substeps equal.
- Focus safety on Windows is easy to regress (any activation steals the paste target); keep a Windows test of `WS_EX_NOACTIVATE`.
- State machine has many subtle orderings (fade cancel, stand-in, transient timer stop, hands-free survive-while-hidden). Port with the tests, not from memory.
- `Visual.band_width` is mutated during small-look drawing (shared state); in C++ pass a draw context instead.

**Rough C++ size**: about 4500-5500 lines (waveform_overlay ~1500, looks ~1700 incl. ocean ~600, moments ~450, live_recording ~200, particles ~300, tidy_lines ~90, preview ~250, language menu ~100, math/placement/drag ~200, plus tests).

**Native gains**
- 30 fps timer could track the display refresh (`QWindow::screen()->refreshRate`) or use `QPropertyAnimation` clocks; keep 30 fps default to match.
- Preallocate paths and use `QVector<QPointF>` buffers instead of per-frame `QPainterPath` rebuilds; fixed arrays for ribbon history.
- Cache `QFontMetrics` results (chip/badge widths) until the font or text changes.
- Spatial grid for flocking (only 56 fish, optional).
- Overlay can read levels straight from the C++ audio thread through an atomic float and a queued signal, removing the Python callback hop.
- Optionally a QOpenGLWidget/RHI path for the fish looks if profiling shows need (probably not).

**Differences to state in the port**: Moonshine/Whisper single-language reasons removed; Parakeet's language-detect behaviour defines the language menu; embedded overlay on Wayland is kept as a Qt platform case, not Omarchy-specific engine code.
