# App shell: entry point, config, bootstrap, main window, tray, loading screen

Area a01-app-shell. Source root: `D:\coding\whisper_local` (OpenWhisper 2.8.92,
`_version.py:11`). 18 files, about 5,900 lines; `ui_qt/main_window.py` alone is 2,463.

## What the user gets

- A launch that shows a custom-painted splash ("OpenWhisper" with a pulsing microphone glow)
  within one event-loop pass, then the main window once controllers are built. Slow work
  (model load, consent prompts, meeting recovery) runs only after the window is visible.
- One running UI per data directory (Linux enforces it with a lock plus a "show" socket;
  Windows frozen builds use a named mutex owned by the updater area; macOS relies on the Dock).
  A second launch brings the first window forward.
- A frameless main window with its own title bar (File / View / Help menus, centered title,
  painted minimize / maximize / close glyphs, update notice), four tabs (Quick Record, Upload
  File, Walkthrough, Meeting Mode), a collapsible History side panel with an edge tab, and a
  footer with Minimize to Tray / Stats / Settings / Quit.
- The window fits its height (and, when needed, width) to the current tab's page with a 300 ms
  InOutCubic animation, remembers position and size, and has three views: full, Compact Mode
  (420x250 fixed recording controller), and Host Mode (dashboard of what this computer serves).
- A system tray icon with a menu (Show, Hide, Start/Stop Recording, Copy original of last
  dictation, Start/Finish Walkthrough, Take Screenshot, Language submenu, Start/End Meeting,
  Open Meeting Dashboard, Scratchpad, Stats, Settings, Update to X..., Exit). Closing the window
  hides it to the tray when "minimize_tray" is on (default) and a tray exists.
- A command line: `--help`, `--version`, `--diagnostics OUT.json`, `--api`, `--channel`,
  `--self-test`, `--workflow-smoke`, plus internal worker modes.
- Logs (`openwhisper.log`, rotating 5 MB x 3), a hard-crash log (`openwhisper.crash.log`),
  a privacy-safe failure journal and metrics file used by the opt-in diagnostics export.
- The central `AppConfig` constants that every other area reads (paths, defaults, timings,
  cleanup prompts, window sizes, default hotkeys).

