# UI foundation: theme stylesheet, palette, fonts, icons, shared widgets

Area `a02-ui-foundation`. 49 Python files, ~13.1k lines, of which `theme.qss` is 6119.
Source root: `D:\coding\whisper_local` (all paths below are relative to it).

Reading note: every `.py` file was read in full. `theme.qss` was read in full for lines 1-2430, 2610-2640 and 4036-4300 (all the generic and shared rules); the remaining ~3.4k lines are per-feature rules keyed by object name for widgets owned by other areas (Meeting, Upload, Settings pages, Stats, Hooks, Actions, Scratchpad...). They were inventoried by section header and selector, not rule by rule. Because C++ Qt Widgets uses the same QSS engine, the porter should copy the file, not rewrite it (see Porting notes).

## What the user gets

- One look for every window: Apple-system-grey dark theme (default) and a light theme, rounded surfaces, system-blue accent, green/red/amber status colours. Both themes come from one stylesheet template whose colours are `@token` names.
- Four theme choices (Settings): Dark, Light, "Match system" (follows the OS colour scheme live), and "Omarchy desktop" (Linux tiling-desktop palette read from the user's Omarchy theme file; changes within ~2 s). Choice applies instantly to every open window, including widgets with their own stylesheet and painted widgets.
- Four UI text sizes (Settings): Small 90%, Default 100%, Large 115%, Extra large 130%. Only font sizes scale; paddings and control sizes stay; widgets that carry pixel sizes (switch, help card, studio stage) scale themselves from the same factor.
- The app icon: a blue-to-cyan gradient rounded square with a white microphone, drawn in code (simplified filled mic below 32 px for the tray), unless a bundled `.ico` exists.
- Shared controls that give Settings and the tabs their feel: standard/primary/danger/success/warning buttons with a hover pill showing the keyboard shortcut; on/off switch; setting tiles (icon, title, description, trailing control, indented body); segmented choice bar; chapter-and-page navigation rail with per-page value captions; collapsible card headers with a 300 ms eased height animation; "New"/"?" feature badge on buttons; help cards that open on hover or Enter and stay open while their links are used; searchable fuzzy-filter combo; elided labels and combos that never push the window wider; spin box with a stacked chevron stepper; wrapped labels that wrap long hashes/paths; animated progress bar; decode-in-place text label; empty-state box; update-available pill in the title bar; rounded tooltips with a 200 ms delay.
- Plain behaviours users notice: mouse wheel never changes a combo/spin box unless it has focus; popups of elided combos widen to the longest item; dialogs larger than the screen scroll instead of clipping (compositor-managed desktops); frameless overlays can be dragged and stay on the pointer's screen; "Show in folder" selects the file in Explorer/Finder; Markdown transcripts render with theme typography; GitHub release notes render as styled rich text with checksums stripped; macOS Dock icon disappears while only the tray remains.

## Feature inventory

Disposition vocabulary: port / drop (engine-specific) / undecided backend / other-platform only. "Undecided" here is only used where a feature depends on cloud/remote/Nemotron decisions.

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A02-01 | Palette tokens (dark, light) | 96 role names per theme, identical key sets (tested). Dark: bg #1c1c1e, surface #2c2c2e, text #f5f5f7, accent #0a84ff, success #30d158, danger #ff453a, warning #ff9f0a. Light: bg #f2f2f7, surface #ffffff, text #1d1d1f, accent #007aff, success #34c759, danger #ff3b30, warning #ff9500. `-rgb` tokens are bare "r, g, b" for `rgba(@x-rgb, a)`. `icon-suffix` = "" (dark) or "-light" (light). Unknown theme name -> dark | `ui_qt/utils/palette.py:33-246,315` | port |
| A02-02 | Token substitution | `@name` -> value via regex `@([a-z][a-z0-9]*(?:-[a-z0-9]+)*)`; unknown token raises KeyError (a typo must not ship as literal text). Text without "@" returned untouched | `palette.py:30,286-295` | port |
| A02-03 | Token to QColor | Handles `#hex`, `rgba(r, g, b, a)` (alpha is 0-1 float, rounded by QColor.setAlphaF), and "r, g, b" triples; optional alpha override 0-255 | `palette.py:265-284` | port |
| A02-04 | Current-palette global | Process-wide current palette (dark default); painted widgets read it via `token_color()` | `palette.py:302-326` | port |
| A02-05 | theme.qss template | 6119 lines, 929 rule blocks, 636 distinct `#objectName`s, 79 distinct tokens. Relative `url(ui_qt/assets/...)` rewritten to absolute posix paths at load (regex skips quoted/absolute/drive-letter urls). Missing file: logged error, UI unstyled | `ui_qt/styles/theme.qss`, `theme_manager.py:20-22,52-71,160-182` | port |
| A02-06 | ThemeManager | Owns template + current Palette; `stylesheet` = Omarchy-rewrite(palette.resolve(template)); `scaled_stylesheet(scale)`; `set_theme(name, desktop_palette)` returns True only if palette changed (value-equality), emits `theme_changed(str)` once; `apply_platform_palette` sets QPalette (19 roles + disabled text = text-muted) and pins the colour scheme | `theme_manager.py:25-157` | port |
| A02-07 | QPalette mapping | Window=bg, WindowText/Text/ButtonText=text, Base/Button=surface, AlternateBase=bg, ToolTipBase=surface-hover, PlaceholderText=text-secondary, Highlight/Link=accent, HighlightedText=on-accent, LinkVisited=accent-soft, Mid=border, Dark=border-strong, Light/Midlight=surface-hover, Shadow=shadow-rgb. Disabled WindowText/Text/ButtonText = text-muted | `theme_manager.py:125-157` | port |
| A02-08 | Theme preference resolution | `UiTheme` dark/light/system/omarchy; light->LIGHT, anything other than system->DARK (garbage -> dark), system -> Qt colour scheme (Light->LIGHT, else DARK). Explicit choice pins `styleHints().setColorScheme` (window frame matches); "system" must `unsetColorScheme()` instead and never pin it (would silence colorSchemeChanged). Qt < 6.8 AttributeError swallowed | `font_scale.py:115-185` | port |
| A02-09 | Font scaling | Multiplies every `font-size: Npx|Npt` in a stylesheet; regex `(font-size:\s*)(\d+(?:\.\d+)?)(px\|pt)` case-insensitive. Integer value: `max(1, int(round(v*scale)))` (Python banker's rounding); decimal value: one decimal, trailing zeros/dot stripped. Scale 1.0 (within 1e-6) returns the input string unchanged. Other lengths untouched | `font_scale.py:22-63` | port |
| A02-09b | App font | `QApplication.setFont(QFont(family, max(6, round(10*scale))))`; family "Segoe UI", or "Noto Sans" when Omarchy UI is active | `font_scale.py:28,108-111` | port (Windows) / other-platform only (Noto) |
| A02-10 | Widget-local stylesheet resolution | Every widget stylesheet is stored in dynamic property `unscaledStyleSheet` (source) and `scaledStyleSheet` (last resolved); theme/size change re-resolves from source so scales never compound. Only sheets containing "font-size" or "@" are touched. A re-entrancy flag stops the StyleChange from recursing | `font_scale.py:26-27,75-78,188-232` | port |
| A02-11 | WidgetStyleFilter | App-wide event filter: on `QEvent::StyleChange` of any QWidget, treats a newly set stylesheet as designed source and resolves it (covers menus/dialogs created later). Returns False always | `font_scale.py:235-249` | port |
| A02-12 | Platform theme change follow | `colorSchemeChanged` re-applies when preference is "system" | `ui_qt/app.py:122,142-145` | port |
| A02-13 | Omarchy palette loader | Reads first valid of `$XDG_STATE_HOME/omarchy/current/theme/colors.toml`, `$XDG_CONFIG_HOME/omarchy/current/theme/colors.toml` (defaults `~/.local/state`, `~/.config`). Keeps only string values matching `#[0-9a-fA-F]{6}\Z` (so CSS injection is impossible); needs `background` and `foreground`; else falls back to dark palette. `mode` = "dark"/"light" else luminance of background < 0.5 | `omarchy_theme.py:13-29` | other-platform only |
| A02-14 | Omarchy colour mixing | Derives all 96 tokens from bg/fg/accent via `mix` (per-channel round of linear blend): surface = `lighter_background`/`lighter_bg` or mix(bg,fg,.06); hover .12; border .24; border-subtle .13; secondary .74; muted/faint .48; accent from `accent`/`blue`/`color4`/fg; accent-hover mix(accent,fg,.14); accent-pressed mix(accent,bg,.16); tints mix(bg,accent,.10/.18/.4/.6/.22); on-accent = bg if accent lightness > .5 else white; success/danger/warning/purple from green/red/yellow/magenta or color2/1/3/5 (tint variants 18%); `-rgb` recomputed. Monochrome themes must keep every role (groups are role lists, not colour-keyed) | `omarchy_theme.py:32-164` | other-platform only |
| A02-15 | Omarchy QSS rewrite | When Omarchy UI active: every `border*-radius` becomes `2px`, every `font-family` becomes `"Noto Sans", "DejaVu Sans", sans-serif` in global and local QSS | `omarchy_theme.py:167-180` | other-platform only |
| A02-16 | Omarchy theme poll | Every 2000 ms stat both theme files (inode, mtime_ns, size) and re-apply only when the signature changes; only for preference system/omarchy. File watchers are not used because Omarchy replaces the directory atomically | `ui_qt/app.py:123-140` | other-platform only |
| A02-17 | Desktop-session detection | `OPENWHISPER_UI` env: `omarchy`/`classic` forces, `auto` (default) detects on Linux via `OMARCHY_PATH`, theme file present, or `/etc/os-release` `ID=omarchy`; cached | `services/desktop_session.py` (other area) | other-platform only |
| A02-18 | Tooltip delay | Proxy style returns 200 ms for `SH_ToolTip_WakeUpDelay` (platform ~700 ms) | `tooltip_filter.py:17-31` | port |
| A02-19 | Rounded tooltips | Event filter on `QTipLabel` Polish: translucent background + NoDropShadowWindowHint so QSS `border-radius: 8px` shows. On Wayland sets `QToolTip { border-radius: 0px; }` instead | `tooltip_filter.py:34-56` | port (Windows/macOS/X11) |
| A02-20 | App icon | `render_app_pixmap(size)`: gradient #0a84ff -> #64d2ff diagonal, rounded square radius 0.22*size; below 32 px a filled capsule (0.36,0.16,0.28x0.40) + 0.10*size pen cradle arc (min 1.5 px); 32 px up an outlined capsule (0.37,0.20,0.26x0.38, pen 0.055*size), cradle arc, stem and base line. `app_icon()` prefers bundled `ui_qt/assets/openwhisper.ico`, else QIcon with pixmaps at 16,24,32,48,64,128,256; cached for process lifetime | `app_icon.py` | port (rebrand?) |
| A02-21 | Tabler icon loader | `tabler_icon(filename)` cached by name, `tabler_pixmap(name, size)`, `design_icon` adds a Disabled-mode pixmap (24 px) so a disabled "current choice" button keeps its colour. 59 bundled SVGs incl. `-light` variants for chevrons/star/player/mic | `icons.py`, `ui_qt/assets/tabler` | port |
| A02-22 | Collapse animation constants | `maximumHeight` animation, 300 ms, InOutCubic, unlimited = 16777215; run replaces any in-flight run and fires `finished` once | `collapse_animation.py` | port |
| A02-23 | Overlay positioning math | `clamp_rect_to_available` (margin 8, pins to top-left inset if larger than screen, no-op for non-positive size/null rect), `preferred_overlay_position` (below-right offset 10, flips left/above if overflowing `edge - margin`, then clamps), `max_height_for_anchor` (max of room below/above, min with configured, >= 1) | `overlay_position.py` | port |
| A02-24 | Drag to move frameless window | Open-hand cursor, closed-hand while dragging; left button only; clamps to the screen under the pointer (`QApplication.screenAt`), margin 8; `moved` flag persists until `reset()` | `drag_move.py` | port |
| A02-25 | Fuzzy match and score | 5 strategies, first match wins: exact 10000; substring 3000 (+1000 at start or after "/", +500 after " -_:.", - min(100, 2*pos)); normalized-token substring 2200 (+800 start / +500 word, - position penalty); multi-token 1800 (+200 in order, +50 per exact token; query token must match a distinct target token: exact, then prefix, then infix if len >= 3); delimiter-stripped substring 1500 - position penalty; subsequence 700 (+20*run for adjacent chars, +40 per word-boundary char, - min(200, 3*span), floor 100, needs >= 2 chars). Empty query scores 0 (matches all); empty target -> no match | `fuzzy_match.py` | port |
| A02-26 | List reconcile | Reuse unchanged cards (by id + deep-copied fingerprint), create new, remove stale (hide, unparent, deleteLater), keep display order; also defines `HistoryDelivery` / `MeetingDelivery` cross-thread result signals | `list_reconcile.py` | port |
| A02-27 | Loudness curve | RMS fraction -> 0-1 via dB: floor -60 dBFS, ceiling -12 dBFS, level <= 0 -> 0 | `loudness.py` | port |
| A02-28 | Markdown render | QTextDocument GitHub-dialect import, then one pass writes typography: PREVIEW (13 pt, line 135%, gap 8, indent 22, code bg `code-on-surface`, quote `quote-on-surface`) and READER (15 pt, 155%, 12, 26, `code-on-bg`, `quote-on-bg`); heading scales 1.55/1.3/1.15/1.05/1.0/1.0 DemiBold in `text-heading`; code 0.9x in Cascadia Mono/Consolas/SF Mono/Menlo/monospace, fenced block lines are one slab (125% line height, gap only after last line); quotes tinted + left margin = indent, `text-soft`; links `accent-soft` no underline; tables: 1 px collapsed border, cell padding max(4, 0.75*gap), header row shaded; first block top margin 0; `scaled(f)` multiplies body_pt, gap (min 2), indent (min 12) | `markdown_render.py` | port |
| A02-29 | Release-notes HTML | GitHub markdown subset (headings, bullets, ordered, `---`, fenced, inline code, links http(s) only, bold, italic) to inline-styled Qt rich text; input capped at 6000 chars cut at a newline; drops checksum sections (`sha-256`, checksums, hashes, verification, artifacts, assets, downloads headings until a same/higher-level heading), bare digest lines (32+ hex chars, <= 3 words), "Full Changelog:" lines, HTML comments; all text html-escaped; paragraph lines joined by spaces; style colours are tokens resolved at render | `release_notes.py` | port |
| A02-30 | Restyle helpers | `repolish(widget)` unpolish/polish/update; `set_style_property(widget, name, value)` is a no-op when unchanged | `restyle.py` | port |
| A02-31 | Reveal in file manager | Windows: `explorer /select,"<path>"` as a raw command line (exit code ignored); macOS `open -R`; Linux/failure: open the containing folder; returns bool, never raises. `open_folder_in_file_manager` opens a directory itself, returns False if missing | `file_reveal.py` | port |
| A02-32 | Engine-selection relay | Session-lifetime QObject; one settings watch on 13 keys re-emitted as a Qt signal on the GUI thread so Quick Record, Upload and Settings reload their engine fields | `engine_selection.py` | port (keys adapt) |
| A02-33 | Agent-lookup relay | Session-lifetime relay for `installed_agents` lookups finishing on a worker thread -> `looked_up(agent_id)` on the GUI thread | `agent_lookup.py` | port (agent area) |
| A02-34 | Desktop (compositor) dialog fit | `on_screen_position(frame, available)`; `fit_desktop_dialog` bounds size to screen minus 64 px, wraps an oversized dialog's layout in a `QScrollArea` (only when its size hint exceeds the room), min size 240x120 bounded; `DesktopWindowFilter` strips window buttons and size grip on Polish and fits on first non-spontaneous Show, only when Omarchy UI is active; `without_window_buttons(flags)` | `desktop.py` | other-platform only (`on_screen_position` also used on Windows by the Settings dialog, `settings_dialog.py:1799`) |
| A02-35 | macOS Dock sync | Dock icon shown only while a regular Window/Dialog top-level is visible; Show sets Regular policy immediately and activates; Hide re-syncs next event-loop turn; Tool/Popup/ToolTip windows never count; installed only when a tray icon exists | `macos_dock.py` | other-platform only |
| A02-36 | Button family | `Button` (min 44 px high, Segoe UI 12, hand cursor, StrongFocus, min width 140 or fitted to label +40 / size hint), `PrimaryButton`/`DangerButton`/`SuccessButton`/`WarningButton` (object names `primaryButton` etc., min 48x140). `set_active(False)` keeps the widget enabled (still hoverable for the hotkey hint) but swallows mouse/Space/Enter/Return, arrow cursor, sets property `inactive="true"`. Helpers: `fit_compact_button` (>= 34 px, optional fixed width), `neutral_button` / `compact_primary_button` / `compact_danger_button` (property `tone`, min 34), `START_BUTTON_WIDTH = 340`, `fit_start_width` | `buttons.py:193-419` | port |
| A02-37 | Hotkey hover hint | Pill above the hovered button: padding 12/6, QSS font 11 px 600, letter-spacing 0.5 px, painted rounded (`overlay-bg` fill, `overlay-border` 1 px, half-pixel inset), translucent tool window, fade-in 140 ms OutCubic, 8 px above the button; hide delay 80 ms after Leave; only if hotkey set and button enabled. On compositor-managed desktops a plain QToolTip below the button reads "<key> · desktop shortcut" or "<key> · while OpenWhisper is focused" | `buttons.py:32-190` | port (compositor branch other-platform only) |
| A02-38 | Cards | `Card` (object `card`, margins 16, spacing 12, min height 100), `ControlPanel` (`controlPanel`, horizontal), `HeaderCard` (centred bold Segoe UI 14 title `headerLabel`, 12 px spacing under header). Collapsible variant: toggle "Show transcription output" / "Hide transcription output", starts collapsed state False=expanded param, `toggled(bool)` signal, content animated 300 ms; `set_collapsed(emit=False)` applies instantly (state restore) | `cards.py` | port |
| A02-39 | Collapsible section toggle | `QToolButton`, text `{prefix}{title}  ▾` (expanded) / `▸`, tooltip swaps, 11 px 600 `text-secondary`, hover `text`; `toggled_expanded(bool)`; `set_expanded(emit=False)` silent | `collapsible_header.py` | port |
| A02-40 | Animated progress bar | Object `animatedProgressBar`, default height 8, 16 ms tick (only while shown), track = `overlay-rgb` alpha 22, determinate fill eases 18% per tick toward target, gradient accent -> accent-cyan, minimum fill = bar height, white sheen band 70 alpha every 1900 ms while < 99.9%; indeterminate: 1500 ms smoothstep sweep of a 34% (min 56 px) gradient pill; `set_fraction(f, animate)`, `set_indeterminate`, `reset` | `animated_progress_bar.py` | port |
| A02-41 | Decode label | Rich-text label that reveals text left to right over 720 ms (32 ms tick), 4 scrambling glyphs from `0123456789ABCDEF#%&$@<>/\|` in accent colour ahead of the locked run; emphasised segments `text` + weight 600; starts on first show, finished text restored on hide; consecutive spaces pinned as `&nbsp;` | `decode_label.py` | port |
| A02-42 | Eliding label | Single-line label, floor width 56 + chrome, elides right (or given mode) to current width, tooltip = full text, `text()` returns the full text, size hint ceil(fractional advance) + stylesheet chrome (margins + 2*margin + implicit indent of one "x" when framed) | `eliding_label.py` | port |
| A02-43 | Downloads label | Eliding label that, if its text ends with " in Downloads.", keeps a clickable "Downloads" link (accent, underlined) visible when elided; emits `downloads_requested`; min width reserves "… Downloads." | `downloads_label.py` | port |
| A02-44 | Wrapped label | Word-wrap label whose size hints carry the wrapped height for its current width (`hasHeightForWidth` False); optional `break_long_words`: inserts U+200B after backslashes and every 8 chars in runs >= 16 chars without space/hyphen/slash; `text()`/`selectedText()`/clipboard return the original (clipboard repaired only when the cleaned text is a substring of this label's own text) | `wrapped_label.py` | port |
| A02-45 | Empty state | Frame `emptyState` (dashed `slate-border-strong`, fill `slate-disabled`, radius 10), margins 20/18/20/18, spacing 4, centred `emptyStateTitle` (13 px 600) + `emptyStateHint` (12 px, hidden when empty), `add_action(button)` centred with 8 px space above | `empty_state.py` | port |
| A02-46 | Feature badge | 18 px high pill on a button's right (12 px inset): "NEW" with a 4-point sparkle, or "?"; compacts to the star alone when the button label would come within 8 px; frosted tint of `on-accent` over a button, accent tint standalone, muted when the button is `inactive`; hover brightens; click shows the explanation tooltip (rich text) instead of reaching the button; What's-this cursor | `feature_badge.py` | port |
| A02-47 | Field help | Caption that behaves like a button: after 450 ms hover (or Space/Enter) opens a card (radius 10, `surface-hover`, `border-hover`, padding 14/12, spacing 6, width 360*scale clamped to the screen, text 11-13 px per QSS) with title, wrapped description and link buttons (emit `destination_requested(str)` and close); closes when the pointer is outside caption and card (+8 px slop) every 200 ms, Esc, or card deactivation; Wayland embeds the card in the host window with square corners; accessible name "Help for <caption>" | `field_help.py` | port (embedded branch other-platform only) |
| A02-48 | Flow layout | Left-to-right wrapping layout, default spacing 6, each line's items vertically centred, `line_width()` | `flow_layout.py` | port |
| A02-49 | Button row | Buttons share a row while labels fit, else wrap into the fewest balanced rows (short row first: 3 buttons -> 1 + 2); each button floors at its label width; stretch mode spans an lcm-sized grid; `stretch=False` left-aligned chips | `button_row.py` | port |
| A02-50 | Reflow | Lead widget with actions beside it, or below once `width < lead + 8 + actions`; lead width = given px * font scale or its natural width; 8/6 px spacing; stacked alignment default right | `reflow.py` | port |
| A02-51 | Studio split | Control column (320*scale... fixed 320) beside a `Stage` (min 400x520 scaled, wants 704 wide), gap 16; stacks column over stage when `width < column + 16 + stage min`; Stage holds panes with no layout, shows one at a time | `studio.py` | port |
| A02-52 | Scrolling dialog body | Frameless vertical-only scroll area; `fit_dialog` sizes the dialog to the copy's height at its real width up to 420 px (or screen height - 160), min 96 | `scrolling_body.py` | port |
| A02-53 | No-wheel combo/spin/date | Wheel ignored unless focused; `ElidingComboBox` min width 170, elided closed text, popup widened to longest item (+ scrollbar); `NoWheelSpinBox` hides native buttons and shows a 22 px stacked chevron rail (Increase/Decrease tooltips, auto-repeat 400 ms then 80 ms, buttons disable at range ends); `NoWheelDateEdit` calendar popup | `no_wheel.py` | port |
| A02-54 | Searchable combo | Editable, NoInsert; typing hides non-matching rows in the combo's own dropdown using `fuzzy_score` and highlights the best row; first keystroke after open replaces the text; Up/Down wrap over visible rows; Enter commits highlighted row and emits `activated`/`textActivated` itself when the platform did not (Cocoa vs Win/Linux); Esc closes; Ctrl+A/C/V/X/Y/Z, Backspace, Delete, arrows, Home/End go to the editor; optional green badge (18 px, e.g. "Active") inside the field | `searchable_combo.py` | port |
| A02-55 | Segmented bar | Tray of equal-width checkable buttons with a title and an optional second line, wrapping into balanced rows (4 -> 2+2), one column with wrapped text when not even one fits; signals `currentIndexChanged`, `activated` (person's click only); object `mcpChoiceBar`/`mcpChoice`; compact variant | `segmented_bar.py` | port |
| A02-56 | Setting tiles | `TileBase` (object `settingsTile`, property `kind` toggle/field/notice, 14/12 margins, 12 px icon gap, icon 20 px, title + description `WrappedLabel`, trailing slot, indented body, `add_actions`, `add_title_badge`), `SettingTile` (whole card toggles its `SettingsSwitch`; mirror property `checked`), `FieldTile`, `InfoTile` | `setting_tile.py` | port |
| A02-57 | Settings switch | Painted toggle 46x28 * scale (fixed), track `accent` when on and enabled else `slate-border-strong`, knob `on-accent`, 0.45 opacity disabled, focus ring in accent | `settings_switch.py` | port |
| A02-58 | Nav rail | Left rail (width 258, min 220) of chapters (heading rows) and pages (name + elided current value); selection pill via `selected`/`active` properties; guide line for nested pages; chapters open/close independently; click on current open chapter folds it; arrows/PageUp/PageDown (5)/Home/End walk pages across chapters; signal `destination_changed(key)` | `nav_rail.py` | port |
| A02-59 | Main tab container | Pill tab bar (Quick Record, Upload File, Walkthrough, Meeting Mode) over a stack; persists last tab 250 ms after change as legacy ids (Quick 0, Upload 1, Meeting 2, Walkthrough 3); Meeting tab greyed (`unsupportedMeeting`) and gated by a one-time acknowledgement dialog on unsupported OSes; during recording all other tabs are disabled | `tabbed_content.py` | port (tab set adapts to ported features) |
| A02-60 | Context menu style | Roomy translucent right-click menu (rgba surface .95, radius 10, items 8x28/14, selected = accent) used by history and Past Meetings | `context_menu.py` | port |
| A02-61 | Desktop header | Compact chrome for compositor-owned windows: brand "OpenWhisper", "OMARCHY" badge (hidden below 520 px or when not Omarchy), non-native menu bar, trailing notice slot | `desktop_header.py` | other-platform only |
| A02-62 | Update notice button | Pill in the title bar, hidden until a newer release is known: "Update to <version>" (or "Update" when compact), download icon, tooltip "OpenWhisper <v> is available. Click to see what's new." | `update_notice.py` | undecided (depends on whether OpenWhisper_CPP self-updates) |
| A02-63 | Menus, tooltips, scrollbars, tab bars, inputs | Global QSS for QPushButton, QComboBox, QLineEdit, QSpinBox, QSlider, QTextEdit, QScrollBar (12 px, no arrows), QTabBar, QMenuBar, QMenu, QCheckBox (15 px indicator), QToolTip (12 px, radius 8) | `theme.qss:1-260,294-306,1625-1840,2104-2170,2398-2408` | port |
| A02-64 | Settings slate family styling | `slate-*` token rules for Settings, Downloads, MCP, Backup, hotkeys, cleanup profiles, tiles (Settings pages are another area's widgets) | `theme.qss:308-2100, 4039-4570` | port |
| A02-65 | Feature-owned QSS sections | Meeting Mode (90 object-name roots), Upload File (38), remote engine (36), hooks (29), actions (14), stats (28), dictionary/snippets/hotkeys, scratchpad (10), update dialog, walkthrough ready card (`walkthrough_ready.py`) | `theme.qss:2410-4035, 4300-6119` | port (those features' own areas decide); Remote/Meeting/Hooks sections undecided/other |

## Modules

### Stylesheet and palette pipeline

Pipeline (what Qt finally receives): `theme.qss` template -> `_absolutize_urls` -> `Palette.resolve` (substitute `@tokens`) -> `desktop_stylesheet` (Omarchy rewrite, no-op otherwise) -> `scale_qss_fonts`. Widget-local sheets run the same three steps (`font_scale.resolve_stylesheet`, `font_scale.py:66-72`).

**`ui_qt/utils/palette.py`** (326)
- Responsibility: semantic colour tokens, two palettes, current-palette global.
- Surface: `DARK`, `LIGHT`, `Palette(name, tokens)` (`css`, `color`, `resolve`, `is_dark`), `DARK_PALETTE`, `LIGHT_PALETTE`, `PALETTES`, `current_palette()`, `set_current_palette()`, `palette_for()`, `resolve_tokens()`, `token_color()`.
- `Palette` is a frozen dataclass; equality is name + token mapping (so Omarchy palettes compare by value; `ThemeManager.set_theme` relies on it, `theme_manager.py:94`).
- Token families (dark values): neutral surfaces `bg surface surface-hover surface-active surface-disabled surface-sunken`; markdown chips `code-on-surface quote-on-surface code-on-bg quote-on-bg`; borders `border border-subtle border-strong border-hover`; text `text text-heading text-soft text-body text-secondary text-secondary-strong text-tertiary text-muted text-faint`; `knob knob-border`; `surface-rgb surface-hover-rgb`; `icon-suffix`; accent family `accent accent-hover accent-pressed accent-soft accent-border accent-cyan accent-rgb accent-cyan-rgb accent-tint accent-tint-strong accent-tint-border accent-tint-border-strong accent-muted on-accent on-accent-rgb on-tint-rgb`; status `success*`, `danger*`, `warning*` (incl. `-text`, `-text-soft`, `-text-strong`, `-tint`, `-tint-border`, `-rgb`), `purple purple-text purple-rgb`; `overlay-rgb inset inset-soft shadow-rgb`; `overlay-bg overlay-border overlay-text splash-bg splash-border`; 22 `slate-*` roles (`slate-bg slate-rail slate-rail-hover slate-surface slate-surface-hover slate-field slate-raised slate-disabled slate-border* slate-text slate-text-2/3/4 slate-text-disabled slate-text-3-rgb slate-check-disabled`).
- `slate-*` values equal the matching neutral greys except `slate-rail` (one shade darker: #161618 dark, #e9e9ee light).
- Value formats: hex, `rgba(r, g, b, a)` (`inset*`, `overlay-bg`, `overlay-border`) and "r, g, b" triples. `Palette.color` handles all three (`palette.py:265-284`).
- Threading: none; the global is set from the GUI thread.

**`ui_qt/utils/theme_manager.py`** (182)
- Responsibility: owns template + palette; builds the QPalette; pins the platform colour scheme.
- Surface: `ThemeManager(theme=DARK)`, `.current_theme`, `.palette`, `.stylesheet`, `.scaled_stylesheet(scale)`, `.set_theme(name, desktop_palette=False) -> bool`, `.apply_platform_palette(app, pin_color_scheme=True)`, signal `theme_changed(str)`; module `build_qpalette(palette)`, `_absolutize_urls`.
- Constructing a `ThemeManager` immediately calls `set_current_palette` (side effect; tests rely on it).
- Edge cases to keep: missing qss logs an error and leaves template "" (`theme_manager.py:60-62`); any exception while reading is logged and swallowed (`:68-69`); url rewrite uses forward slashes because backslashes are escapes (`:177-180`).

**`ui_qt/utils/font_scale.py`** (250)
- Responsibility: scale + theme application for the whole app; the widget-local stylesheet resolver.
- Module state: `_current_percent` (100), `_theme_preference` ("dark"), `_theme_manager`, `_resolving` guard.
- Surface: `current_ui_font_scale()`, `current_ui_font_scale_percent()`, `scale_qss_fonts`, `resolve_stylesheet`, `apply_ui_font_scale(percent, app, theme_manager)`, `current_ui_theme_preference()`, `resolve_theme_preference`, `apply_ui_theme(preference, app, theme_manager) -> changed`, `WidgetStyleFilter`.
- `apply_ui_theme` order matters (`font_scale.py:156-185`): remember preference; for "system" unset colour scheme; `desktop_palette = preference == omarchy or (system and omarchy UI)`; `manager.set_theme`; `apply_platform_palette(pin = not system)`; only if the palette changed `apply_ui_font_scale` re-applies sheet + font + every widget's sheet.
- `apply_ui_font_scale` order: store percent, `app.setStyleSheet(manager.scaled_stylesheet(scale))`, `setFont`, then `_resolve_widget_stylesheets` over `app.allWidgets()`.
- Hazard: `Palette.resolve` raises KeyError on an unknown token; `WidgetStyleFilter` calls it on any StyleChange, so a bad `@token` in any widget sheet throws from an event filter. The port should guard (log + keep the sheet).

**`ui_qt/utils/omarchy_theme.py`** (180) and **`ui_qt/utils/desktop.py`** (104): Linux tiling-desktop adaptations; see Platform-specific code.

**`ui_qt/utils/restyle.py`** (29): see feature table; C++: `style()->unpolish(w); style()->polish(w); w->update();`.

**`ui_qt/utils/tooltip_filter.py`** (56): `SnappyTooltipStyle(QProxyStyle)` wraps `QStyleFactory.create(app.style().objectName())` (`ui_qt/app.py:158-163`); keep a strong reference (a destroyed proxy crashes at exit). `RoundedTooltipFilter` checks `obj.inherits("QTipLabel")` on Polish. Construction order in app.py: ThemeManager, tooltip filter, snappy style, widget style filter, then `set_theme`, `apply_font_scale`.

**`ui_qt/utils/app_icon.py`** (145), **`icons.py`** (41): see features A02-20, A02-21. Asset paths come from `config.bundle_root()`. `functools.cache` on icons; icons are shared values (callers must not mutate).

### Pure logic utilities

**`fuzzy_match.py`** (220): see A02-25. Detail to preserve exactly:
- Split regex `[^\w]+` (Unicode), tokens lower-cased with `casefold`.
- `_score_substring` runs on the raw lower-cased target (position penalty uses raw index); `_score_normalized_substring` on space-joined tokens.
- Multi-token needs >= 2 query tokens and consumes target tokens uniquely (`taken` list), exact pass before prefix before infix (infix only for query tokens >= 3 chars).
- Subsequence takes leftmost positions; consecutive run bonus = `(run-1)*20` accumulated per adjacent pair; boundary chars are `" -_:./"`.
- Returns `0` for empty query (even against empty target) and `None` for no match; `fuzzy_match` is `score is not None`.

**`overlay_position.py`** (114), **`loudness.py`** (23), **`list_reconcile.py`** (45), **`release_notes.py`** (252), **`markdown_render.py`** (298): see features. Markdown notes: `QTextDocument::setMarkdown` with `MarkdownDialectGitHub`; styling merges formats instead of replacing; a use-after-free note in the test says a `QTextBlock` only borrows its document (C++ ownership is the same constraint); `_merge_over_block` deliberately avoids `BlockUnderCursor` (restyles previous block's last char).

**`engine_selection.py`** (53), **`agent_lookup.py`** (29): relays; connection is by signal; "lives for the session, so the settings watch never holds a view". `settings_manager.watch(keys, cb)` may fire on any thread (tests: `test_engine_selection_sync.py::TestWatch`).

**`file_reveal.py`** (67), **`drag_move.py`** (62), **`collapse_animation.py`** (36), **`macos_dock.py`** (77): see features.

**`ui_qt/utils/__init__.py`** (10): re-exports `ThemeManager`, `fuzzy_match`, `fuzzy_score`, `render_release_notes_html`.

### Widgets

All sizes in px at 100%; QSS font sizes come from `theme.qss`.

**`widgets/__init__.py`** (57): lazy export table (`__getattr__` + `import_module`) so importing `ui_qt.widgets` does not import every widget. 45 names. Pure Python startup optimisation; irrelevant in C++.

**`widgets/buttons.py`** (419)
- Classes: `HotkeyHoverHint(QWidget)`, `HotkeyHintFilter(QObject)`, `Button(QPushButton)`, `PrimaryButton`, `DangerButton`, `SuccessButton`, `WarningButton`; functions `show_hotkey_hint`, `hide_hotkey_hint`, `fit_compact_button`, `neutral_button`, `compact_primary_button`, `compact_danger_button`, `fit_start_width`; constant `START_BUTTON_WIDTH = 340`.
- `Button`: min height 44, font `QFont("Segoe UI", 12)` (the stylesheet overrides to the QSS size), pointing-hand cursor, StrongFocus, horizontal policy Minimum, `_base_min_width` 140 / `_base_min_height` 44, owns a `HotkeyHintFilter`. `setText` -> `_refresh_size`: `ensurePolished()`, width = max(text advance + 40, sizeHint().width), `setMinimumWidth(max(base, fitted))`.
- Subclasses set object names `primaryButton`/`dangerButton`/`successButton`/`warningButton` (the QSS keys on them), min 48 x 140. Comment at `buttons.py:371-373`: identify an instance with `setProperty("buttonId", ...)`, never `setObjectName`, or it loses its colours (QSS `[buttonId="meetingStart"]` exists).
- `set_active(bool)` (`buttons.py:230-247`): publishes `inactive` property via `set_style_property`, cursor, hides the hint when inactive; `mousePress/Release` and Space/Return/Enter key presses are ignored while inactive.
- Dynamic properties the QSS keys on: `inactive`, `tone` (neutral/primary/danger), `buttonId`.
- Hint: `HotkeyHoverHint` flags `ToolTip | FramelessWindowHint | NoDropShadowWindowHint`, attributes translucent, show-without-activating, transparent-for-mouse, object `hotkeyHoverHint`, layout margins (12,6,12,6), label stylesheet font 11 px 600 letter-spacing 0.5 px "Segoe UI","Helvetica Neue",sans-serif; fade `windowOpacity` 0 -> 1, 140 ms OutCubic, skipped when `compositor_managed()`; positioned `top_center.x - w/2, top - h - 8`. `HotkeyHintFilter`: Enter -> show, Leave -> 80 ms timer to hide, Hide -> hide now; `omarchyGlobalHotkeys` application property selects the compositor-mode wording.
- Tooltips/strings: `"<hotkey> · desktop shortcut"`, `"<hotkey> · while OpenWhisper is focused"` (brand string).

**`widgets/cards.py`** (208)
- `Card(QWidget)` object `card`; `ControlPanel` object `controlPanel`; `HeaderCard(Card)` with `collapsible` mode.
- Layout tree (`HeaderCard`): `QVBoxLayout(margins 16, spacing 12)` -> [`QHBoxLayout header_layout` (spacing 8): stretch, title `QLabel#headerLabel` (Segoe UI 14 bold, centred) or `CollapsibleSectionToggle`, stretch], spacing 12, then body (or `content_widget` with `content_layout` spacing 12 when collapsible).
- Collapsible state machine (`cards.py:125-208`): `set_collapsed(collapsed, emit)` stores `_content_height = max(height, sizeHint().height)` when collapsing; with `emit` it emits `toggled(bool)` and runs the animation (start = current height or natural; end 0 or natural; on finish restore max height, hide body if collapsed, then `_apply_card_size_policy`: collapsed -> max height = layout sizeHint height, expanded -> unlimited); without `emit` it applies immediately. Early return when not collapsible or unchanged.
- The default collapsible tooltips are transcription-specific strings ("Show transcription output" / "Hide transcription output").

**`widgets/collapsible_header.py`** (83): `CollapsibleSectionToggle(QToolButton)`; checkable, `ToolButtonTextOnly`, own stylesheet `TOGGLE_STYLE` (11 px 600; colour token `text-secondary`, hover `text`), `toggled_expanded(bool)`. `_on_toggled` ignores a toggle that matches current state (guards recursion with `set_expanded`).

**`widgets/animated_progress_bar.py`** (162): see A02-40. Fixed height = `bar_height` (default 8), `Expanding x Fixed`, transparent for mouse events. Timer started in `showEvent`, stopped in `hideEvent`. Constants `_TICK_MS=16`, `_SWEEP_MS=1500`, `_SHEEN_MS=1900`, `_EASE=0.18`, sheen RGBA (255,255,255,70). Easing snap threshold |delta| < 0.001. Painted with antialiasing, clip to the rounded track.

**`widgets/decode_label.py`** (140): see A02-41. `_tick` computes `locked = round(total*progress)` (Python banker's rounding; negligible). `text()` returns the final HTML (not the plain text); `set_segments(segments, animate)`.

**`widgets/eliding_label.py`** (97), **`downloads_label.py`** (60), **`wrapped_label.py`** (110): see features. Elide uses `QFontMetricsF` for hint but integer `fontMetrics().elidedText` for display (`eliding_label.py:67-98`).

**`widgets/empty_state.py`** (47): object names `emptyState`, `emptyStateTitle`, `emptyStateHint`; `text()` joins title and hint for tests/accessibility.

**`widgets/feature_badge.py`** (188)
- Constants: `_HEIGHT 18`, `_RIGHT_INSET 12`, `_PAD 7`, `_SPARKLE 10`, `_GAP 4`, `_CLEARANCE 8`; word font 9 px bold, letter spacing 0.8; "?" font 11 px bold; text `NEW = "New"` drawn upper-case; accessible name `"New: what this button does"` or `"what this button does"`.
- Behaviour: attaches to a button (child of the button, positioned in `eventFilter` on Resize/Paint, `DynamicPropertyChange` -> repaint for `inactive`) or stands alone in a layout with `button=None`. Tooltip is `<p style='white-space:normal'>{explanation}</p>`.
- Colours: standalone `accent` with alpha 38/70 fill and 90/140 edge (hover); on a button `on-accent` alpha 46/92 fill, 80/150 edge; inactive button `text-muted` alpha 28 fill, 70 edge. Sparkle path: four quadratic curves bending inward by `radius*0.08`.

**`widgets/field_help.py`** (252): `HelpCard(QFrame)` (object `fieldHelpCard`, radius 10) and `FieldHelp(QPushButton)`. Card stylesheet (tokens): title `fieldHelpTitle` `@text` 600; body labels `@text-secondary`; link buttons `@accent`, left-aligned, padding `4px 0 0 0`, underline on hover/focus. Layout: `QVBoxLayout` margins (14,12,14,12) spacing 6: title, wrapped body (fixed width = card width - 28, min 80), link buttons. Card width = `min(round(360*scale), max(160, screen width))`; embedded (Wayland) additionally `min(width, host.width-24)`. Position: below the caption (+4 px), clamped to screen/host with 8 px margin, flips above when it would pass the screen bottom. Timers: 450 ms show (single shot), 200 ms dismiss poll. Windows quirk: `card.move()` is issued twice because the first can be ignored until the native window exists (`field_help.py:118-120`).

**`widgets/flow_layout.py`** (82): `QLayout` subclass; `heightForWidth` support; `sizeHint == minimumSize`; no margins.

**`widgets/button_row.py`** (109): see A02-49. Layout: `QGridLayout` margins 0; size policy `Ignored x Preferred`; relayout debounced by a 0 ms single-shot timer on LayoutRequest/FontChange/StyleChange; `_row_sizes` picks the smallest row count where every row fits `self.width()`; shape cache avoids redundant relayout. Buttons are reparented to the row at construction.

**`widgets/reflow.py`** (82): object `reflow`; grid spacing 8 horizontal / 6 vertical; `Ignored x Preferred`.

**`widgets/studio.py`** (136): constants `COLUMN_WIDTH 320`, `GAP 16`, `STAGE_MIN 400x520`, `STAGE_WIDTH 704`; `scaled(value)` = round(value * font scale). `Stage` default object name `walkthroughStage`, `StudioSplit` default `walkthroughStudio`; `place_for_width(width)` lets the window pre-measure. Stage deliberately has no layout.

**`widgets/scrolling_body.py`** (84): object `dialogScrollingBody`, content `dialogScrollingBodyContent`, spacing 12, `_SCREEN_MARGIN 160`.

**`widgets/no_wheel.py`** (253): see A02-53. `NoWheelSpinBox` structure: `QWidget#spinStepper` (child, `WA_StyledBackground`) with `QVBoxLayout` margins 0 spacing 0: `QToolButton#spinStepUp` (icon `spin-chevron-up.svg`, 14x14, tooltip "Increase"), `QFrame#spinStepperDivider` (fixed height 1), `QToolButton#spinStepDown` ("Decrease"); geometry `(w-22-1, 1, 22, h-2)`; line-edit right text margin 22+4; size hints grow by 26. Auto-repeat 400/80 ms. Buttons NoFocus, hand cursor.

**`widgets/searchable_combo.py`** (362): see A02-54. Badge label object `searchableComboBadge` with its own stylesheet (`success-text-strong` on `rgba(success-rgb, 0.16)`, border `rgba(success-rgb, .45)`, radius 8, padding 0 7, 10 px bold); `BADGE_HEIGHT 18`, `BADGE_GAP 8`; badge placed at the right edge of the editor sub-control rect. Key routing needs event filters on line edit, popup container and the list view (installed first time the popup shows).

**`widgets/segmented_bar.py`** (235): `SegmentButton(QPushButton)` object `mcpChoice` with child labels `mcpChoiceTitle` / `mcpChoiceDetail` (property `selected` set on the first label from `toggled`); margins (14,8,14,8) or compact (12,4,12,4), spacing 2, floor height 36/28; `SegmentedBar(QWidget)` object `mcpChoiceBar`, grid margins/spacing 4 (compact 3), `QButtonGroup`; reflow: `cell = max(natural widths)`, `fits = max(1, min(n, room // (cell + gap)))`, `wrap = cell + gap > room`, balanced columns `ceil(n / ceil(n / fits))`.

**`widgets/setting_tile.py`** (228): layout tree: `QFrame#settingsTile` -> `QVBoxLayout` (14,12,14,12; spacing 10) -> [`QHBoxLayout _row` (spacing 12): `QLabel#settingsTileIcon` fixed 20x20 (top-aligned) | `QWidget#settingsTileText` (`QVBoxLayout` spacing 3: `WrappedLabel#settingsTileTitle`, `WrappedLabel#settingsTileDescription`; stretch 1, top-aligned) | trailing widgets], then `QWidget#settingsTileBody` (hidden until content; margins left 32 = icon + 12). Properties: `kind` (toggle/field/notice), `checked`. `SettingTile` toggles on left-press when enabled; cursor follows EnabledChange. `add_title_badge(text, object_name="meetingBetaBadge")`.

**`widgets/settings_switch.py`** (52): object `settingsSwitch`; size hint scales with the font scale.

**`widgets/nav_rail.py`** (406)
- Layout tree: `QListWidget#modelManagerRail` (NoFrame, no h-scroll, per-pixel vertical scroll, min width 220, `RAIL_WIDTH 258`, spacing 0). Chapter item: `QListWidgetItem` (enabled, not selectable, role `UserRole+1` = title) with `_ChapterRow` (`QWidget#settingsRailChapter`, margins (10,9,10,9), spacing 9: icon 18x18 `modelManagerRailIcon`, name `settingsRailChapterName` (stretch), chevron 12x12 `settingsRailChevron`). Page item (role `UserRole` = key) with `_RailItem` (`QWidget#modelManagerRailItem`, property `nested`): top-level margins (10,8,10,8) spacing 9; nested margins (18,0,10,0) spacing 17 with 2 px `QFrame#settingsRailGuide`; copy column spacing 2 (nested margins 7 top/bottom): `modelManagerRailName`, `modelManagerRailValue` (hidden until it has text).
- Dynamic properties: `selected` (item, name, value, guide), `active` (chapter row and name), `nested`.
- Value elision width = item width (clamped to parent) - 47, fallback 178. Chevron is `chevron-down-slate{icon-suffix}.svg` at 12 px, rotated -90 deg when closed, redrawn on StyleChange (theme switch).
- Behaviours pinned by tests (`test_nav_rail.py`): rows fit beside the scroll bar and follow a narrower rail; scrolling keeps labels over their click targets (`updateGeometries`/`scrollContentsBy` overrides call `updateEditorGeometries`, with zero deltas passed to the base); opening a chapter leaves others open; a chapter heading is never current; Up/Down/PageUp/PageDown/Home/End walk pages and open target chapters (via `moveCursor`); a first value grows a closed chapter's row height (layout invalidate/activate then `setSizeHint`). Drag cannot select headings (`mouseMoveEvent` swallows).

**`widgets/tabbed_content.py`** (329)
- Layout tree: `QVBoxLayout` (0,0) -> [`QWidget tab_container` (margins 24,16,24,8; centred) -> `QTabBar#contentTabBar` (font Segoe UI 13 DemiBold, `setDrawBase(False)`, expanding, no scroll buttons, tabs "Quick Record", "Upload File", "Walkthrough", "Meeting Mode")] + `QStackedWidget#contentStack` (stretch 1).
- Local stylesheet on the tab bar duplicates the theme's tab rules (12/12 padding, 14 px 600, `min-width: 104px`, selected accent + 2 px bottom border, `@text-faint` for disabled, `[unsupportedMeeting="true"]::tab:last` greyed but selected = accent).
- Constants: `TAB_QUICK_RECORD..TAB_MEETING_MODE = 0..3`; `SAVED_TAB_IDS` maps tab -> persisted id {0:0, 1:1, 3:2, 2:3} (legacy order Quick, Upload, Meeting, Walkthrough) read/written through `SettingsKey.LAST_TAB_INDEX`; save debounced 250 ms (`_tab_save_timer`), flush on exit; `_programmatic_tab_change` suppresses the unsupported-platform gate.
- Meeting tab gate: locked when platform unsupported and not acknowledged; a click (even double-click) while locked opens `acknowledge_unsupported_meeting_mode(window)`; selection falls back to the last allowed tab; strings in `_apply_meeting_tab_tooltip` (`tabbed_content.py:155-184`), e.g. "Meeting Mode is in beta. Transcripts and insights may be inaccurate."
- `set_recording_state(is_recording, source_tab)`: re-enable and select the source tab first, then disable every other tab (disabling the selected tab lets Qt pick a neighbour, which could visit Meeting Mode).

**`widgets/context_menu.py`** (40): `context_menu(parent)` returns a `QMenu` with the roomy stylesheet; QMenu translucency is not set here.

**`widgets/desktop_header.py`** (58): `QFrame#desktopHeader` margins (12,4,12,4) spacing 10: brand `desktopBrand` ("OpenWhisper", 13 px 600), badge `desktopBadge` ("OMARCHY", 10 px, accent, 1 px `accent-muted` border), stretch, `QMenuBar` (non-native), trailing notices.

**`widgets/update_notice.py`** (61): object `updateNoticeButton`, radius 11, padding `2px 10px 2px 8px`, margin `0 8px`, 12 px 600, icon `download-blue.svg` 14 px, hidden until `show_version(version)`; accessible name `"Update to OpenWhisper <v>"`.

### Added by check

- QSS selector census of `theme.qss` (all consumed through the stock Qt engine, so copying the file keeps them): QLabel 402, QPushButton 268, QWidget 264, QFrame 143, QComboBox 74, QDialog 62, QLineEdit 44, QToolButton 43, QListWidget 37, QCheckBox 34, QPlainTextEdit 23, QSpinBox 22, QScrollBar 19, QTextEdit 18, QScrollArea 15, QTabBar 14, QTextBrowser 8, QRadioButton 6, QDateEdit 6, plus single uses of QSplitter (`#settingsSplitter` handle), QProgressBar, QCalendarWidget (backup page), QStackedWidget, QMainWindow, QTabWidget, QSizeGrip. Sub-controls used: `::item ::indicator ::tab ::down-arrow ::handle ::drop-down ::up-button ::down-button ::sub-page ::add-page ::up-arrow ::add-line ::sub-line ::groove ::pane ::chunk ::menu-indicator`. The C++ widgets must therefore be the same Qt classes (a QTextBrowser stays a QTextBrowser, a QSplitter keeps object name `settingsSplitter`), or the selectors stop matching.
- `url()` references in `theme.qss` are exactly 9 files: `check.svg`, `tabler/{chevron-down-gray, chevron-down-slate, chevron-up-gray, microphone-slate, player-stop-danger, star-filled-amber, star-gray}@icon-suffix.svg` and `tabler/x-gray.svg`. The `@icon-suffix` token sits inside the url, so `_absolutize_urls` must run before token substitution (as in the pipeline order above) and a `.svg` suffix is built from the token. The sheet also uses `qproperty-icon: url(...)`, which needs the same path rewrite.
- `DEFAULT_MAX_HEIGHT = 420` (ScrollingBody) and `TWO_COLUMNS_FROM = 640` / `ChipRow`, `ActionCard` live in `widgets/` but belong to other areas (actions); `action_copy.py` strings are not part of this area.
- Correction note: the app-font point size uses `int(round(10 * scale))` (Python banker's rounding, 9/10/12(11.5)/13 pt for 90/100/115/130%); with `std::nearbyint` this is 9, 10, 12, 13, matching the font-size rule in A02-09.

## Data and formats

Settings keys (defined in `services/settings.py`, another area; read here):
| Key | Type | Default | Where read |
|---|---|---|---|
| `ui_theme` (`SettingsKey.UI_THEME`) | choice dark/light/system/omarchy | "dark" (`config.UI_THEME`); on an Omarchy machine with no saved value resolves to "omarchy" (`settings.py:1530`) | `ui_qt/app.py` via `resolve_ui_theme`; Settings combo `ui_theme_combo` |
| `ui_font_scale` | int choice 90/100/115/130 | 100 | `resolve_ui_font_scale`, `ui_qt/app.py`, `ui_controller.py:592-594` |
| `last_tab_index` | raw int (legacy ids Quick 0, Upload 1, Meeting 2, Walkthrough 3) | unset | `tabbed_content.py:218-236,282` |
| Meeting platform acks (`resolve_meeting_unsupported_platform_ack`, `resolve_meeting_linux_preview_ack`) | bool | False | `tabbed_content.py:140-148` |
| Engine-field keys (A02-32): `SELECTED_MODEL, WHISPER_MODEL, WHISPER_DEVICE, WHISPER_COMPUTE_TYPE, CUSTOM_WHISPER_MODELS, LOCAL_ASR_MODELS, LOCAL_ASR_DEVICES, LOCAL_ASR_LANGUAGE, API_TRANSCRIPTION_ENDPOINT, API_TRANSCRIPTION_SERVERS, API_TRANSCRIPTION_MODEL, DICTATION_LANGUAGES, DICTATION_ACTIVE_LANGUAGE` | various | n/a | `engine_selection.py:16-31` |

Dynamic widget properties used as data between widgets and the stylesheet (names are an interface to the QSS): `inactive`, `tone` (neutral/primary/danger; label tones error/warn/ok/success/paused/muted/live/up/voice/warning belong to other areas), `selected`, `active`, `nested`, `kind`, `checked`, `unsupportedMeeting`, `buttonId`, `headed`, `state`, `stepState`, `searchHit`, `capturing`, `hasText`, `dot`, `available`, `framed`, `group`, `backupCard`, `iconTone`, `off`, `recording`. Further QSS-keyed property names found by scanning `theme.qss` that the list above lacked: `finalizationTone`, `meetingLink`, `meetingResultAction`, `muted`, `nearCap`, `overlay`, `scratchpadAction`, `starred`, `stage`, `tileId`, `walkthroughAction` (about 34 distinct `[prop=...]` selectors in total). Internal-only properties: `unscaledStyleSheet`, `scaledStyleSheet` (font_scale), Application property `omarchyGlobalHotkeys` (list of hotkey strings, read by `buttons.py:170`).

Files on disk read by this area: `ui_qt/styles/theme.qss`; `ui_qt/assets/{openwhisper.ico, openwhisper.png, check.svg, spin-chevron-up.svg, spin-chevron-down.svg}`; `ui_qt/assets/tabler/*.svg` (59 files + LICENSE.txt; names follow `<glyph>-<tone>[-light].svg`; the stylesheet builds names with `@icon-suffix`); Omarchy `colors.toml` (A02-13). No database tables, no network, no wire protocols. Nothing is written to disk by this area except through `settings_manager.save_setting(LAST_TAB_INDEX)`.

Interface that other programs or existing installs depend on: settings values `ui_theme`, `ui_font_scale`, `last_tab_index` (legacy tab ids must keep their numbering); object names and property names (a Python-era `theme.qss` and C++ widgets must agree); `OPENWHISPER_UI` environment override; asset file names.

## External dependencies

| Dependency | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| PyQt6 Widgets/Gui/Core | everything | Qt 6.5+ Widgets (already the app's stack). Add `Qt6::Svg` (QSS `image: url(*.svg)` and `QIcon("x.svg")` need the SVG plugins), `Qt6::Test` for tests |
| QSS engine | theme | same engine; load the resolved text with `qApp->setStyleSheet` |
| `re` | token regex, font-size regex, markdown/release-notes parsing, fuzzy split | `QRegularExpression` (UseUnicodePropertiesOption for `\w`) or `std::regex` for ASCII-only pieces |
| `tomllib` | Omarchy `colors.toml` | `toml++` (vcpkg `tomlplusplus`) or a 20-line `key = "#rrggbb"` parser plus `mode`; only needed on Linux |
| `html.escape` | release notes, decode label | `QString::toHtmlEscaped()` (note: Qt escapes `& < > "`, Python also escapes `'` as `&#x27;`) |
| `random` | decode-label glyphs | `QRandomGenerator` |
| `functools.cache`, `copy.deepcopy`, `math.lcm`, `itertools.pairwise` | icon cache, reconcile fingerprints, ButtonRow, fuzzy | `QHash` cache, value copy of the fingerprint (QVariant/QVariantMap), `std::lcm`, index loop |
| `subprocess` | Explorer / `open -R` | `QProcess::startDetached` with a raw command line on Windows (`QProcess::setNativeArguments`), `open -R` on macOS |
| `QDesktopServices.openUrl` | open folder | same |
| `AppKit` (pyobjc) | Dock activation policy | Objective-C++ (`NSApp setActivationPolicy:`), macOS builds only |
| `config.bundle_root` | assets | `QCoreApplication::applicationDirPath()` or `qrc:` resources |
| Qt style hints `colorScheme` / `setColorScheme` | system theme follow, title bar | same API in Qt 6.8+ (older: no-op, as Python does) |

## Platform-specific code

- **Windows**: default fonts "Segoe UI" (QSS font-family `"Segoe UI", "Helvetica Neue", sans-serif`; app font Segoe UI 10 pt * scale); `explorer /select,"path"`; the dark title bar comes from `QStyleHints::setColorScheme` (Qt 6.8+); Field-help card needs the double `move()` (`field_help.py:118-120`); tooltip translucency + `NoDropShadowWindowHint`.
- **macOS**: `open -R`; Dock icon sync (`macos_dock.py`); `QApplication.setDesktopFileName` and bundle name override live in `ui_qt/app.py` (other area); `SearchableComboBox` papers over a Cocoa difference where the native activation fires even though Return is accepted (`searchable_combo.py:209-221`); button width "can undershoot by a pixel or two on macOS" (`buttons.py:260-261`); `SF Mono`/`Menlo` monospace fallbacks; Helvetica Neue.
- **Linux X11**: no special-case; tooltips as Windows; `reveal_in_file_manager` opens the containing folder (no select support).
- **Linux Wayland**: `is_wayland()` = platform name startswith "wayland" (not "XWayland present"); `compositor_managed()` = wayland or Omarchy UI: Qt tooltip replaces the pill hint (Qt anchors native tooltips to the parent surface), tooltip radius 0, field-help card embedded in the host window with square corners and no translucency, no fade-in.
- **Hyprland / Omarchy** (`use_omarchy_ui()`): Fusion style, `QT_QPA_PLATFORMTHEME=""`, no client window decorations, palette from Omarchy colours, all radii 2 px, font Noto Sans/DejaVu Sans, `DesktopWindowFilter` (window buttons stripped, fit to tile size, scroll area for oversized dialogs), `DesktopHeader` with menus because the compositor owns move/size, theme file poll every 2 s. Tests: `test_omarchy_ui.py` (21 tests incl. tiled geometry, Wayland, tray restore), `test_desktop_dialog_fit.py`.

## Engine-specific code

- Drop: none of the foundation is engine-code. `engine_selection.ENGINE_KEYS` lists settings for engines that will not be ported: `WHISPER_MODEL`, `WHISPER_DEVICE`, `WHISPER_COMPUTE_TYPE`, `CUSTOM_WHISPER_MODELS` (faster-whisper/CTranslate2), `LOCAL_ASR_MODELS`, `LOCAL_ASR_DEVICES`, `LOCAL_ASR_LANGUAGE` (Moonshine/Qwen3/Parakeet MLX/Unified local-ASR registry). Replace with the Parakeet keys (model path, runtime build CPU/GPU, device, language) plus whatever Nemotron adds.
- Undecided: `API_TRANSCRIPTION_ENDPOINT`, `API_TRANSCRIPTION_SERVERS`, `API_TRANSCRIPTION_MODEL` (cloud/own-server transcription), and `LocalEngineControls`/engine field widgets that listen to the relay (other areas).
- Adapt: the relay must tell the Quick Record/Upload/Settings engine fields when the saved Parakeet model/device/language changes. Settings watchers in OpenWhisper_CPP run on the GUI thread already (EngineWorker is the only other thread), so the thread-hop in `_EngineSelectionRelay` can be a plain Qt `Q_EMIT` guarded by a queued connection if saves can come from the worker.
- `agent_lookup` belongs to the text-rewrite/agent area, not the speech engine: port with that area.
- Branding strings that say OpenWhisper in this area: hotkey hint scope text, DesktopHeader brand, update notice strings, `app_icon` docstrings/`openwhisper.ico`, `OPENWHISPER_UI` env var, `QApplication.setDesktopFileName("openwhisper")`. Deliberate difference to decide: rename to OpenWhisper_CPP (suggested, with the env var accepted under both names if desired).

## Dependencies on other areas

Calls out:
- `services.settings`: `SettingsKey`, `UiTheme`, `UiFontScale`, `settings_manager` (`load_all_settings`, `save_setting`, `watch`), `resolve_ui_theme`, `resolve_ui_font_scale`, meeting acknowledgement resolvers.
- `services.desktop_session`: `is_wayland_session`, `use_omarchy_ui`, `omarchy_theme_paths`.
- `services.installed_agents.add_lookup_listener` (agent_lookup).
- `meeting.platform`: `meeting_mode_supported`, `linux_meeting_implementation_ready`, `meeting_unsupported_os_name` (tabbed_content).
- `ui_qt.dialogs.meeting_unsupported_dialog.acknowledge_unsupported_meeting_mode` (tabbed_content, lazy import).
- `config.bundle_root` (asset root), `config.UI_THEME` / `UI_FONT_SCALE` defaults.

Called by (non-exhaustive, from grep):
- `ui_qt/app.py` (QtApplication: `app_icon`, `ThemeManager`, `RoundedTooltipFilter`, `SnappyTooltipStyle`, `WidgetStyleFilter`, `DesktopWindowFilter`, `apply_ui_theme`, `apply_ui_font_scale`, Omarchy poll timer, `colorSchemeChanged`).
- `ui_qt/ui_controller.py` (live theme/scale changes `:592-594,1755-1760`; Dock sync `:271-273`).
- Settings dialog and every settings page (`ElidingComboBox` x 40+, `SettingTile`, `FieldTile`, `InfoTile`, `NavRail`, `SegmentedBar`, `SettingsSwitch`, `EmptyState`, `WrappedLabel`, `FieldHelp`, `lookup_relay`, `engine_selection_relay`, `on_screen_position`).
- `transcription_tab_base.py` / `transcript_viewer_dialog.py` (`render_markdown`, `PREVIEW_STYLE`, `READER_STYLE.scaled(font_scale * zoom)`).
- `app_update_dialog.py` (`render_release_notes_html`).
- `ui_qt/overlays/live_recording.py`, `rule_activity.py` (`level_to_height`).
- `history_sidebar.py`, `past_meetings_panel.py` (`context_menu`, `reconcile_cards`, `HistoryDelivery`, `MeetingDelivery`).
- Overlays (waveform HUD, hotkey hint, snap overlay) use `overlay_position` and `DragToMove`.
- `history_sidebar.py`, `past_meetings_panel.py` use `reveal_in_file_manager`.
- Many dialogs call `app_icon()`.

## Tests

| Test file | What it pins | Logic-only / golden-vector candidate |
|---|---|---|
| `tests/test_fuzzy_match.py` (86 lines, 11 tests) | empty query/target, case folding, delimiter vs space, multi-token out-of-order, stripped, subsequence, regex-special chars, ranking order | Yes: generate `(query, target) -> score` table from Python |
| `tests/test_searchable_combo.py` (238) | filtering, replace-not-append, focus stays on editor, Enter activates best fuzzy row, badge | GUI test; port to QtTest |
| `tests/test_release_notes.py` (78, 12 tests) | lists, headings, inline markup, link target, checksum/changelog drop, escaping, paragraph join | Yes: markdown in -> HTML out (token-resolved colours must be fixed to dark palette) |
| `tests/test_markdown_render.py` (180) | heading sizes, chip colours, palette follow, fenced slab, quotes, tables, plain-text round trip, `MarkdownStyle.scaled`, `heading_pt` clamp | Partly (style math); doc formatting needs QtGui |
| `tests/test_overlay_position.py` (95) | clamp, flip, secondary monitor origin, max height | Yes: rectangles in -> (x, y) out |
| `tests/test_ui_font_scale.py` (90) | `scale_qss_fonts` (integer px, pt, decimal, identity returns same string), widget sheets rewritten from the original (no compounding), theme sheet scales all 14 px | Yes for `scale_qss_fonts`; rest QtWidgets |
| `tests/test_ui_theme.py` (174) | both palettes same keys, `resolve` semantics, KeyError on unknown token, `color()` formats, no unresolved `@` in either theme, themes differ only in colours and `-light.svg`, light chevron files exist, `apply_ui_theme` switching, `theme_changed` emits once, preference resolution (garbage->dark), settings combos persist | Palette tests: yes |
| `tests/test_omarchy_ui.py` (407) | Omarchy detection/override, both theme locations incl. atomic replacement, CSS injection falls back to dark, monochrome palettes keep every role, in-mode theme changes, tiled geometry, Wayland behaviour | `palette_from_colors` vectors: yes (Linux only) |
| `tests/test_nav_rail.py` (269) | rail geometry, chapter behaviour, keyboard walk | QtTest |
| `tests/test_eliding_label.py` (123), `test_eliding_combo.py` (32), `test_no_wheel_spinbox.py` (104) | elide math, hints, popup width, stepper behaviour and chevron-image (not border) stylesheet | QtTest |
| `tests/test_field_help.py` (96) | link emits destination and closes; card is a window anchored to the caption; Esc dismisses | QtTest |
| `tests/test_drag_to_move.py` (235) | overlay drag, stays on screen, snap, walkthrough parking | mixed (overlay area) |
| `tests/test_update_notice.py` (164), `test_inactive_buttons.py` (64), `test_settings_polish.py` (338: switches, compact buttons by property, SegmentedBar, WrappedLabel break points and clipboard repair, ButtonRow wrapping), `test_macos_dock.py` (89), `test_desktop_dialog_fit.py` (60), `test_engine_selection_sync.py` (224: watch semantics and tabs following the saved engine), `test_recording_look.py` (583; uses palette/loudness), `test_agent_mcp_theme.py` (283; effective theme reporting via MCP) | behaviours as named | `level_to_height` vectors: yes |
| `tests/conftest.py` `isolated_app_theme` fixture | The test app runs unstyled; any test that themes/rescales must restore `_current_percent`, theme manager, preference, palette | fixture pattern to copy |

Golden-vector candidates (pure, deterministic, Python-generated): `fuzzy_score(query, target)`; `scale_qss_fonts(sheet, scale)` (watch banker's rounding: e.g. 10px at 1.15); `Palette.resolve/color`; `palette_from_colors(colors, mode)` token map; `render_release_notes_html(notes)` (with dark palette); `level_to_height`; `clamp_rect_to_available` / `preferred_overlay_position` / `max_height_for_anchor` / `on_screen_position`; `MarkdownStyle.heading_pt/scaled`; `_absolutize_urls`; `desktop_stylesheet`; `_with_break_points` (WrappedLabel); `reconcile_cards` order logic (with fake layout); ButtonRow `_row_sizes`; SegmentedBar `_reflow` column math; `FlowLayout._arrange`.

## Porting notes

**Decomposition**
- Qt-free core (new `src/engine`-style `src/ui_core` or a header-only layer; no QtWidgets, only QtCore/QtGui types if desired, or plain C++20): `palette` (token maps + `resolve` + color parse into `{r,g,b,a}`), `qss_pipeline` (url absolutize, token resolve, font-scale rewrite, Omarchy rewrite), `fuzzy_match`, `overlay_position`, `loudness`, `release_notes` (output rich-text string, takes a resolved style table), `omarchy_palette`, `button_row`/`segmented_bar`/`flow_layout` size algorithms (pure functions), `list_reconcile` ordering. Easy to unit-test with a compiler alone and to replay Python golden vectors.
- Qt layer (`src/app/ui`): `ThemeManager`, `build_qpalette`, `FontScale` (widget-local sheet filter, app font), tooltip proxy style + filter, `app_icon`/`icons`, markdown styling (needs QTextDocument), all widgets, `file_reveal`, `drag_to_move`, relays.
- Resources: ship `theme.qss` and `assets/` through `qt_add_resources` and rewrite `url(ui_qt/assets/...)` to `:/assets/...` at load (QSS accepts `url(:/path)` and `image` SVG via the Svg plugin; `qproperty-icon: url(...)` also works). Keep the same file names so the QSS can be diffed against the Python original.

**Order of work (suggested)**
1. Palette + QSS pipeline + ThemeManager + font scale + app icon + tooltip filter; wire theme/size/colour-scheme into the existing one-window app; smoke test with the real `theme.qss` copied over unchanged. This unlocks "same look" for everything else.
2. Pure utilities with golden vectors (fuzzy, overlay positions, loudness, release notes, font scale, Markdown style).
3. Leaf widgets: buttons (+hotkey hint), cards/collapsible, switch, tiles, wrapped/eliding labels, empty state, no-wheel/eliding combo/spin, searchable combo, progress bar, decode label, context menu.
4. Layout helpers: ButtonRow, Reflow, FlowLayout, SegmentedBar, Studio, ScrollingBody.
5. Composite: NavRail, FieldHelp, FeatureBadge, TabbedContent, UpdateNotice.
6. Platform extras (macOS Dock, Omarchy/Wayland, DesktopHeader) last and only if Linux/macOS targets are real.

**Risks**
- The look is carried by 636 object names and ~25 dynamic property names; widgets in other areas must set them exactly. Add a CI check that every `#name` in `theme.qss` is referenced by some C++ widget or intentionally unused (scripted grep), and a test that both themes leave no `@` unresolved (mirrors `test_ui_theme.py`).
- Python's `font-size` rounding is banker's rounding on IEEE doubles (`round`), C++ `lround` is half-away: use `std::nearbyint` with the default rounding mode to match; same for `QColor` alpha from `setAlphaF` (Python float -> int conversion in Qt is the same call).
- `casefold` vs `QString::toCaseFolded` differ for a few characters (e.g. "ß"); character indices in Python are code points, Qt's are UTF-16 units (affects position penalties only for astral characters).
- `Palette.resolve` raising inside `WidgetStyleFilter` event handler: C++ exceptions from an event filter are worse. Catch and log.
- Windows quirks to retain: double `move()` of the help card, translucent tooltip flags, `explorer /select` raw command line.
- Qt version differences: `QStyleHints::setColorScheme` needs Qt 6.8 (CLAUDE.md requires 6.5+); guard with `QT_VERSION_CHECK`. `QTextDocument::setMarkdown` GitHub dialect exact behaviour is Qt-version-sensitive (headings, tables).
- The stylesheet contains rules Qt ignores or interprets oddly (`line-height` on QTextEdit, `letter-spacing`, comments in selectors); copying verbatim keeps parity by construction.
- High-DPI: icon pixmaps requested at 12/14/16 px logical; use `devicePixelRatio` aware `QIcon::pixmap` (Python path does not, SVG is vector so fine).

**Size estimate**: ~5.5-6.5k lines of C++ (palette/pipeline/theme ~900, pure utils ~900, widgets ~3.5k, relays/platform ~400), plus `theme.qss` copied as data (6.1k lines, not code) and 64 asset files. Tests ~1.5-2k lines.

**Native improvements available**
- Startup: Python resolves the 6.1k-line sheet with regexes at every theme/size change and re-resolves every widget's local sheet by walking `allWidgets()`; in C++ the same string pass is microseconds, and the stylesheet can be pre-resolved for both themes and four scales at build time or cached by `(theme, scale)`.
- Lazy exports in `widgets/__init__` (a Python import-time optimisation) vanish.
- `QSvgRenderer` / QIcon caching replaces `functools.cache`; pre-rasterise tray icon sizes; `render_app_pixmap` can also generate the `.ico`/`.icns` at build time.
- Avoid repeated `unpolish/polish` for property changes by batching (`set_style_property` already no-ops unchanged values).
- A single typed `Theme` object (struct of QColors) could replace per-paint `token_color()` string parsing in painted widgets (progress bar, switch, badge paint every frame and re-parse tokens: `animated_progress_bar.py:116,130-133`).
- Optional functionality gains: expose the Omarchy-style palette import on Windows/macOS (custom accent colour), live preview of theme/size in Settings, follow Windows accent colour.