## Feature inventory

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A01-01 | Data directory resolution | `OPENWHISPER_DATA_DIR` wins (expanded, made absolute, created). Else frozen build -> per-user dir; source run -> cwd or checkout if it holds `openwhisper_settings.json`, else checkout if it has `.git`, else per-user dir. Per-user: `%LOCALAPPDATA%\OpenWhisper`, `~/Library/Application Support/OpenWhisper`, `$XDG_DATA_HOME` or `~/.local/share/OpenWhisper`. | `config.py:data_root`, `_source_data_dir`, `local_app_dir` | port (OpenWhisper_CPP needs its own app name/env var decision) |
| A01-02 | Components root | Always `<local_app_dir>/components`, never per checkout | `config.py:components_root` | port |
| A01-03 | `.env` lookup | `<data_root>/.env` first, then `<checkout>/.env`; returns the first path even if missing | `config.py:env_file_path` | port |
| A01-04 | App constants (`AppConfig`) | ~200 constants: file names, audio (44100 Hz, 1024 frame chunks, mono int16), timings (hotkey debounce 300 ms, min hold 250 ms, latch 400 ms, post-roll 1200 ms cap, adaptive quiet 300 ms, +12 dB margin, etc.), window sizes, cleanup prompts, batch notes, meeting defaults, updater intervals | `config.py:AppConfig` | port (constants), several engine-only ones drop |
| A01-05 | Default backend choice | `parakeet_mlx` on macOS 14+ arm64; `parakeet` on Windows/Linux x86_64; else `local_whisper` | `config.py:__post_init__`, `parakeet_default_supported`, `parakeet_mlx_supported` | adapt: OpenWhisper_CPP is always Parakeet |
| A01-06 | Host CPU arch without WMI | Windows: `IsWow64Process2` native machine, then `PROCESSOR_ARCHITEW6432`, then `PROCESSOR_ARCHITECTURE` | `config.py:host_machine`, `_windows_native_machine` | port (engine `platform.cpp` may already cover it) |
| A01-07 | Default hotkeys | Win/Linux: record `kp *`, cancel `kp -`, enable/disable `ctrl+alt+kp *`, minimize `ctrl+alt+m`, paste walkthrough `ctrl+shift+f12`, rest unbound. macOS: `ctrl+alt+r`, `ctrl+alt+escape`, `ctrl+alt+shift+r`, `ctrl+alt+m`, `ctrl+shift+f12` | `config.py:__post_init__` | port |
| A01-08 | Model choice lists | Display names and value map (`Local Whisper`->`local_whisper`, `API`->`api`, `Parakeet`->`parakeet`, `Qwen3-ASR`, `Nemotron Streaming`, `Moonshine`, `Parakeet MLX`, `Parakeet Unified`, `Remote computer`->`remote`); API models `gpt-transcribe` (default), `gpt-4o-transcribe`, `gpt-4o-mini-transcribe`, `whisper-1`; Whisper sizes list | `config.py:MODEL_CHOICES`, `MODEL_VALUE_MAP`, `API_MODEL_CHOICES`, `WHISPER_MODEL_CHOICES` | Parakeet port; Whisper/Moonshine/Qwen/MLX/Unified drop; Nemotron, API, Remote undecided backend |
| A01-09 | AI cleanup prompt presets | Light / Medium (default) / High built from fixed sentences; unknown-app and terminal line rules; legacy prompt; rule-polish prompt; batch relation notes and timeouts (8 s / 1 retry dictation, 120 s / 2 retries batch) | `config.py:TRANSCRIPT_CLEANUP_*`, `TRANSCRIPT_BATCH_*` | port (text verbatim; consumed by a12) |
| A01-10 | CLI help | `-h`/`--help` prints `_USAGE` and exits 0 before Qt or CUDA load | `main.py:_USAGE`, `_handle_early_cli` | port (OpenWhisper_CPP wording) |
| A01-11 | `--version` | Prints `OpenWhisper 2.8.92`, exit 0 | `main.py:_handle_early_cli` | port |
| A01-12 | `--diagnostics OUT.json` | Writes sanitized report to a new file (`open("x")`: refuses to overwrite), exit 0 | `services/diagnostics.py:main`, `collect_report` | port |
| A01-13 | `--api`, `--channel` | Dispatch to History API server / Claude Code channel bridge before Qt loads | `main.py:_handle_early_cli` | port (owned by a19/a20) |
| A01-14 | Worker modes | `--local-asr-worker`, `--isolated-worker`, `multiprocessing.freeze_support()` | `main.py` | drop (engine-specific) |
| A01-15 | `--self-test`, `--workflow-smoke [REPORT]` | Import every module, check bundled assets (`theme.qss`, `.ico`, `.png`, `webui/dist/index.html`, SVG plugin), CA certs, then run workflow smoke; prints "OpenWhisper package self-test passed". Smoke rejects more than one extra arg with `Usage: --workflow-smoke [report.json]` | `main.py:_run_package_self_test`, `_handle_package_self_test` | port (rewrite as asset/plugin checks; a16 owns smoke) |
| A01-16 | `--macos-update-helper` | macOS-only updater helper dispatch | `main.py` | other-platform only (a16) |
| A01-17 | Pre-start update gate | Windows frozen only: exit if an update is in progress, recover, then take the app mutex | `main.py:_early_update_gate` | port with a16 (Windows) |
| A01-18 | Native library bootstrap | Register CUDA DLL dirs, Qt ICU dirs, preload CUDA libs, settle/activate downloadable components; Linux dependency preflight exits 1 on failure | `main.py:283-294` | drop CUDA/ICU parts; component activation undecided (a14) |
| A01-19 | No console windows for children | Windows: every `subprocess.Popen` gets `CREATE_NO_WINDOW` | `main.py:_patch_subprocess_for_windows` | port (QProcess / CreateProcess flag) |
| A01-20 | Null stdio in windowed builds | Replace missing stdout/stderr with write-nowhere streams | `main.py:_ensure_writable_stdio` | drop (C++ has no equivalent failure) |
| A01-21 | Startup data lease | Acquire shared data lease after any pending backup restore; on failure: stderr + (frozen) critical box "Could not safely open application data: {exc} The backup restore remains pending. Close other OpenWhisper processes and start the app again." exit 1 | `main.py:367-375`, `_show_startup_error` | port (with a10 backup) |
| A01-22 | Restart exit code | If `main()` returns `RESTART_EXIT_CODE`, release update mutex (Win frozen), spawn a new process; on failure show "Could not restart OpenWhisper: {exc} Please start OpenWhisper again manually." Process ends with `os._exit` | `main.py:382-416` | port |
| A01-23 | Launch identity | `remember_launch()` before main, to detect code changing under the app | `main.py:383-386` | port (a16) |
| A01-24 | Logging setup | Level from `OPENWHISPER_LOG_LEVEL` (default INFO); format `%(asctime)s - %(levelname)s - %(name)s - %(message)s`; rotating file 5 MiB x 3 UTF-8; console handler when stdout exists; httpx/httpcore at WARNING (URLs may carry dictated words or tokens) | `ui_qt/bootstrap.py:setup_logging`, `config.py:LOG_*` | port |
| A01-25 | Crash log | `faulthandler` to `<log>.crash.log` (append, line buffered), all threads, SIGSEGV/ABRT/FPE/ILL | `bootstrap.py:_enable_crash_logging` | port (native crash handler / minidump) |
| A01-26 | Qt message routing | qDebug/info/warning/critical/fatal -> logger "qt" with ` (file:line function)` suffix when present; installed once | `bootstrap.py:_install_qt_message_handler`, `_qt_context_suffix` | port (`qInstallMessageHandler`) |
| A01-27 | Failure journal | Every ERROR log record appends `{utc, component, category, line}` (no message text) to `diagnostic-failures.json`, max 50, atomic tmp+replace, IO errors swallowed | `services/diagnostics.py:FailureCapture` | port |
| A01-28 | Metrics capture | Allowlisted numeric metrics (12 names), max 100 samples each, finite and 0..1e12, written by one coalescing daemon thread to `diagnostic-metrics.json`; close waits at most 0.1 s | `diagnostics.py:MetricsCapture`, `record_metrics`, `METRIC_NAMES` | port |
| A01-29 | Diagnostics report | schema 1: app_version, packaged, generated_utc, platform (system, release, machine, cpu_count, python, nvidia_gpu {name,total_mib,compute_capability}), engine (allowlisted settings else `default_or_custom`), package versions, recent failures, metric summary (count, p50, p95 linear-interpolated, maximum, latest), fixed privacy sentence | `diagnostics.py:collect_report`, `_metric_summary` | port (package list becomes runtime versions) |
| A01-30 | Startup profiler | Named marks with seconds since start; summary lines `  %-32s total=%7.3fs delta=%7.3fs`; `startup_to_window_s` metric from the `main_window_shown` mark | `ui_qt/startup_profiler.py`, `bootstrap.py:main` | port |
| A01-31 | Single UI instance (Linux) | flock on `$XDG_RUNTIME_DIR/openwhisper/ui-<key>.lock` (or `/tmp/openwhisper-<uid>`), key = sha256(normcase(realpath(data_dir)))[:16]; abstract socket `\0openwhisper-ui-<uid>-<key>`; second launch sends `show\n` for up to 2.0 s (0.4 s connect timeout, 50 ms retry) then exits 0; log "OpenWhisper is already running; bringing the existing window forward" | `services/ui_instance.py` | port (QLocalServer/QLockFile, all platforms) |
| A01-32 | Show request across threads | Activation from the accept thread is queued to the Qt thread via a signal | `bootstrap.py:qt_thread_callback` | port |
| A01-33 | Startup sequence | Splash status/progress pairs: "Initializing components..." / "Preparing startup..."; "Loading application..." / "Loading runtime components..."; "Creating interface..." / "Setting up windows..."; "Initializing audio system..." / "Starting services..." | `bootstrap.py:main` | port |
| A01-34 | Background import with live splash | Heavy imports run on a worker while the UI thread spins 33 ms nested event loops | `bootstrap.py:run_with_ui_pulse` | adapt (C++ has no import cost; keep for engine/runtime load) |
| A01-35 | Post-window startup | Guided-setup init, keep-unchosen-engine and keep-separate-meeting-speech migrations before the window; after: keyring-unresponsive ambient status, apply-error check, backup setup, `notify_main_ui_ready`, guided setup offer, idle Settings build, MCP runtime configure + restore | `bootstrap.py:main` | port |
| A01-36 | Update health acknowledgement | When started with a health token: window disabled; 1500 ms after the loop starts, exit 1 on readiness error or failed ack, else re-enable | `bootstrap.py:_schedule_update_acknowledgement` | port (a16) |
| A01-37 | Orderly shutdown | Stop MCP runtime (wait), destroy splash, `app_controller.cleanup()` (else `ui_controller.cleanup()`), log "Application shutdown complete" between `=`*60 lines; startup errors still log the profile and re-raise | `bootstrap.py:_shut_down` | port |
| A01-38 | HF offline note | Logs "HF_HUB_OFFLINE set in environment — Hugging Face downloads disabled" | `bootstrap.py:_log_hf_hub_offline` | drop (engine-specific) |
| A01-39 | CUDA preload summary (Linux) | Three wordings: "Preloaded %d CUDA library/libraries: ...", "Preloaded GPU Acceleration's CUDA libraries: ...", "No NVIDIA CUDA libraries preloaded — local transcription will use CPU (install GPU Acceleration from Downloads for GPU acceleration)" | `bootstrap.py:log_cuda_preload_summary` | adapt (report which NeMo runtime, CPU or GPU, was loaded) |
| A01-40 | QApplication setup | App name / display name / organization "OpenWhisper"; Linux desktop file name `openwhisper`; macOS bundle-name rewrite; Omarchy: empty `QT_QPA_PLATFORMTHEME`, `QT_WAYLAND_DISABLE_WINDOWDECORATION=1`, Fusion style; app icon; desktop-window, rounded-tooltip and widget-style event filters; snappy tooltip proxy style; theme + font scale; follows OS color scheme when theme is "system" | `ui_qt/app.py:QtApplication` | port |
| A01-41 | Omarchy theme polling | Every 2000 ms, stat both `colors.toml` paths (inode, mtime_ns, size); restyle only when the signature changes and the preference is system/omarchy | `app.py:_refresh_desktop_theme` | other-platform only (Linux Omarchy) |
| A01-42 | Loading screen | 450x300 frameless, stay-on-top tool, translucent, centered on primary screen, draggable with the left button; rounded 16 px gradient card, pulsing glow (4.5 rad/s, 33 ms precise timer), 5 orbiting dots, mic outline; text "OpenWhisper" / status / progress. Compositor-managed: 450x180 opaque SplashScreen, no animation, left-aligned text, 3 px accent strip | `ui_qt/loading_screen.py` | port |
| A01-43 | Custom title bar | 32 px base height (grows with font scale), menu bar left, centered "OpenWhisper" label, 46 px painted glyph buttons with tooltips Minimize / Maximize<->Restore / Close; drag to move, double-click to maximize, edge drag to resize | `main_window.py:CustomTitleBar`, `WindowGlyphButton` | port |
| A01-44 | Menus | File: Settings, Guided setup…, Downloads..., Hotkeys, ---, Minimize to Tray, Exit (macOS "Quit", Ctrl+Q). View: History/Past Meetings/Walkthroughs (Ctrl+H), History Calendar (Ctrl+Shift+D), Compact Mode (Ctrl+Shift+C), Host Mode (checkable, Ctrl+Shift+H), ---, Scratchpad, Stats, ---, Open Meeting Dashboard. Help: Check for Updates..., About | `main_window.py:_setup_menu` | port |
| A01-45 | Footer | 48 px bar, buttons 34 px high: "Minimize to Tray" (or "Hide" when compositor-managed) with hotkey hint, "Stats", "Settings", "Quit" (Ctrl+Q hint), 10 px gaps, centered | `main_window.py:_build_footer` | port |
| A01-46 | Lazy tabs | Upload / Walkthrough / Meeting tabs are placeholders until first shown; queued state calls replay on creation; Host dashboard likewise | `main_window.py:_ensure_screen`, `update_screen`, `update_transcription_tabs` | port |
| A01-47 | Context side panel | Panel shows History, Past Meetings (Meeting tab) or Walkthroughs (Walkthrough tab); menu label and edge-tab name follow; refresh deferred 75 ms after a tab switch; window grows in lockstep with the panel animation | `main_window.py:_on_tab_changed`, `toggle_history`, `_on_sidebar_width_animated` | port |
| A01-48 | Fit window to page | Height = chrome + page content height for the coming width, capped at screen available height; floor = min(fit, 660); elastic pages (open transcript, storyboard) keep the user's dragged height; width widens for a page that needs it and is handed back later; animated 300 ms InOutCubic; scrollbar forced off during the animation | `main_window.py:_sync_page_size`, `_page_width`, `_animate_resize`, `_on_resize_animation_state` | port |
| A01-49 | Transcript collapse resize | Collapse/expand of the transcription card syncs all tabs and animates window height by the body delta | `main_window.py:_on_transcription_collapsed` | port |
| A01-50 | Stats reveal | When a result's stats card appears, scroll it into view 350 ms later (12 px margin) | `main_window.py:_reveal_shown_stats` | port |
| A01-51 | Geometry persistence | Debounced 500 ms save after move/resize (not before first show, not while maximized/minimized); saves width without the open sidebar and without page widening; restore accepts any screen that intersects (primary first), normalizes widths <= 680 and legacy expanded widths to 680, clamps to screen | `main_window.py:_save_geometry`, `_restore_window_geometry`, `_apply_saved_geometry`, `_restored_width` | port |
| A01-52 | Compact Mode | Toggle Ctrl+Shift+C; fixed 420x250, compact controller replaces tabs; hides Stats button, edge tab, sidebar, title, maximize; remembers its own x/y; switching to another tab or opening History leaves it; persisted | `main_window.py:set_compact_mode`, `_enter_compact_mode`, `_leave_compact_mode` | port |
| A01-53 | Host Mode | Toggle Ctrl+Shift+H; dashboard replaces tabs; title "OpenWhisper Host"; History and Compact disabled; own geometry, first time grows to at least 780x780; persisted; leaves compact mode | `main_window.py:set_host_mode` | undecided backend (remote engine) |
| A01-54 | Close to tray | Close hides when `minimize_tray` (default true; true on read error) and a tray exists; otherwise quits | `main_window.py:closeEvent` | port |
| A01-55 | Quit confirmation | Quit asks `confirm_quit()` (busy work) first; saves geometry; `QApplication.quit()` | `main_window.py:request_quit`, `quit_application` | port |
| A01-56 | Restore from tray | Clear minimized, set active, show/showNormal, sync title bar maximize state, raise, activate | `main_window.py:restore_from_tray` | port |
| A01-57 | Recording state relay | Starting a recording switches to Quick Record unless a walkthrough runs in full mode (then Walkthrough tab); compact status "Recording in progress..." / "Ready to record" | `main_window.py:_update_recording_state`, `set_walkthrough_active` | port |
| A01-58 | Transient statuses | "Copied to clipboard" / "Entry deleted" for 2000 ms then "Ready to record" | `main_window.py:_on_history_entry_copied`, `_on_history_entry_deleted` | port |
| A01-59 | Meeting Mode intro | First visit to the unlocked Meeting tab shows the intro once (not if a meeting is already active) | `main_window.py:_schedule_meeting_mode_intro` | port (with a31) |
| A01-60 | History entry dialog and calendar | Sidebar entry opens modal entry dialog; calendar window created once and reused | `main_window.py:_open_history_entry`, `open_history_calendar` | port (a09) |
| A01-61 | Tray icon and menu | Tooltip "OpenWhisper" or "OpenWhisper: update {v} available"; menu as listed above; Meeting Dashboard disabled until a meeting is active; update item hidden until a version is set ("Update to {v}…"); Win/Linux double-click restores; macOS left-click restores, right/Ctrl-click pops the menu under the item | `ui_qt/system_tray.py:SystemTrayManager` | port |
| A01-62 | No-tray desktops | Tray button and "Minimize to Tray" hidden; tooltip "This desktop session does not provide a system tray."; minimize instead of hide; close quits; log warning "No system tray is available; close-to-tray and tray controls are disabled" | `system_tray.py:__init__`, `main_window.py:set_tray_available` | port |
| A01-63 | What's New note | release_line "2.8", updated "2026-11-09", text <= 280 chars; seen when stored `whats_new_seen` equals `updated` | `ui_qt/whats_new.py` | port (OpenWhisper_CPP text) |
| A01-64 | Writing styles per app category | Categories email/work/personal/other; tones formal/casual/very_casual; new-install seed email formal, work casual, personal casual, other formal; prompt blocks for casual and very casual; terminal and code surface rules; overrides list | `services/app_styles.py` | port (logically a12/a13) |
| A01-65 | Child-process environment | Programs started for the user get the user's environment: app-set vars restored, `.env` keys removed, frozen loader paths undone, the app's own venv removed | `services/process_env.py` | port (simplified: no venv/PyInstaller) |
| A01-66 | Desktop session detection | Wayland: Linux and (`XDG_SESSION_TYPE=wayland` or `WAYLAND_DISPLAY`); Omarchy: `OPENWHISPER_UI=omarchy|classic` override, else Linux and (`OMARCHY_PATH` or a `colors.toml` exists or `/etc/os-release` `ID=omarchy`), cached | `services/desktop_session.py` | other-platform only (Linux) |
| A01-67 | Quiet asyncio resets | Proactor `ConnectionResetError` while closing logged at DEBUG, not ERROR | `services/asyncio_utils.py` | drop (no asyncio; keep the intent in the HTTP server layer) |

## Modules

### `_version.py` (13 lines)
Single `__version__ = "2.8.92"`; also read by the PyInstaller version resource and Inno Setup.
Port: a CMake `project(VERSION)` feeding `version.h.in` (OpenWhisper_CPP already has
`src/engine/version.h.in`). Note: unrelated to `services.database.SCHEMA_VERSION`.

### `config.py` (843 lines)
Responsibility: path resolution and the global `config = AppConfig()` constant bag, imported by
nearly every module and test.
- Public: `APP_NAME`, `ENV_FILE_NAME`, `DATA_DIR_ENV`, `SETTINGS_FILENAME`, `is_frozen`,
  `host_machine`, `parakeet_default_supported(platform_name, machine)`,
  `parakeet_mlx_supported(platform_name, machine, macos_version)`, `bundle_root`, `local_app_dir`,
  `data_root`, `components_root`, `user_data_path`, `env_file_path`, `AppConfig`, `config`.
- Threading: none; `data_root()` is re-evaluated on every call (cheap fs probes) and creates the
  directory. `AppConfig` field defaults call `user_data_path` at construction, so paths are fixed
  at import time (`config.py:843`).
- Edge cases to keep: `host_machine` must never call WMI on Windows (`config.py:57-71`, 0xC000070A
  crash); `.git` may be a file in a worktree (`config.py:162`); settings file in cwd wins over the
  checkout (`config.py:159-161`); `LOG_LEVEL` is read from env at class definition (`:243`).
- Constants by consumer area (all in `AppConfig`):
  - Paths: `SETTINGS_FILE`, `RECORDED_AUDIO_FILE`, `LOG_FILE`, `HISTORY_FILE`, `RECORDINGS_FOLDER`,
    `DATABASE_FILE`, `SCRATCHPAD_FILE`, `MCP_EVENT_SUBSCRIPTIONS_FILE`, `MEETINGS_FOLDER`.
  - Retention: `MAX_SAVED_RECORDINGS=20`, `MAX_SAVED_RECORDINGS_MB=1024`.
  - Audio (a05): `CHUNK_SIZE=1024`, `CHANNELS=1`, `SAMPLE_RATE=44100`,
    `WHISPER_TARGET_SAMPLE_RATE=16000`, post-roll block `:369-402`, `END_PADDING_MS=500`.
  - Hotkeys/timing (a06): `HOTKEY_DEBOUNCE_MS=300`, `AUTO_PASTE_CLIPBOARD_RESTORE_DELAY_MS=250`,
    `RECORD_MIN_HOLD_MS=250`, `RECORD_LATCH_WINDOW_MS=400`, `OVERLAY_HIDE_DELAY_MS=1500`,
    `CANCELLATION_ANIMATION_DURATION_MS=1200`, `CANCELLATION_GRACE_MS=200`,
    `HOTKEY_WATCHDOG_INTERVAL_MS=10000`, `HOTKEY_SLEEP_GAP_THRESHOLD_SEC=30`,
    `HOTKEY_HOOK_REFRESH_INTERVAL_MS=300000`, `RECORDING_TRIGGER_MODE="toggle"`,
    `COMMAND_SELECTION_TIMEOUT_MS=700`, context capture 1000/200 chars, 0.4 s deadline.
  - Window (this area): min 650x660, default 680x680, transcript expand height 840, sidebar 380,
    edge tab 24, max width 1280, compact 420x250, host default 780x780.
  - Overlay (a07): 300x80, streaming max height 400, 30 fps, smoothing 0.3, font 16,
    `STREAMING_OVERLAY_LAYOUT="header"`, `RECORDING_OVERLAY_LOOK="bars"`, particle style config.
  - Appearance (a02): `UI_FONT_SCALE=100`, `UI_THEME="dark"`.
  - Splitting (a04): `MAX_FILE_SIZE_MB=23`, silence 0.01 / 0.5 s, min chunk 30 s, overlap 2 s.
  - Streaming (a04): `STREAMING_ENABLED=False`, chunk 3.0 s, overlap 0.75 s, queue 10 / 3.0 s,
    preview/native/warmup/incremental backend tuples, native update 0.75 s, native queue 10 s,
    incremental poll 1.0 s, `STREAM_FINAL_MIN_SEC=30`, tolerance 0.1 s.
  - Cleanup (a12): provider "openrouter", model "gpt-6-luna", OpenRouter model "openrouter/free",
    sort "alphabetical", reasoning "off", base URL `https://openrouter.ai/api/v1`, timeout 8.0 s,
    max retries 1, level "medium", all prompt sentences `:541-674`, max rules 50, dictionary 500,
    recognition phrases 50, snippets 200, transforms 50.
  - Batch upload (a08/a12): relation "separate", custom combine False, instruction 2000 chars,
    source name 120 chars, timeout 120 s, retries 2, prompt templates `:690-713`.
  - Updater (a16): checks and notices on, restart prompt on, interval 4 h, first check delay 8000 ms.
  - Meeting (a24-a31): `MEETING_*` block `:729-750`.
  - `DEVELOPER_MODE=False`.

### `main.py` (416 lines)
Responsibility: process entry, CLI dispatch, pre-Qt native bootstrap, data lease, restart.
Ordered steps (order is load-bearing; tests pin some of it):
1. `multiprocessing.freeze_support()` (`:9`) - drop.
2. `_handle_early_cli()` (`:159`): help, diagnostics, api, channel, local-asr-worker,
   macos-update-helper, version. No Qt or CUDA loaded on these paths
   (`tests/test_main_entrypoint.py:test_help_loads_neither_qt_nor_the_speech_runtimes`).
3. `native_libraries` import, `_early_update_gate()` (Windows frozen), CUDA/ICU registration,
   CUDA preload, component prune + activate (failures logged, never fatal, `:242-262`),
   subprocess patch, Linux dependency check.
4. `_handle_package_self_test()` (isolated-worker, workflow-smoke, self-test) after the native
   bootstrap so Qt imports see the ICU dirs.
5. `_ensure_writable_stdio()` after every CLI dispatch (`test_stdio_guard_is_placed_after_every_cli_dispatch`).
6. `acquire_startup_data_lease(data_root())` (`:367`) or fatal error.
7. `remember_launch()`, `ui_qt.bootstrap.main()`, `release_startup_data_lease()` in `finally`,
   `logging.shutdown()`, restart handling, `os._exit(exit_code)` (skips interpreter teardown that
   crashed with the `keyboard` listener thread).
Port: `int main(argc, argv)` with a small arg parser (QCommandLineParser cannot be used before
QCoreApplication for `--help` speed requirement; a hand parser is fine).

### `services/diagnostics.py` (306 lines)
- Public: `FailureCapture(logging.Handler)`, `install_failure_capture()`, `MetricsCapture`,
  `record_metrics(**values)`, `collect_report(root=None)`, `main(argv)`, constants
  `JOURNAL_NAME`, `METRICS_NAME`, `MAX_FAILURES=50`, `MAX_METRIC_SAMPLES=100`, `METRIC_NAMES`.
- `FailureCapture.emit` maps logger name to component: `services.<known>` for 11 known components
  (`_COMPONENTS`, `:31-35`), `transcriber.` -> transcriber, `ui_qt.` -> ui, `meeting.` -> meeting,
  else application. Category = first matching exception class name from `_EXCEPTIONS`
  (TimeoutError, PermissionError, FileNotFoundError, MemoryError, ConnectionError, OSError,
  ValueError, RuntimeError; order matters: subclasses first) or "Error", or "LoggedError" with no
  exception. Never formats the message (`test_failure_handler_never_formats_message_or_exception`).
- Loading revalidates every persisted event (`_failure_events`, `:49-69`): component and category
  allowlists, ISO timestamp, line int in 1..99999 else null. Files over 1 MiB are ignored.
- `MetricsCapture`: one daemon writer; `record` updates in-memory samples under a condition and
  sets an event; writer snapshots and writes outside the lock; generations track completion;
  `flush(timeout)`/`close(timeout=0.1)` never retry IO. Must not block the caller when storage
  stalls (`test_stalled_metric_storage_does_not_block_qt_or_build_write_queue`).
- `record_metrics` is a no-op before `install_failure_capture` (`test_record_metrics_is_inert_before_app_logging`).
- Report: engine allowlist uses `local_asr.catalog.MODELS` backends and `WHISPER_MODEL_CHOICES`.
- Metric names: `startup_to_window_s`, `stop_to_result_s`, `cancel_s`, `shutdown_s`,
  `captured_frames`, `dropped_frames`, `device_switches`, `input_overflows`, `peak_rss_mb`,
  `context_capture_ms`, `context_capture_timeouts`, `mouse_hook_callback_ms`.

### `services/process_env.py` (191 lines)
- Public: `set_for_app(name, value, keep_existing=False)`, `load_for_app(values) -> refused`,
  `prepend_for_app(name, entries)`, `external_env(extra=None) -> dict`, `drop(env, names)`,
  `forget()`.
- A module-global `_original` map (lock-protected) remembers each variable's value before the first
  change (None = was unset). Windows keys are upper-cased (`_key`).
- `load_for_app` (the `.env` loader) never overrides a user value; a value already equal to the
  file's is still recorded as the app's (`:63-89`).
- `external_env`: restores originals; frozen build removes `_PYI_*`, `_MEIPASS2`, sets
  `PYINSTALLER_RESET_ENVIRONMENT=1`, restores `*_ORIG` loader paths or drops bundle entries;
  removes the app's own virtualenv only if `sys.prefix` is inside it (`:126-131`).
- Port: keep `set_for_app` / `prepend_for_app` / `load_for_app` / `external_env` / `drop` as a
  Qt-free `fv::ProcessEnv` returning a map usable as `QProcessEnvironment`. PyInstaller and venv
  branches drop; a bundled-library dir (if OpenWhisper_CPP ships one on Linux) replaces them.

### `services/ui_instance.py` (204 lines)
- Public: `SHOW = b"show\n"`, `runtime_paths(data_dir)`, `request_show(sock_name, timeout=0.4)`,
  `UiInstance(lock, sock)` with `attach_activate(callback)`, `close()`, `acquire_ui_instance(data_dir)`,
  `release_ui_instance()`.
- Thread `ui-instance` (daemon) accepts connections, reads up to ~64 bytes until `\n`, and on a
  line starting with `show` sets `_pending` and calls the callback. A request that arrives before
  `attach_activate` is replayed on attach (`:82-89`).
- Non-Linux returns a no-op primary. The lock file refuses symlinks (`:159-160`, `O_NOFOLLOW`),
  mode 0600, directory 0700; a lock holder that never answers still blocks a second copy
  (`test_lock_holder_without_a_socket_blocks_a_second_copy`). Different data dirs get separate UIs.
- Port: `QLockFile` + `QLocalServer`/`QLocalSocket` keyed by the same hash on every platform
  (OpenWhisper_CPP could extend single-instance to Windows; decide with the updater mutex).

### `services/desktop_session.py` (50 lines)
`is_wayland_session()`, `omarchy_theme_paths()` (`$XDG_STATE_HOME/omarchy/current/theme/colors.toml`
then `$XDG_CONFIG_HOME/...`), `use_omarchy_ui()` with `lru_cache` on the probe. Env override
`OPENWHISPER_UI`. No Qt, no X connection (must stay so).

### `services/asyncio_utils.py` (31 lines)
`quiet_connection_lost(loop)`: exception handler that demotes `ConnectionResetError` /
`ConnectionAbortedError` from `_call_connection_lost` to DEBUG. Used by `meeting/web/server.py`
and `services/agent_mcp/runtime.py`. Python-only; the C++ HTTP layer should simply not log a peer
reset during close as an error.

### `services/app_styles.py` (194 lines)
Pure logic over settings and a `FocusSnapshot` (a06). Public: `AppCategory`, `Tone`, `Surface`,
`AppStyle`, `NEW_INSTALL_TONES`, `resolve_tones`, `resolve_overrides`, `category_for`, `style_for`,
`prompt_block`, `set_tone`, `set_override`, `remove_override`.
- No style when styles are off, the identity is missing, or the focused app is OpenWhisper itself
  (`identity.is_self`). `category_for` works with styles off (History keeps the category).
- Formal adds no text; casual/very casual blocks use `{writing}` = "an email", "a work chat
  message", "a personal message", "a note". Terminal and code rules apply whatever the tone.
- `set_tone` writes all four categories (so unset ones stay Formal). `set_override` trims to
  `catalog.MAX_MATCH_CHARS`, replaces case-insensitively, drops a choice equal to the built-in
  category, puts the newest first, caps at `catalog.MAX_OVERRIDES`; raises
  `ValueError("Choose an app and a style")`.

### `ui_qt/__init__.py` (28 lines)
Lazy exports (`QtApplication`, `MainWindow`, `LoadingScreen`, `WaveformOverlay`). Drop; C++ has
headers.

### `ui_qt/app.py` (189 lines) - `QtApplication`
Wraps `QApplication`. Sets names before construction, macOS `CFBundleName` rewrite (CoreFoundation
via ctypes, `:28-72`), Wayland data-control preference (`ui_qt.clipboard.prefer_wayland_data_control`,
a06), Omarchy env/style, app icon, event filters (`DesktopWindowFilter`, `RoundedTooltipFilter`,
`WidgetStyleFilter`), `SnappyTooltipStyle` proxy over the platform style (keep a reference or it
crashes at exit, `:155-162`), `ThemeManager` built from the resolved palette before the splash
paints, `set_theme(preference) -> bool`, `apply_font_scale(percent)`, `colorSchemeChanged` hook,
`run(main_window)` shows and `exec()`s, `quit()`, `exit(code)`.

### `ui_qt/bootstrap.py` (454 lines)
Startup flow, logging, crash log, Qt message handler, `qt_thread_callback`, `run_with_ui_pulse`,
update acknowledgement, shutdown. Profiler marks in order: `main_entered`, `logging_ready`,
`early_imports_started`, `early_imports_finished`, `qt_app_created`, `loading_screen_constructed`,
`loading_screen_shown`, `first_visual_flushed`, `late_imports_started`, `late_imports_finished`,
`ui_controller_created`, `application_controller_created`, `main_window_shown`.
State: `acquire_ui_instance` returns None -> return 0 without creating Qt objects
(`test_main_hands_off_to_the_running_ui`). The splash is destroyed before
`show_main_window()`; the instance activation callback is attached right after.
`create_deferred_local_whisper_backend` builds `LocalWhisperBackend(load=False)` - drop.

### `ui_qt/startup_profiler.py` (39 lines)
Dataclass with `start_time = perf_counter()`, `events: [(name, elapsed)]`, `mark`, `log_summary`.
Port with `QElapsedTimer` or `std::chrono::steady_clock`.

### `ui_qt/loading_screen.py` (229 lines) - `LoadingScreen(QWidget)`
Layout: none (fully painted). Palette tokens (a02 `current_palette()`): `splash-bg`,
`splash-border`, `accent`, `accent-cyan`, `text`, `text-secondary`, plus `is_dark`.
Classic (not compositor-managed):
- Flags `FramelessWindowHint | WindowStaysOnTopHint | Tool`, `WA_TranslucentBackground`, fixed
  450x300, centered on `primaryScreen().geometry()`.
- Background: rounded rect radius 16, vertical gradient bg -> `bg.darker(120)` (dark) or
  `darker(104)` (light), 1 px border.
- Glow center (w/2, h/2 - 20); `phase = elapsed_monotonic * 4.5`, `pulse = 0.5 + 0.5 sin(phase)`.
  Outer radial radius `58 + 14 pulse`, stops 0: accent a=`55+45p`, 0.45: a=`18+22p`, 1: a=0.
  Inner radius `28 + 6 pulse`, stops 0: accent-cyan a=`70+70p`, 0.55: accent a=`35+40p`, 1: 0.
- 5 dots on a 35 px orbit starting at -90 deg, `dot_pulse = 0.5+0.5 sin(phase + 0.7 i)`, size
  `5 + 2 dp`, alpha `140 + 115 dp`.
- Mic: 16x24 rounded rect radius 8, 2 px pen in text color alpha `200 + 55p`, stem from y+12 to
  y+18, base line x-8..x+8 at y+18; inner fill accent alpha `(p-0.35)/0.65*55` when p > 0.35,
  rect adjusted 2 px radius 6.
- Text: "OpenWhisper" Segoe UI 16 Bold at `(0, h-90, w, 30)` centered; status Segoe UI 10
  DemiBold accent at `(0, h-55, w, 20)`; progress Segoe UI 9 text-secondary at `(0, h-35, w, 20)`.
- Timer 33 ms `PreciseTimer` calling `update()`; phase from wall clock so delayed ticks still
  show the right frame. Left-button drag moves it.
Compositor-managed (Wayland or Omarchy): `Qt::SplashScreen`, opaque, fixed 450x180, no timer,
no move; fill bg, 3 px accent strip at top; "OpenWhisper" Noto Sans 18 DemiBold at
`(28, 28, w-56, 40)`, status Noto Sans 11 accent at `(28, 86, ...)`, progress Noto Sans 9 at
`(28, 122, ...)`, left-aligned.
Defaults: status "Initializing...", progress "Please wait...". `update_status`, `update_progress`,
`destroy()` stops the timer. Log "Loading screen closed" on close.

### `ui_qt/system_tray.py` (196 lines) - `SystemTrayManager(QSystemTrayIcon)`
Signals: `exit_requested`, `toggle_recording`, `meeting_toggle_requested`,
`meeting_dashboard_requested`, `copy_last_original_requested`, `walkthrough_requested`,
`screenshot_requested`, `scratchpad_requested`, `stats_requested`, `update_requested`,
`language_menu_requested(QMenu*)` (emitted on the submenu's `aboutToShow` so the controller
refills it). Methods: `set_recording(bool)`, `set_walkthrough_active(bool)`,
`set_update_available(version)`, `set_meeting_active(active, dashboard_available=None)`;
attribute `available`.
Menu order: Show | Hide | --- | Start Recording | Copy original of last dictation | Start
Walkthrough | Take Screenshot | Language > | --- | Start Meeting | Open Meeting Dashboard
(disabled) | --- | Scratchpad | Stats | Settings | --- | (hidden update item) | Exit.
Settings, update and Exit use `MenuRole::NoRole` (so macOS does not move them).
Show calls `main_window.restore_from_tray()`, Hide calls `main_window.hide()`, Settings calls
`main_window.open_settings()`. Created by `UIController` (a03, `ui_controller.py:267`), which wires
every signal (`:480-496`) and, on macOS with a tray, installs dock icon sync.

### `ui_qt/whats_new.py` (49 lines)
`WhatsNew(release_line, updated, text)`, `WHATS_NEW`, `MAX_TEXT_LENGTH=280`, `is_seen(seen)`.
Read by Settings Home (`ui_qt/dialogs/settings_home.py:270-289`, a11) with setting
`whats_new_seen`. Port as a constexpr struct.

### `ui_qt/main_window.py` (2,463 lines)

#### `WindowGlyphButton(QPushButton)`
Glyphs `minimize`, `maximize`, `restore`, `close`; 10 px glyph box centered at whole pixel + 0.5;
1.0 px flat-cap pen in `text-secondary`, or the hover token when hovered (`text-heading`, or
`on-accent` for close). Restore draws a 7x7 front square offset 2 px down plus two back edges.
`WA_Hover`; stylesheet paints only the hover background.

#### `CustomTitleBar(QFrame)` - objectName `customTitleBar`
- `BASE_HEIGHT=32`; `setFixedHeight`; stylesheet set before children (`:174-179`, avoids a parse
  warning every launch). `#customTitleBar { background @surface; border-bottom 1px solid @border }`.
- HBox margins (8, 0, 0, 0), spacing 0: `QMenuBar` (AlignVCenter) | stretch | `title_label`
  "OpenWhisper" | stretch | [notice inserted here by `add_notice`] | minimize | maximize | close.
- Menu bar style: transparent, `@text-secondary`, 12 px, items padding 4px 10px, selected
  `@surface-hover` / `@text-heading`, pressed `@surface-active`, separators 1 px `@surface-hover`
  margin 4px 8px. Title label: `@text`, 13 px, weight 600, `'Segoe UI', sans-serif`.
- Buttons 46 x bar height; window buttons hover `@surface-hover`; close hover `@danger` with
  `@on-accent`. Tooltips "Minimize", "Maximize"/"Restore", "Close"; accessible names "Minimize
  window", "Maximize window"/"Restore window", "Close window".
- `_sync_height` on every `LayoutRequest`: height = max(32, menu bar sizeHint + frame border);
  buttons follow (`tests/test_title_bar.py` checks every font scale).
- Maximize toggle keeps `_normal_geometry`, does `showNormal()` before `setGeometry` (`:262-266`);
  ignored in compact mode. `sync_maximized(bool)` follows external changes.
- Mouse: press near a window edge starts the window's resize; else drag; moving a maximized
  window first leaves maximized in place; double-click toggles maximize.
- Uses `@token` stylesheet substitution from a02 (`WidgetStyleFilter`).

#### `_PageLayoutWatcher(QObject)`
Event filter on each page's scroll content; on `LayoutRequest` calls `_schedule_page_fit`.

#### `MainWindow(QMainWindow)`
Window-local shortcuts: `Ctrl+H` (side panel), `Ctrl+Shift+D` (History Calendar),
`Ctrl+Shift+C` (Compact), `Ctrl+Shift+H` (Host), `Ctrl+Q` (Quit/Exit).

Signals (all forwarded to a03 UIController): `record_toggled(bool)`, `record_canceled`,
`walkthrough_requested`, `walkthrough_library_requested(object)`, `walkthroughs_changed`,
`model_changed(str)`, `whisper_engine_changed`, `remote_model_selected(str,str)`,
`remote_runtime_selected(str,str,dict)`, `remote_retry_requested`, `live_preview_changed`,
`cleanup_toggled`, `settings_requested`, `guided_setup_requested`,
`settings_destination_requested(str)` (keys used here: "engine_downloads", "downloads",
"walkthroughs"), `engine_help_requested(str)`, `hotkeys_requested`, `about_requested`,
`check_for_updates_requested`, `update_notice_requested`, `retranscribe_requested(str)`,
`upload_file_requested(str,float)`, `upload_files_requested(object)`, `upload_cancel_requested`,
`upload_copy_requested(str)`, `quick_record_copy_requested(str)`, `meeting_dashboard_requested`,
`scratchpad_requested`, `stats_requested`, `host_model_selected(str,str)`,
`past_meeting_requested(str)`, `past_meeting_copy_requested(str)`,
`past_meeting_delete_requested(str,bool)`, `past_meetings_clear_requested(bool)`,
`screen_created(str, object)`.

Callbacks set by a03: `on_show_copied_animation`, `confirm_quit() -> bool`.

Public methods called from outside: `existing_transcription_tabs`, properties
`transcription_tabs`, `upload_file_tab`, `walkthrough_tab`, `meeting_mode_tab`, `host_dashboard`,
`host_mode`; `update_screen(key, method, *args)`, `update_transcription_tabs(method, *args)`,
`bind_host_service`, `meeting_is_active`, `refresh_live_preview_controls`, `set_walkthrough_active`,
`set_status`, `set_device_info(info, ready)`, `set_remote_models`, `set_remote_link`,
`set_remote_clients`, `set_transcript(text, raw)`, `clear_transcription`,
`set_partial_transcription(text, is_final)`, `clear_partial_transcription`,
`set_transcription_stats(t, audio, size, cleanup_time, remote)`, `clear_transcription_stats`,
`open_settings`, `open_downloads`, `open_hotkey_settings`, `check_for_updates`, `show_about`,
`set_update_notice(version)`, `set_tray_available`, `minimize_to_tray`, `toggle_compact_mode`,
`set_compact_mode(compact, persist)`, `toggle_host_mode`, `set_host_mode(enabled, persist)`,
`toggle_tray_visibility`, `restore_from_tray`, `request_quit`, `quit_application`,
`toggle_history`, `refresh_history`, `show_walkthrough(folder)`, `show_past_items(tab_index)`,
`reload_walkthrough`, `walkthrough_finished(folder)`, `refresh_past_meetings`,
`open_history_calendar`, `update_hotkeys(record, cancel, enable_disable, minimize, walkthrough,
screenshot)`; also `is_recording` attribute and `_update_recording_state()` (called by a03).

Layout tree (classic, frameless):
```
QMainWindow  flags Window|FramelessWindowHint  (compositor: Window|CustomizeWindowHint minus buttons)
 centralWidget  objectName "centralWidget"  "QWidget#centralWidget { border: 1px solid @border }"
  QVBoxLayout margins 1,1,1,1 spacing 0
   CustomTitleBar (or DesktopHeader from a02 when compositor-managed)
     + UpdateNoticeButton via add_notice (a02)
   content_wrapper (stretch 1; inside a frameless QScrollArea when compositor-managed)
    QHBoxLayout margins 0 spacing 0
     main_area (stretch 1) QVBoxLayout margins 0 spacing 0
      TabbedContentWidget (a02): "Quick Record", "Upload File", "Walkthrough", "Meeting Mode"
      CompactRecordController (a08), hidden
      [HostDashboard (a18), added on first use, hidden]
     HistoryEdgeTab (a09) shortcut hint "Ctrl+H"
     HistorySidebar (a09)
   footer  objectName "footerBar"  fixed height 48
    QHBoxLayout margins 16,7,16,7 spacing 0:
     stretch | trayButton | 10 | statsButton | 10 | settingsButton | 10 | quitButton | stretch
```
Footer buttons (all height 34, `PointingHandCursor`, radius 8, padding 6px 18px, weight 600,
13 px, bg `@surface`, border 1px `@border`):
- `trayButton` (a02 `Button`, hotkey hint from `minimize_tray` default), text "Minimize to Tray"
  (min width 140) or "Hide" (60), color `@text-body`, hover `@accent`/`@on-accent`, pressed
  `@accent-pressed`; tooltip "Hide OpenWhisper in the system tray".
- `statsButton` "Stats", min 80, `@purple-text`, hover `@purple`, pressed `rgba(@purple-rgb, 0.8)`;
  tooltip "Your words, pace, streak and favorite apps".
- `settingsButton` "Settings", min 100, `@accent-cyan`, hover `@accent`, pressed `@accent-pressed`;
  tooltip "Settings, models, and downloads (starts on the Overview)".
- `quitButton` "Quit", min 100, `@danger-text`, hover `@danger`, pressed `@danger-pressed`;
  `HotkeyHintFilter(quit, "Ctrl+Q")`.
Footer bar: `background @bg; border-top: 1px solid @border-subtle`.
Menus: see A01-44. `Check for Updates...` uses `ApplicationSpecificRole` on macOS (merged into
the app menu); Settings, Guided setup, Downloads, Quit/Exit, About use `NoRole`. Host Mode status
tip "Show what this computer serves to paired computers instead of the recording tabs".
The native `menuBar()` is hidden; menus live in the title bar's `QMenuBar`.

State and invariants:
- View state machine: Full <-> Compact (not from Host), Full <-> Host (entering Host leaves
  Compact first, persisting it off). Each view has its own saved geometry: `window_geometry`,
  `compact_window_geometry` (x, y only), `host_window_geometry`.
- Tab switch (`_on_tab_changed`): creates the lazy screen, leaves compact mode for any tab but
  Quick Record, retargets the side panel, schedules a 75 ms sidebar refresh, refits now
  (`_refit_page`, not deferred, `:1271-1279`), schedules the Meeting intro.
- `update_screen` keeps the latest call per method (meeting `set_meeting_state` merges dicts;
  `set_model_downloading` keys by first arg) and replays on creation (`:771-781`).
- Page fit (`_sync_page_size`, `:1333-1395`) is skipped when compositor-managed, compact, host,
  hidden, maximized, resizing, or animating (sidebar or window); refits after those finish.
  Width logic `_page_width` (`:1309-1331`); content height uses `heightForWidth` and the
  walkthrough split's `place_for_width` (`:1288-1300`).
- Resize from edges: 8 px margin; cursor shapes per edge; left/top edges clamp to min/max so the
  window does not slide (`:2211-2222`); a drag ends by saving Quick Record's chosen height when
  its transcript is open and clearing `_sidebar_squeeze`.
- Mouse tracking via an application-wide event filter (classic only) to update edge cursors.
- `_clamp_geometry` (`:1866-1893`) keeps the rect fully on one screen; fallback screen
  1280x800 when none.
- `_restored_width`: legacy format detection via `format != "collapsed_content_v1"` and width >=
  680+380-24 (`:2386-2406`).
- Compositor-managed: no fitting, no animation, no geometry save/restore (first show sizes to
  min(960, avail.w-64) x min(680, avail.h-64)), min size 360x240, unlimited max.
- `showEvent`: first show refits without animation; later shows restore the current view's
  geometry, then refit.
- `closeEvent` flushes the tab bar's pending tab-selection save first.

## Data and formats

Settings keys read or written in this area (JSON file `openwhisper_settings.json` in the data
root, accessed through `services/settings.py` a10):

| Key | Type | Default | Where |
|---|---|---|---|
| `window_geometry` | dict `{x, y, width, height, format:"collapsed_content_v1", history_expanded}` | absent | `main_window.py:_save_geometry`, `_restore_window_geometry` |
| `compact_window_geometry` | dict `{x, y}` | absent | `_save_compact_geometry`, `_restore_compact_geometry` |
| `compact_mode` | bool | false | `set_compact_mode`, `_restore_compact_mode` |
| `host_mode` | bool | false | `set_host_mode`, `_restore_host_mode` |
| `host_window_geometry` | dict `{x, y, width, height}` | absent | `_save_host_geometry`, `_restore_host_geometry` |
| `minimize_tray` | bool | true | `closeEvent` |
| `meeting_mode_intro_seen` | bool | false | `_schedule_meeting_mode_intro` (via `resolve_meeting_mode_intro_seen`) |
| `last_tab_index` | raw (saved tab id) | absent | via `TabbedContentWidget` (a02) |
| `selected_model` (via `load_model_selection`) | str | `DEFAULT_BACKEND` | `_load_saved_settings` |
| `ui_theme`, `ui_font_scale` | str / int | "dark" / 100 | `app.py` via `resolve_ui_theme`, `resolve_ui_font_scale` |
| `whats_new_seen` | str ISO date | "" | Settings Home (a11) against `whats_new.WHATS_NEW.updated` |
| `app_styles_enabled` | bool | true | `app_styles.style_for` |
| `app_style_tones` | dict category->tone or absent | absent (= all formal); seeded with `NEW_INSTALL_TONES` on a brand-new install (`settings.py:1412-1418`) | `app_styles.resolve_tones`, `set_tone` |
| `app_style_overrides` | list `[{match, category}]` | [] | `resolve_overrides`, `set_override`, `remove_override` |
| diagnostics allowlist reads `selected_model`, `whisper_model`, `whisper_device`, `whisper_compute_type`, `local_asr_models` | | | `diagnostics.collect_report` |

Files in the data root (`data_root()`):
- `openwhisper_settings.json`, `openwhisper.db`, `transcription_history.json` (legacy),
  `recorded_audio.wav`, `recordings/`, `meetings/`, `scratchpad.txt`,
  `mcp_event_subscriptions.json`, `.env` (API keys; also checkout fallback).
- `openwhisper.log` (+ `.1`..`.3`), `openwhisper.crash.log` (append).
- `diagnostic-failures.json` (list of `{utc, component, category, line}`, pretty-printed, max 50),
  `diagnostic-metrics.json` (`{name: [numbers]}`, compact), both via `.tmp` + atomic replace.
- `<local_app_dir>/components/` for downloaded runtimes and models (shared across checkouts).
Linux runtime files: `$XDG_RUNTIME_DIR/openwhisper/ui-<16hex>.lock` (0600, dir 0700) and abstract
socket `\0openwhisper-ui-<uid>-<16hex>`; wire protocol: client writes `show\n` and closes.
Diagnostics report JSON: see A01-29; `privacy` string is fixed: "No transcript, audio, log
messages, paths, credentials, hostnames or pairing data included."
Environment variables: `OPENWHISPER_DATA_DIR`, `OPENWHISPER_LOG_LEVEL`, `OPENWHISPER_UI`,
`OPENWHISPER_API_TOKEN` (for `--api`, a20), `LOCALAPPDATA`, `XDG_DATA_HOME`, `XDG_RUNTIME_DIR`,
`XDG_STATE_HOME`, `XDG_CONFIG_HOME`, `XDG_SESSION_TYPE`, `WAYLAND_DISPLAY`, `OMARCHY_PATH`,
`PROCESSOR_ARCHITECTURE(W6432)`, `HF_HUB_OFFLINE` (drop).
Installed-system contracts: Linux `.desktop` file name `openwhisper` must match
`setDesktopFileName`; Windows frozen install at `%LOCALAPPDATA%\Programs\OpenWhisper` with data in
`%LOCALAPPDATA%\OpenWhisper`; bundled assets `ui_qt/styles/theme.qss`,
`ui_qt/assets/openwhisper.ico`, `openwhisper.png`, `check.svg`, `webui/dist/index.html`.
Decision needed: whether OpenWhisper_CPP reads an existing OpenWhisper data directory (migration) or
uses its own name (the CLAUDE.md says it is a separate project).

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| PyQt6 QtWidgets/QtGui/QtCore | everything UI | Qt 6 Widgets (same classes) |
| `logging`, `RotatingFileHandler` | logs | small `fv::Log` sink with size rotation, or spdlog rotating sink (vcpkg); `qInstallMessageHandler` |
| `faulthandler`, `signal` | crash log | Windows `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` / stack walk; POSIX `sigaction` with backtrace; or crashpad/breakpad (decide) |
| `threading` | metrics writer, ui-instance accept thread, startup worker | `std::thread`/`std::condition_variable`; QLocalServer needs no thread |
| `socket` AF_UNIX abstract, `fcntl.flock` | single instance | `QLocalServer`/`QLocalSocket`, `QLockFile` (or `flock` directly) |
| `hashlib.sha256` | instance key | `QCryptographicHash::Sha256` |
| `json` | journal, metrics, report, settings | `QJsonDocument` in Qt layer, or nlohmann-json in the Qt-free core (vcpkg) |
| `importlib.metadata` | package versions in the report | compile-time versions (Qt, NeMo runtime version from the C ABI) |
| `platform`, `os.cpu_count` | report | `QSysInfo`, `std::thread::hardware_concurrency` |
| `ctypes` kernel32 `IsWow64Process2` | arch | direct Win32 call |
| `ctypes` CoreFoundation | macOS bundle name | not needed when shipping a real `.app` with Info.plist |
| `subprocess` / `CREATE_NO_WINDOW` | children without consoles | `QProcess` with `setCreateProcessArgumentsModifier` adding `CREATE_NO_WINDOW` |
| `os.environ` | process_env | `qputenv`/`qgetenv`, `QProcessEnvironment::systemEnvironment()` |
| `asyncio` | quiet resets | drop |
| `numpy` (`AUDIO_FORMAT`) | constant only | `int16_t` |
| `multiprocessing` | frozen workers | drop |

## Platform-specific code

- Windows: WMI-free arch (`config.py:host_machine`); `%LOCALAPPDATA%` paths; frozen-build update
  gate and application mutex (`main.py:_early_update_gate`, release before restart `:400-406`);
  `CREATE_NO_WINDOW` for children; CUDA DLL and ICU directory registration (drop); env names
  case-insensitive (`process_env._key`, `drop`); proactor reset noise (asyncio_utils).
- macOS: Control+Option default hotkeys; `Python.app` bundle-name rewrite (`app.py:28-72`);
  `--macos-update-helper`; "Quit" vs "Exit" label; `Check for Updates...` in the app menu;
  tray: menu not attached, left click restores, right/Ctrl(Meta)-click pops menu at
  `geometry().bottomLeft()` or cursor (`system_tray.py:13-16, 116-139`); `restore_from_tray`'s
  full clear-minimized + show + raise + activate sequence exists for macOS
  (`main_window.py:1758-1783`); explicit `Qt::Window` flag with frameless or the window cannot
  return from hide (`:428-436`); dock icon sync (a03). Parakeet MLX defaults drop.
- Linux X11: classic frameless window; single-instance lock and socket; Linux dependency
  preflight (`services.linux_deps`, a16); CUDA preload summary; desktop file name.
- Linux Wayland and Omarchy (Hyprland): `compositor_managed()` = Wayland platform or Omarchy UI.
  Then: native decorations off but compositor-managed toplevel (`without_window_buttons`),
  `DesktopHeader` instead of `CustomTitleBar`, content inside a QScrollArea, no self-resizing,
  no geometry persistence, no animations, edge resizing disabled, splash opaque and static,
  footer tray button says "Hide". Omarchy also: `QT_QPA_PLATFORMTHEME=""`,
  `QT_WAYLAND_DISABLE_WINDOWDECORATION=1`, Fusion style, 2 s theme-file polling.
  Wayland clipboard data-control preference before `QApplication` (a06).
- Any desktop without a tray: tray-only controls hidden, close quits (A01-62).

## Engine-specific code

Drop (non-Parakeet or Python worker plumbing):
- `config.py`: `WHISPER_MODEL_CHOICES`, `DEFAULT_WHISPER_MODEL`, `FASTER_WHISPER_*`,
  `WHISPER_RELOAD_DEBOUNCE_MS`, `parakeet_mlx_supported` and the MLX branches,
  `MEETING_WHISPER_MODEL`, Moonshine/Qwen/MLX/Unified entries in `MODEL_CHOICES`/
  `MODEL_VALUE_MAP` and in the `STREAMING_*_BACKENDS`, `SPEECH_WARMUP_BACKENDS`,
  `INCREMENTAL_DICTATION_BACKENDS` tuples; `STREAM_FINAL_*` (Parakeet Unified stream-final).
- `main.py`: `freeze_support`, `--local-asr-worker`, `--isolated-worker`, CUDA DLL/ICU/preload
  bootstrap, self-test imports of `ctranslate2`, `faster_whisper`, `onnxruntime`, `av`.
- `bootstrap.py`: `create_deferred_local_whisper_backend`, `_log_hf_hub_offline`,
  CUDA preload summary (replace with a NeMo runtime-load summary).
- `diagnostics.py`: `whisper_*` allowlist entries and the faster-whisper/ctranslate2/av package
  versions.
Adapt to in-process Parakeet:
- `DEFAULT_BACKEND` is always Parakeet; `parakeet_default_supported` becomes "is a NeMo runtime
  available for this host" (OpenWhisper_CPP engine `platform.cpp`/`runtime.cpp`).
- `run_with_ui_pulse` is the natural place to load the NeMo runtime and model on
  `EngineWorker`'s thread while the splash animates; per CLAUDE.md the engine must never be
  called from the GUI thread, so use queued signals rather than a nested loop polling a box.
- `STREAMING_PREVIEW_BACKENDS` = parakeet (window re-decode via `Recognizer::transcribe`;
  Parakeet TDT has no native streaming), `SPEECH_WARMUP_BACKENDS` and
  `INCREMENTAL_DICTATION_BACKENDS` = parakeet.
- Undecided backends: Nemotron Streaming (same NeMo runtime, native streaming), API (OpenAI/Groq/
  own server: `API_MODEL_CHOICES`), Remote computer (Host Mode, `host_model_selected`,
  `remote_*` signals, `bind_host_service`).

## Dependencies on other areas

Out (this area calls):
- a02 ui-foundation: `TabbedContentWidget`, `Button`, `HotkeyHintFilter`, `UpdateNoticeButton`,
  `DesktopHeader`, `compositor_managed`, `without_window_buttons`, `DesktopWindowFilter`,
  `collapse_animation` constants (300 ms, InOutCubic, 16777215), `palette` (`token_color`,
  `current_palette`), `app_icon`, `font_scale` (`apply_ui_font_scale`, `apply_ui_theme`,
  `WidgetStyleFilter`, `resolve_theme_preference`), `ThemeManager`, `tooltip_filter`.
- a03 ui-controller: `UIController` (creates `MainWindow` and `SystemTrayManager`, wires every
  signal, `show_main_window`, `offer_guided_setup`, `build_settings_when_idle`,
  `set_ambient_status`, `show_apply_error_if_any`, `agent_data_changed`, `cleanup`),
  `ApplicationController(ui_controller, local_backend=)` (`setup_backup`, `notify_main_ui_ready`,
  `meeting_runtime`, `remote_engine`, `update_readiness_error`, `cleanup`).
- a06: `format_hotkey_display`, `prefer_wayland_data_control`, `focus_context` (`FocusSnapshot`,
  `catalog.classify`, `parse_overrides`, `builtin_category`, `MAX_MATCH_CHARS`, `MAX_OVERRIDES`).
- a08: `QuickRecordTab`, `CompactRecordController`, `UploadFileTab` (lazy).
- a09: `HistorySidebar`, `HistoryEdgeTab`, `history_manager.delete_entry`, `HistoryEntryDialog`,
  `HistoryCalendarDialog`.
- a10: `settings_manager`, `SettingsKey`, `resolve_*`, `keep_unchosen_engine`,
  `keep_separate_meeting_speech`, `is_hf_hub_offline_env_set`, `backup_startup`
  (`acquire_startup_data_lease`, `release_startup_data_lease`, `spawn_restart`,
  `RESTART_EXIT_CODE`), `credentials` (`keyring_did_not_respond`, `UNRESPONSIVE_KEYRING_STATUS`,
  `load_for_app` caller).
- a14: `native_libraries`, `components.prune_orphans`, `component_runtime.activate_components`,
  `local_asr.catalog.MODELS`, `local_asr.worker`, `isolated_worker`, `gpu_info.nvidia_gpu`,
  `transcriber.LocalWhisperBackend`.
- a16: `app_update_apply` (gate, mutex, health token, ack), `app_update_macos`, `code_identity`,
  `linux_deps`, `package_checks`, `release_checks`, `guided_setup.initialize`.
- a18: `HostDashboard` (lazy). a23: `WalkthroughTab` (lazy). a31: `MeetingModeTab` (lazy),
  `meeting_intro_dialog`.
- a19: `automation.channel_bridge.main` (`--channel`). a20: `agent_api.cli.main` (`--api`),
  `agent_mcp.runtime` (`configure_controls`, `restore`, `stop`).
In (who calls this area):
- Every area reads `config` / `data_root` / `user_data_path` / `components_root`.
- `record_metrics`: a03 application_controller, a04 runtime/transcription, a05 recorder,
  a06 focus_context capture and Windows mouse hook.
- `process_env`: `credentials` (a10, `.env` via `load_for_app`), `component_runtime` and
  `native_libraries` (a14, `prepend_for_app`), `installed_agents` (`external_env`, `drop`),
  `ui_qt/clipboard` (a06, `set_for_app`), and this area's `app.py`.
- `desktop_session`: a02 (desktop, font_scale, omarchy_theme, desktop_header, stats charts),
  a06 (hotkeys, pynput, synthetic keys, focus_context linux, snap), a10 settings, a11 dialog.
- `app_styles`: a10 settings seed, a06 synthetic_keys (`Surface`), a13 settings_styles page,
  a12 cleanup prompt assembly.
- `asyncio_utils`: a30 meeting web server, a20 MCP runtime.
- `whats_new`: a11 Settings Home.
- `SystemTrayManager`, `MainWindow`: a03 UIController (constructs and drives them).

## Tests

| Test file | Pins |
|---|---|
| `tests/test_main_entrypoint.py` | Help prints usage and loads neither Qt nor speech runtimes; `main` import does not import ApplicationController; stdio guard placement and behavior; components activate before backend imports even when activation fails; profiler marks and summary format |
| `tests/test_multiprocessing_startup.py` | Frozen helper dispatch before app imports (drop) |
| `tests/test_data_root.py` | All `data_root` rules (A01-01): env wins, frozen per-user, git checkout keeps data, settings in cwd/checkout stay in use, start dir irrelevant |
| `tests/test_bootstrap.py` | Startup order (splash shown and events flushed before late imports), cleanup on run error, keyring ambient status, update health ack (disable, 1500 ms, exit 1 paths), hand-off to running UI creates no Qt objects, cross-thread show runs on Qt thread, CUDA summary wording |
| `tests/test_startup_visibility.py` | Startup shows only the main window |
| `tests/test_ui_instance.py` | Non-Linux primary, second launch sends show and exits, show callback, per-data-dir instances, re-acquire after release, silent lock holder blocks |
| `tests/test_diagnostics.py` | Allowlisted export, handler never formats message, write failure safe, export refuses overwrite, metric bounds and statistics, inert before logging, stalled storage does not block, close flush bounded, malformed files safe |
| `tests/test_process_env.py` | Restore app-set vars, keep user value, remove prepended paths, undo frozen loader env, drop own venv, keep user venv, agents/hooks get user env |
| `tests/test_asyncio_utils.py` | Reset-on-close demoted, other errors still ERROR |
| `tests/test_personalize_s1_styles.py` | Formal for existing installs, new-install tones, no style off/unknown/self, category with styles off, overrides, prompt blocks, set_tone stores all, surfaces always apply, profile wins (a12 side) |
| `tests/test_whats_new.py` | release_line equals major.minor of version, text trimmed and <= 280, updated not future, only current counts as seen |
| `tests/test_loading_screen.py` | Left-button drag moves the splash |
| `tests/test_system_tray.py` | macOS click/right-click/Ctrl-click semantics; other platforms keep the context menu and restore on double-click only |
| `tests/test_title_bar.py` | Menu items fit at every font scale; default keeps a usable bar; larger scale never shrinks it; glyphs are painted; maximize <-> restore |
| `tests/test_main_window_compact.py` | Last tab restored, reshow keeps tab; compact size 420x250 and geometry round-trip; no-tray behavior; footer labels and signals; File > Downloads emits "downloads"; compact delegates to Quick Record; compact persisted/restored; restore clamps short/narrow geometry (660 / 680); minimum width keeps tabs and footer separate; Quick Record controls fit at min height; tab switching while recording (and walkthrough); walkthrough reopen from tray; clamp to 1280x800; geometry save waits for first show; history toggle preserves height; stats scroll into view at 1024x640 |
| `tests/test_window_fits_tab.py` | Every tab fits the window; re-show fits at once; walkthrough keeps size; dragged heights stay; open transcript keeps user height; edge drag leaves maximized; restore leaves maximized; left edge stops at width cap; Quick Record shrinks after a long result |
| `tests/test_host_mode.py:835-940` | Host Mode swap, title "OpenWhisper Host", menu enable state, persisted keys, own geometry exact dict, first size 780x780 clamped, leaves compact, restored at launch, View menu order `History, History Calendar, Compact Mode, Host Mode` and Ctrl+Shift+H, model signal, stats behind dashboard do not resize |
| `tests/test_omarchy_ui.py` | Omarchy detection/override, compositor sizes respected across all transitions, offscreen saved host geometry ignored, opaque static splash, tray restore keeps state, theme polling only restyles on change |
| `tests/test_history_calendar_wiring.py`, `tests/test_update_notice.py` | Calendar and update notice wiring into the window (shared with a09/a02) |

Golden-vector candidates (pure, deterministic; generate inputs/outputs from Python and replay):
`config.data_root`/`_source_data_dir` decision table (env, frozen, cwd/checkout probes),
`config.parakeet_default_supported`, `diagnostics._failure_events` + `_metric_samples` +
`_metric_summary` (percentiles), `diagnostics.FailureCapture` component/category mapping,
`process_env.external_env` (input env + recorded originals -> output env),
`ui_instance._data_key`/`runtime_paths`, `app_styles.prompt_block`/`set_override`/`resolve_tones`,
`MainWindow._clamp_geometry` and `_restored_width` (pure rect math once extracted),
`AppConfig.TRANSCRIPT_CLEANUP_LEVEL_PROMPTS` strings (byte-exact).

## Porting notes

Suggested decomposition:
- Qt-free core (`src/core` or extend `src/engine`'s style, namespace `fv`):
  `fv::paths` (data_root, local_app_dir, components_root, env_file_path; UTF-8 paths per CLAUDE.md),
  `fv::config` (constexpr constants + cleanup prompt strings, default hotkeys per platform),
  `fv::diagnostics` (FailureJournal, MetricsCapture with one writer thread, report builder),
  `fv::process_env`, `fv::app_styles`, `fv::desktop_session` (env/file probes), `fv::whats_new`,
  `fv::StartupProfiler`, CLI parser for early modes. All unit-testable with a compiler alone.
- Qt layer (`src/app`): `Application` (QApplication setup, theme/font, filters), `Bootstrap`
  (startup sequence, logging sink + `qInstallMessageHandler`, crash handler, shutdown),
  `SingleInstance` (QLockFile + QLocalServer), `LoadingScreen`, `SystemTray`, `MainWindow`
  split into `TitleBar`/`WindowGlyphButton`, `MainWindowGeometry` (save/restore/clamp/fit; extract
  the rect math into pure functions for tests), `FooterBar`, and the view-mode logic.
Order of work:
1. paths + config + version + logging/crash + profiler + CLI (`--help`, `--version`).
2. QApplication setup and LoadingScreen (needs a02 palette/theme first, or a stub).
3. MainWindow skeleton: title bar, menus, footer, frameless move/resize, tray, close-to-tray,
   single instance. Tabs as placeholders until a02/a08 land.
4. Geometry persistence and page fitting (needs real tab pages to verify).
5. Compact Mode, lazy screens + `update_screen` replay, side panel lockstep animation.
6. Diagnostics journal/metrics/report, process_env, app_styles (with a12).
7. Host Mode (if the remote engine is kept), update gate/health ack (a16), backup lease (a10).
Risks:
- Page fitting depends on `heightForWidth` of every page and on many timers; it is the most
  fragile behavior and has the most tests. Port it after the pages exist, and keep the same
  deferral points (0 ms timer, refit after animations).
- Frameless window behavior differs per platform (macOS hide/restore, Wayland cannot move
  itself); keep the `compositor_managed` branches exactly.
- `@token` stylesheet substitution must exist before any styled widget is constructed (title
  bar sets its sheet before children for this reason).
- Startup ordering contracts (no Qt on CLI paths, splash before heavy work, data lease before
  settings, restart after lease release) are easy to break in a monolithic `main()`.
- The single-instance design is Linux-only in Python; extending it to Windows interacts with the
  updater's named mutex.
- Many config constants are owned by other areas; keeping one header avoids drift but couples
  rebuilds.
Rough C++ size: core ~1,500 lines (config 450, paths 150, diagnostics 400, process_env 200,
app_styles 170, misc 130); Qt layer ~3,300 lines (main window 2,000, title bar 300, bootstrap
350, application 150, loading screen 220, tray 180, single instance 150). About 4,800 total plus
tests.
Native gains: startup no longer needs lazy imports or `run_with_ui_pulse`; the splash can stay for
NeMo runtime + model load only (target well under the Python `startup_to_window_s`). No worker
processes, no PyInstaller env juggling, no null-stdio guard, no `os._exit` workaround (but still
avoid unloading the NeMo runtime at exit per CLAUDE.md). Lower idle memory; the metrics writer and
journal become trivially cheap.

## Added by check

### Added by check: Feature inventory

| ID | Feature | Exact behavior | Source | Disposition |
|---|---|---|---|---|
| A01-68 | CLI dispatch is first-argument only | Modes match on `argv[1]` (`--help`/`-h`, `--diagnostics`, `--api`, `--channel`, `--macos-update-helper` on macOS) or on the whole tail (`--version` and `--self-test` only when they are the sole argument; `--workflow-smoke` takes at most one more). Anything unrecognised (also `--version extra`) is ignored and the GUI launches. `--diagnostics` uses argparse: one required `output` path; a missing path prints usage and exits 2; an existing file raises `FileExistsError` (traceback, nonzero) rather than a friendly message. | `main.py:159-215`, `diagnostics.py:main` | port (a friendly error for an existing file is an allowed gain) |
| A01-69 | `--workflow-smoke` report | Writes the report as indented JSON UTF-8 to the given path, exit 0 with no "passed" line; more than one extra arg exits with the usage text. | `main.py:203-212` | port with a16 |
| A01-70 | Update gate skip list | The early update gate does nothing for `--isolated-worker`, `--self-test`, `--workflow-smoke`. | `main.py:265-269` | port with a16 |
| A01-71 | Startup error presentation | Always `OpenWhisper: {message}` on stderr; a modal critical box titled "OpenWhisper" only in frozen builds (swallowing Qt failure). | `main.py:352-364` | port (show the box in every installed build) |
| A01-72 | Restart outcome codes | After a restart request: spawn ok -> exit 0, spawn failure -> message and exit 1; `logging.shutdown()` runs first; the app mutex is released before spawning so the child cannot lose the race. | `main.py:387-416` | port |
| A01-73 | Startup log lines | Three `=`*60 rules around "Starting OpenWhisper"; profiler summary; "Application initialization complete"; "Starting event loop"; on failure "Application startup failed" with traceback then re-raise. | `bootstrap.py:269-271, 384-394` | port |
| A01-74 | Instance release order | `release_ui_instance()` runs in `finally` before `_shut_down`, so a new launch can take over while cleanup is still running; MCP runtime stops (waiting) first in `_shut_down`, each cleanup step failure logged ("Failed to cleanup loading screen", "Failed to cleanup controllers") and never aborts the rest. | `bootstrap.py:395-453` | port |
| A01-75 | Update acknowledgement log lines | "Update readiness failed: {error}" and "Could not acknowledge the update transaction" at ERROR, each followed by `exit(1)`. | `bootstrap.py:419-428` | port with a16 |
| A01-76 | Keyring warning | If `keyring_did_not_respond()`, log WARNING and show the same text as ambient status after the window is up. | `bootstrap.py:341-343` | port with a10 |
| A01-77 | Startup order contract | Single-instance check happens after logging but before any Qt object; `guided_setup.initialize`, then the two settings migrations, run before `UIController`; the splash is destroyed before `show_main_window()`; activation callback attached right after; MCP `configure_controls`/`restore` runs last, after the guided-setup offer. | `bootstrap.py:259-374` | port |
| A01-78 | Diagnostics report engine block | `engine` has `selected_model`, `whisper_model`, `whisper_device`, `whisper_compute_type` (each the value only when in its allowlist, else the literal `default_or_custom`) plus `local_models` (sorted unique catalog ids from `local_asr_models`); `packages` maps a fixed name list to version or null. Output is `json.dump(indent=2)` plus a trailing newline. | `diagnostics.py:255-306` | port (engine block becomes Parakeet runtime/model identifiers) |
| A01-79 | Dropped Python-only bits | `warnings.filterwarnings("ignore", message="pkg_resources...")`, `check_multiprocessing` in `--self-test`, the module import list (`av`, `ctranslate2`, `onnxruntime`, `lxml`, `openpyxl`, `pypdf`, `uvicorn`, keyring/pynput backends, macOS `objc` frameworks). The C++ self-test should instead check Qt plugins (SVG, image formats, platform plugin), TLS backend, and bundled assets. | `main.py:18-88` | drop / adapt |
