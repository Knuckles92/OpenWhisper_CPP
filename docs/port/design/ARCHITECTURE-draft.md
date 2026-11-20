# FioriVoce parity port: architecture (merged draft)

This is the architecture for porting OpenWhisper 2.8.92 (Python 3.12 and PyQt6, at
`D:\coding\whisper_local`) to FioriVoce (C++20 and Qt 6 Widgets) with full feature parity. Work
packages are built from it by worker agents who cannot ask questions. Every name it gives is
therefore binding: when it names a target, a directory, a class or a header, a work package uses
exactly that name. Section 13 lists every decision that was contested, the options, and the reason
for the choice.

Inputs: the 34 area maps in `docs/port/map` and `docs/port/map-summaries.json` (about 205k
estimated C++ lines), `docs/port/areas.json`, the Python source, the FioriVoce tree at `4fbe53d`,
the vcpkg ports at baseline `4702e381`, and the owner's decisions and working assumptions. Where a
working assumption decides the design, the text says so and section 12 lists what a different
answer would change.

---

## 1. Principles

1. **Copy behaviour, not shape.** Strings, timings, file formats, wire bytes, object names and
   pixels match OpenWhisper. Module boundaries, threads and processes are designed for C++.
   *Why:* users, tests and screenshots judge parity; Python's GIL, worker processes and lazy
   imports are not behaviour.
2. **Qt stops at the widget layer.** Everything below the widgets, including the a04 runtimes and
   the a03 state machines, is plain C++20 with an injected executor, clock and I/O. *Why:* most of
   the code then builds and tests with a compiler alone, also on Linux, and the most test-pinned
   logic runs deterministically without an event loop.
3. **One directory, one target, one owner per wave.** Every `src/<dir>` builds exactly one
   library `fv_<dir>`, and within a wave exactly one work package owns each directory. *Why:* two
   branches can only conflict where they touch the same file, so disjoint ownership makes clean
   merges a property of the plan.
4. **No shared file is edited after M0.** CMake discovers directories, the app discovers features
   through a generated registry, settings keys are generated once, and the theme is copied whole.
   *Why:* the usual merge hotspots (top-level CMake, composition root, key table, stylesheet)
   disappear instead of being managed.
5. **Interfaces before implementations.** The cross-cutting headers of section 3 land in M0, with
   fakes. A package codes against the header and tests against the fake. *Why:* a worker never
   waits for, or reimplements, a neighbour's service.
6. **Python semantics live in one library.** Half-to-even rounding, code-point lengths, `casefold`,
   Unicode `\w`, `json.dumps`, `repr(float)`, `int()`, `difflib` and `fromisoformat` are
   implemented once in `fv_pyport`. *Why:* the a33 replay already caught `round(62.5)`; forty
   workers must not each rediscover it.
7. **Python is the oracle, replayed as data.** Pure functions are pinned by golden vectors made by
   running OpenWhisper; widgets by tree dumps and offscreen screenshots compared pixel for pixel.
   *Why:* "ported faithfully" becomes a test a worker runs, not a judgement a reviewer makes.
8. **One owner thread per scarce resource.** The NeMo runtime, each audio device, the keyboard
   hook and the GUI each have one owning thread; everything else posts to it. *Why:* it satisfies
   CLAUDE.md's one-thread rule by construction and replaces Python's lock webs with explicit order.
9. **Formats are OpenWhisper's, names are FioriVoce's.** Settings keys, database schema, backup
   layout and the remote-engine wire stay OpenWhisper's; product name, data folder, credential
   service and environment variables come from one `AppIdentity`. *Why:* import is a mapping, not
   a migration, and no worker ever chooses a name.
10. **Every milestone runs, and the tree is always green.** A package merges only when the whole
    tree configures, builds and passes from its merge commit. *Why:* wave N+1 must start from a
    tree known to be good.

---

## 2. Libraries and directory layout

### 2.1 Tree

```
CMakeLists.txt            frozen after M0; globs src/*/CMakeLists.txt and tests/*/CMakeLists.txt
CMakePresets.json         frozen after M0
vcpkg.json                frozen after M0 (every dependency added at once)
cmake/                    FioriVoceModules.cmake (fv_module, fv_test, fv_feature, fv_embed,
                          registry generation), FioriVoceDeps.cmake (every find_package),
                          FioriVoceWarnings.cmake, FioriVoceQt.cmake, pch/{core.h,qt.h}
src/<dir>/                one target fv_<dir>; public headers at the top level
src/<dir>/detail/         private headers and sources; other libraries never include them
src/<dir>/win/            Windows-only sources (compiled when WIN32)
src/<dir>/posix/          stubs that throw "not supported on this platform" (compiled otherwise)
src/<dir>/testing/        fakes for the interfaces this directory defines
src/<dir>/resources/      qrc content (ui_* directories only)
src/<dir>/data/           JSON tables generated from the Python source, embedded with fv_embed
src/app/                  fiorivoce.exe: main.cpp only
src/cli/                  fiorivoce-cli.exe (Qt-free)
src/updater/              fiorivoce-updater.exe (M10)
tests/<dir>/              fv_<dir>_tests (Catch2), CTest label <dir>, PARITY.json
tests/support/            fv_testkit, fv_golden, fv_screenshot
tests/golden/<python.module>.json                 committed golden vectors
tests/reference/<widget>/<variant>.{png,json}     committed screenshots and tree dumps
tools/parity/             harness moved from out/parity-scratch (section 9)
tools/gen/                generators reading the Python source (settings keys, Unicode tables,
                          keyboard names, catalogs, prompts)
tools/lint/               check_layering.py, check_ownership.py, check_qss_names.py,
                          check_brand.py, check_style_props.py
webui/                    vendored React dashboard source plus committed dist/
third_party/nemo_speech/  vendored C ABI header (unchanged)
third_party/onnxruntime/  vendored onnxruntime_c_api.h only (MIT)
third_party/pdfium/       vendored PDFium public headers only (fpdfview.h, fpdf_text.h)
```

Includes are rooted at `src/`, as today: `#include "history/manager.h"`. Namespace `fv`
everywhere, with one nested namespace per library where names would collide (`fv::net`,
`fv::speech`, `fv::meeting`). `tools/lint/check_layering.py` reads each module's declared `DEPS`
and fails CTest (`lint.layering`) when a file includes a header from outside its closure or
another library's `detail/`. Rejected: nesting by layer (`src/core/settings`), because it
lengthens every include and the module `KIND` already enforces the layer.

### 2.2 Kinds and what they may link

| Kind | Qt | Third-party | May depend on |
|---|---|---|---|
| `core` | none | only the ports named in its row of 2.3 | `core` |
| `qt` | Core, Gui (Widgets only in `appkit`) | as listed | `core`, `qt` |
| `ui` | Core, Gui, Widgets, Svg, SvgWidgets; Network only in `ui_shell` (QLocalServer) | none | anything but `exe` |
| `feature` | as `ui`, plus a registration function collected by the generated registry | none | anything but `exe` |
| `exe` | as needed | as needed | anything |

`core` libraries must build on Linux with GCC: only `win/` sources are Windows-only, and each has
a `posix/` stub. `fv_engine` keeps its CLAUDE.md rules. Qt Sql, Concurrent, Qml/Quick, OpenGL and
PrintSupport are not used; Network is used for QLocalServer only.

### 2.3 Targets

Core (Qt-free), in dependency order:

| Target | Dir | Third-party | Depends on | Holds |
|---|---|---|---|---|
| `fv_engine` | src/engine | NeMo by path | — | unchanged: Recognizer, NemoApi, SharedLibrary, wav |
| `fv_base` | src/base | nlohmann-json, OpenSSL::Crypto | — | identity, paths, env, clock, executors, cancel, `Signal`, log facade, atomic file write, data lease, hashes, ids, desktop session |
| `fv_pyport` | src/pyport | pcre2 | base | Python semantics (principle 6), generated Unicode 15.0 tables |
| `fv_log` | src/log | dbghelp (win) | base | rotating file sink, failure journal, metrics, diagnostics report, startup profiler, crash handler |
| `fv_settings` | src/settings | — | base, pyport | `Setting<T>` kinds, generated keys, `SettingsStore`, enums, shared resolvers |
| `fv_credentials` | src/credentials | advapi32 (win) | base | `CredentialStore`, WinCred backend, memory backend, `.env`, resolution order |
| `fv_sql` | src/sql | sqlite3[fts5,json1] | base | RAII connection, statements, transactions, online backup, functions |
| `fv_db` | src/db | — | sql, pyport | schema v18, migrations v6 to v18, open and repair, `fv_*` tables |
| `fv_http` | src/http | curl | base | `http::Client` on libcurl, SSE parser, multipart, error classes, Windows proxy reader |
| `fv_net` | src/net | boost-beast, boost-asio, openssl | base | `Reactor`, `HttpServer`, WebSocket server and client, TLS identity, UDP discovery socket, LAN address classes |
| `fv_process` | src/process | wil | base | `ProcessLauncher` (job objects, no window, pipes, tree kill, disown), `ProcessEnv`, `Shell` |
| `fv_archive` | src/archive | minizip-ng (zlib only), zlib | base | streaming Zip64 read and write, tar.gz reader, safe extraction |
| `fv_download` | src/download | — | base, http | resumable verified download (`.part`, Range, SHA-256, free space, error texts) |
| `fv_dsp` | src/dsp | pocketfft | base | `fft_resample`, `WindowSplitter`, `split_point`, level dB, Kaldi fbank |
| `fv_audio` | src/audio | portaudio, ffmpeg (avformat, avcodec, swresample), wil | base, dsp | `audio::Backend` (PortAudio capture and playback, WASAPI loopback), device ladder, `PcmTape`, `Resampler16k` (swresample), `AudioFileDecoder`, WAV writer and tags, `Player` |
| `fv_recorder` | src/recorder | — | audio, dsp, settings | `Recorder` (SPSC ring, post-roll gate, pause, fallback and switch), `RecordingJournal` and recovery |
| `fv_models` | src/models | — | download, archive, engine, settings | component catalog, component store, speech-model cache, HF consent policy, NVML probe, runtime resolver, OpenWhisper install discovery and adoption |
| `fv_appcore` | src/appcore | — | base, pyport, settings | a03 core: `OverlayState`, `ActivityState` and gates, readiness messages, `EngineReloadMachine`, `PreviewThrottle`, `CancelRouter`, `ShutdownPlan`, lock order |
| `fv_speech` | src/speech | — | engine, dsp, http, settings | `SpeechService`, `ParakeetBackend`, `CloudSpeechBackend`, `WindowPreview`, `IncrementalSession`, languages |
| `fv_hotkeys` | src/hotkeys | user32 | base, pyport, settings | grammar, generated `keyboard` name table, matcher, hold machine, conflicts, LL hooks |
| `fv_focus` | src/focus | wil (UIA) | base, pyport, settings | `FocusSnapshot`, app catalog, `join_with_context`, capture service, `SyntheticKeys`, `ClipboardPort` interface |
| `fv_uimath` | src/uimath | — | base, pyport | Qt-free UI maths: fuzzy match, overlay geometry, loudness, easing, ribbon, release-notes model, list reconcile, reflow sizes |
| `fv_text` | src/text | — | settings, pyport | a12 pure core: dictionary, vocabulary, snippets, cleanup prompts and profiles, transforms, rewrite, endpoint tables, recognition context, app styles |
| `fv_llm` | src/llm | — | http, text, credentials | text generation over chat, Responses, Anthropic and Google protocols, streaming tool calls, retry, model lists, key checks, local server probes |
| `fv_agents` | src/agents | — | process, settings | installed agent discovery (Claude Code, Codex, OpenCode), headless drivers, ACP client, `run_reading_task` |
| `fv_history` | src/history | — | db, settings | history manager, retention, versions, export, calendar index, format utils, titles, dictation stats |
| `fv_dictation` | src/dictation | — | appcore, speech, recorder, hotkeys, focus, history, text (+llm from M4) | a04 job slot and runtimes (transcription, command, hotkey, streaming), a05 pipeline, uploads and batch types, scratchpad rules |
| `fv_backup` | src/backup | — | db, archive, settings, credentials, models | backup create, inspect, stage, apply, journal, rollback, schedule, OpenWhisper import |
| `fv_update` | src/update | — | download, process, settings | release feed, trust rules, schedule, setup-exe hand-off |
| `fv_automation` | src/automation | — | net, http, process, llm, agents, credentials | a19 actions, routing, event bus, webhooks, programs, filters, live API routes, channel bridge |
| `fv_agentapi` | src/agentapi | — | history, net, automation | a20 History REST, MCP JSON-RPC server and tools, MCP events |
| `fv_walkthrough` | src/walkthrough | — | speech, agents, llm, settings | a22 core: board, sections, manifest, library, write-up prompts, styles |
| `fv_meeting_state` | src/meeting_state | — | db, text | a28 schema, ops, store, corrections, repository, exports |
| `fv_diarize` | src/diarize | ONNX Runtime by path | dsp, base | ORT C API loader, speaker embedder, online diarizer, assignment |
| `fv_meeting_capture` | src/meeting_capture | — | audio, recorder, speech, meeting_state | a25 capture session, watchdog, spool, ASR worker, offline pass, playback |
| `fv_meeting_agent` | src/meeting_agent | — | meeting_state, llm, agents, net | a26 prompts, scheduler, policy, evidence; a27 native agent loop; loopback MCP |
| `fv_meeting_intel` | src/meeting_intel | pugixml, PDFium by path | meeting_state, llm, archive | a29 reports, review, citations, live signals, recall, knowledge-folder search |
| `fv_meeting` | src/meeting | — | meeting_capture, diarize, meeting_state, meeting_agent, meeting_intel | a24 engine, end run, refinalize, recovery |
| `fv_meeting_web` | src/meeting_web | — | net, meeting_state | a30 dashboard API, WS hub, static files |
| `fv_remote` | src/remote | — | net, speech, history, meeting_state, credentials | a17 host, client, pairing, discovery, records, `RemoteSpeechBackend`, `remote/wire.h` |

Qt, non-widget (kind `qt`):

| Target | Dir | Qt | Depends on | Holds |
|---|---|---|---|---|
| `fv_appkit` | src/appkit | Core, Gui, Widgets | base, settings | `QtUiExecutor`, `qt::post`, `TokenGate`, `AppContext`, `Feature`, `FeatureRegistry`, `order.h`, `destinations.h` |
| `fv_clipboard` | src/clipboard | Gui | focus, appkit | `TemporaryClipboard` (implements `ClipboardPort`) |
| `fv_documents` | src/documents | Gui | archive, walkthrough; pugixml | PDF composer and pagination (QPdfWriter), Markdown composer, DOCX writer |

Widgets (kind `ui`, or `feature` where marked):

| Target | Dir | Holds |
|---|---|---|
| `fv_ui_theme` | src/ui_theme | `Palette`, `Theme`, QSS pipeline, `ThemeManager`, font scale, tooltip style, `style_props.h`, `theme.qss`, Tabler SVGs, icons |
| `fv_ui_widgets` | src/ui_widgets | the shared widgets of a02 |
| `fv_ui_hotkeys` | src/ui_hotkeys | shortcut capture fields, Hotkeys page rows |
| `fv_ui_shell` (feature) | src/ui_shell | `Application`, `Bootstrap`, `SingleInstance`, `LoadingScreen`, `SystemTray`, `MainWindow`, `TitleBar`, `FooterBar`, page fit, compact view |
| `fv_ui_controller` (feature) | src/ui_controller | `UiController`, `OverlayRouter`, `ApplicationController`, notices, about, quit and busy confirms, download progress presenter |
| `fv_ui_overlay` (feature) | src/ui_overlay | `WaveformOverlay`, 11 looks, moments, language menu, `RecordingLookPreview` |
| `fv_ui_record` (feature) | src/ui_record | Quick Record, Upload File, Scratchpad, compact controller, reading window, `MicrophoneCheck`, `CaptureDelay` |
| `fv_ui_history` (feature) | src/ui_history | sidebar, cards, entry dialog, playback, export dialog base, calendar, Stats window |
| `fv_ui_settings` (feature) | src/ui_settings | Settings shell, NavRail, search palette, binder, Home, General, Recording, Appearance, Developer, API keys, Backup page |
| `fv_ui_personalize` (feature) | src/ui_personalize | a13 pages and dialogs |
| `fv_ui_downloads` (feature) | src/ui_downloads | a15 Downloads page, Voice model page, consent and runtime dialogs |
| `fv_ui_setup` (feature) | src/ui_setup | a16 update dialog, notice, guided setup, OpenWhisper import page |
| `fv_ui_automation` (feature) | src/ui_automation | a21 Actions, Hooks, API & MCP pages, AgentPicker |
| `fv_ui_snap` (feature) | src/ui_snap | a22 Snap overlay, toolbar, live drawing, Win32 capture plumbing |
| `fv_ui_walkthrough` (feature) | src/ui_walkthrough | a23 tab, live column, storyboard, library dialog |
| `fv_ui_meeting` (feature) | src/ui_meeting | a31 tab, past meetings, dialogs, After End and Fast judgments pages |
| `fv_ui_remote` (feature) | src/ui_remote | a18 Remote page, pair dialog, Host Mode dashboard |

Executables: `fiorivoce` (src/app, GUI subsystem, links every feature, `--self-test-gui`),
`fiorivoce-cli` (src/cli, console, Qt-free: `--version`, `--diagnostics`, `--self-test`, `--api`,
`--channel`, `transcribe`), `fiorivoce-updater` (src/updater, Qt-free, static CRT, M10). Test
support: `fv_testkit` (temporary data root, `FakeClock`, `ManualExecutor`, offscreen
`QApplication` main for Catch2), `fv_golden` (vector loader and comparators), `fv_screenshot`
(tree compare, pixel compare, diff image).

The current bench in `src/app` (`MainWindow`, `EngineWorker`) moves to `src/ui_shell/dev/` in M0
and is retired by M1.16, when `fv::speech::SpeechService` replaces `EngineWorker`.

### 2.4 Areas to targets

| Area | Qt-free targets | Qt targets |
|---|---|---|
| a01 app shell | base (identity, paths, desktop session), log (logging, crash, diagnostics, profiler), process (`ProcessEnv`), text (app styles) | ui_shell, app, cli |
| a02 UI foundation | uimath | ui_theme, ui_widgets |
| a03 UI controller | appcore | ui_controller |
| a04 runtime services | dictation (transcription, command, hotkey, streaming runtimes), models (model and component runtime core), update (schedule), walkthrough, meeting, remote (their runtimes) | ui_controller (glue) |
| a05 recording/audio | dsp, audio, recorder, speech (preview, incremental, languages), dictation (pipeline), history (stats) | ui_record (MicrophoneCheck, CaptureDelay) |
| a06 hotkeys/paste/focus | hotkeys, focus | clipboard, ui_hotkeys |
| a07 overlays | uimath | ui_overlay |
| a08 record/upload tabs | dictation (batch types, scratchpad rules) | ui_record |
| a09 history/stats | sql, db, history | ui_history |
| a10 settings services | settings, credentials, backup | ui_settings (Backup page) |
| a11 settings dialog | appcore (search ranking, mic priority rules, retention text) | ui_settings |
| a12 text AI/personalize | text, llm | — |
| a13 personalize UI | text (copy builders) | ui_personalize |
| a14 models/components | download, archive, models, speech (backends, windows) | — |
| a15 models/downloads UI | models (queue state machine, consent text) | ui_downloads |
| a16 updater/setup | update, updater exe (M10) | ui_setup |
| a17 remote engine services | net (transport, discovery, LAN address), remote | — |
| a18 remote engine UI | remote (view models) | ui_remote |
| a19 actions/automation | automation, process, cli (`--channel`) | — |
| a20 agent API/MCP | agentapi, agents, process, cli (`--api`) | — |
| a21 actions UI | automation (action copy, setup text) | ui_automation |
| a22 walkthrough services/Snap | walkthrough | documents, ui_snap |
| a23 walkthrough UI | walkthrough (column model, layout maths) | ui_walkthrough |
| a24 meeting engine | meeting | — |
| a25 capture/ASR/diarize | meeting_capture, diarize, dsp (fbank), audio (loopback) | — |
| a26 meeting agent | meeting_agent, agents | — |
| a27 sidecars | meeting_agent (native loop), llm (streaming tool calls) | — |
| a28 state/persist/export | meeting_state | — |
| a29 meeting intelligence | meeting_intel | — |
| a30 meeting web server | meeting_web, net; bundle in `webui/` | — |
| a31 meeting Qt UI | meeting (finalization view model, setup checks) | ui_meeting |

---

## 3. Cross-cutting services and their interfaces

M0.1 writes every header in this section, with the shapes below. The owning package implements
it and may add members, never change these. Each interface ships a fake in `<dir>/testing/`.

### 3.1 Identity and paths (`base/identity.h`, `base/paths.h`)

```cpp
namespace fv {
// Every name that differs between FioriVoce and OpenWhisper. Nothing else in the tree spells
// either product name, except remote/wire.h (protocol constants) and user-visible copy.
struct AppIdentity {
    std::string_view product;             // "FioriVoce"
    std::string_view data_dir_name;       // "FioriVoce"
    std::string_view env_prefix;          // "FIORIVOCE_"  (FIORIVOCE_DATA_DIR, ..._LOG_LEVEL)
    std::string_view settings_file;       // "fiorivoce_settings.json"
    std::string_view database_file;       // "fiorivoce.db"
    std::string_view log_file;            // "fiorivoce.log"
    std::string_view lease_file;          // ".fiorivoce-data.lock"
    std::string_view restore_dir;         // ".fiorivoce-restore"
    std::string_view credential_service;  // "FioriVoce"
    std::string_view backup_format;       // "fiorivoce-backup"
    std::string_view backup_extension;    // ".fvbackup"
    std::string_view instance_key_prefix; // "fiorivoce-ui"
};
const AppIdentity& identity();            // FioriVoce
const AppIdentity& legacy_identity();     // OpenWhisper: import and .owbackup reading only

class EnvReader { public: virtual ~EnvReader() = default;
    virtual std::optional<std::string> get(std::string_view name) const = 0; };

class Paths {
public:
    static Paths resolve(const AppIdentity&, const EnvReader&);   // FIORIVOCE_DATA_DIR wins
    const std::filesystem::path& data_root() const;              // %LOCALAPPDATA%\FioriVoce
    std::filesystem::path local_app_dir() const;
    std::filesystem::path components_root() const;               // <local_app_dir>/components
    std::filesystem::path speech_models_root() const;
    std::filesystem::path user_file(std::string_view name) const;  // data_root()/name
    std::filesystem::path env_file() const;
    std::vector<std::filesystem::path> openwhisper_roots() const;  // import and adoption sources
};
}
```

A source checkout never becomes the data root (OpenWhisper's checkout rule exists for
`python main.py`; deliberate difference). Workers never write a product name, file name or
environment variable as a literal.

### 3.2 Clock, executors, cancellation, signals (`base/clock.h`, `executor.h`, `cancel.h`, `signal.h`)

```cpp
namespace fv {
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::steady_clock::time_point steady() const = 0;
    virtual std::chrono::system_clock::time_point utc() const = 0;
    virtual const std::chrono::time_zone* zone() const = 0;   // tests pin it
};
Clock& system_clock();

using Task = std::function<void()>;
class TimerHandle { public: void cancel(); bool active() const; };  // RAII: cancels on destroy

class Executor {
public:
    virtual ~Executor() = default;
    virtual void post(Task) = 0;
    virtual TimerHandle post_after(std::chrono::milliseconds, Task) = 0;   // QTimer.singleShot
    virtual bool is_current() const = 0;
};
enum class ShutdownMode { CancelQueued, Drain };
class ThreadPool final : public Executor {          // named threads "fv-pool-N"
public:
    ThreadPool(std::string name, int threads);
    void shutdown(std::chrono::milliseconds budget);
};
class Strand final : public Executor {              // serial (or N-wide) queue on a pool
public:
    Strand(ThreadPool&, std::string name, int max_concurrency, ShutdownMode);
    void shutdown(std::chrono::milliseconds budget);
};
void assert_not_gui_thread();                       // every blocking API calls it in debug

class CancelToken {
public:
    bool cancelled() const;
    bool wait_for(std::chrono::milliseconds) const; // true when cancelled
    Subscription on_cancel(Task) const;             // runs once, on the cancelling thread
};
class CancelSource { public: CancelToken token() const; void cancel(); };

// Thread-safe multicast with Qt::AutoConnection semantics: a subscriber whose executor is
// current runs inline, in connection order (Python's synchronous callbacks); any other
// subscriber is posted. Never emit while holding a lock another thread needs.
template <class... Args> class Signal {
public:
    Subscription connect(Executor&, std::function<void(Args...)>);
    void emit(const Args&...) const;
};
}
```

Fakes in `fv_testkit`: `FakeClock` (settable, drives timers), `ManualExecutor` (`run_all()`,
`advance(ms)`, `is_current()` true while running). Python's executors map to strands on one pool:
`jobs` (2 wide, cancel queued), `persistence` (1, drain at quit), `components` (1),
`walkthrough-library` (1), `backup` (1), `meeting` (1), `agent` (1), `io` (4).

### 3.3 Settings store (`settings/setting.h`, `settings/keys.h`, `settings/store.h`)

```cpp
namespace fv {
using Json = nlohmann::ordered_json;      // insertion order and int/float/bool kept

template <class T> class Setting {        // Bool, Int (strict/valid/clamp), Float, Str, Choice,
public:                                   // IntChoice, List, Dict, Optional*, Raw
    constexpr std::string_view key() const;
    T read(const Json& snapshot) const;                   // unset or invalid -> default
    std::optional<T> stored(const Json& snapshot) const;  // never the default
    T default_value() const;
    bool dropped() const;                 // keys of dropped engines: preserved, never read
};
namespace keys {                          // generated: settings/keys.inc from settings.py
#include "settings/keys.inc"              // e.g. inline const BoolSetting minimize_tray{...};
}
class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path file);
    std::shared_ptr<const Json> snapshot() const;          // copy-free
    template <class T> T get(const Setting<T>&) const;
    Json update(const Json& patch, std::span<const std::string_view> remove = {});
    Json mutate(const std::function<void(Json&)>&);        // throws -> nothing written
    Subscription watch(std::vector<std::string_view> keys, Executor&,
                       std::function<void(const Json& changed)>);
    bool is_new_install() const;
};
}
```

Keys keep Python's spelling (`keys::minimize_tray`). `tools/gen/settings_keys.py` parses the
Python declarations (key, kind, default, choices, aliases, clamp, strictness) into `keys.inc` in
M0, with a golden vector of the defaults table and the JUNK battery. Writes are atomic: temporary
file, `FlushFileBuffers`, `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`. Resolvers live with
their consumers; only resolvers used by three or more libraries live in `settings/resolve.h`.

### 3.4 Logging and diagnostics (`base/log.h`, `log/*`)

```cpp
namespace fv::log {
enum class Level { Debug, Info, Warning, Error, Critical };
struct Record { Level level; std::string_view logger; std::string_view message;
                std::string_view exception_type; };
class Sink { public: virtual ~Sink() = default; virtual void write(const Record&) = 0; };
void add_sink(std::shared_ptr<Sink>);
class Logger {                            // name = Python's dotted logger ("services.recorder")
public:
    explicit constexpr Logger(std::string_view name);
    template <class... A> void info(std::format_string<A...>, A&&...) const;   // and the rest
    void error_with(const std::exception&, std::string_view message) const;    // type -> category
};
}
```

The facade is in `fv_base`; the sinks live in `fv_log`: `RotatingFileSink` (format
`%(asctime)s - LEVEL - name - message`, 5 MiB x 3), `FailureJournalSink` (`{utc, component,
category, line}`, never message text, max 50), console sink, `MetricsCapture` (12 allowlisted
names), `DiagnosticsReport`, `StartupProfiler`, `CrashHandler`. One writer thread; callers push
into a bounded queue and never block on stalled storage. Qt messages arrive through
`qInstallMessageHandler` as logger `qt`. Dictated text, selections, prompts, keys and URLs with
queries never reach a log call.

### 3.5 Credentials (`credentials/store.h`)

```cpp
namespace fv {
struct CredentialStatus { bool available; std::string backend_name; std::string reason; };
class CredentialBackend {             // WinCred now; Keychain and libsecret later
public:
    virtual ~CredentialBackend() = default;
    virtual std::optional<std::string> get(std::string_view service, std::string_view name) = 0;
    virtual void set(std::string_view service, std::string_view name, std::string_view value) = 0;
    virtual bool remove(std::string_view service, std::string_view name) = 0;
    virtual CredentialStatus probe() = 0;
};
class CredentialStore {               // caches, serializes backend calls, never logs values
public:
    CredentialStore(std::shared_ptr<CredentialBackend>, const AppIdentity&);
    std::optional<std::string> get(std::string_view name);
    void set(std::string_view name, std::string_view value);
    bool remove(std::string_view name);
    CredentialStatus status();
};
enum class CredentialSource { Store, Environment, DotEnv, None };
struct Resolved { std::optional<std::string> value; CredentialSource source; };
Resolved resolve_credential(CredentialStore&, const EnvReader&, const DotEnv&, std::string_view);
std::string mask_key(std::string_view key);
}
```

The Windows backend uses keyring's WinVault target convention (`<name>@<service>`, falling back
to `<service>`), pinned by a golden test, so the importer reads OpenWhisper's entries with the
same code and `legacy_identity()`.

### 3.6 HTTP client (`http/client.h`)

```cpp
namespace fv::http {
using Headers = std::vector<std::pair<std::string, std::string>>;
struct Request {
    std::string method = "GET", url;
    Headers headers;
    Body body;                                      // bytes, multipart with file parts, or file
    std::chrono::milliseconds connect_timeout{10'000}, idle_timeout{120'000};
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool follow_redirects = false;                  // callers re-validate each hop
    std::optional<std::string> pinned_address;      // CURLOPT_RESOLVE: SSRF guard keeps SNI
    bool bypass_proxy = false;
    std::optional<std::uint64_t> range_from;
    std::size_t max_body_bytes = 64u << 20;
};
struct Response { int status = 0; Headers headers; std::string body; std::string effective_url; };
class BodySink { public: virtual ~BodySink() = default;
    virtual bool write(std::span<const char>) = 0; };   // false aborts (SSE, downloads)
enum class ErrorKind { Timeout, Dns, Connect, Tls, Cancelled, TooLarge, Protocol };
struct Error { ErrorKind kind; std::string message; };
class Client {                                      // blocking; never on the GUI thread
public:
    virtual ~Client() = default;
    virtual std::expected<Response, Error> send(const Request&, const CancelToken&,
                                                BodySink* stream = nullptr) = 0;
};
std::shared_ptr<Client> make_curl_client();         // one share handle, Schannel, OS trust store
}
```

Cancel aborts through curl's transfer callback. The system proxy comes from the Windows registry
the way `urllib.getproxies` reads it, plus `HTTP(S)_PROXY` as httpx's `trust_env` does.

### 3.7 HTTP and WebSocket server, WebSocket client (`net/*`)

```cpp
namespace fv::net {
class Reactor {                       // owns the asio io_context, threads "fv-net-0", "fv-net-1"
public:
    explicit Reactor(int threads = 2);
    Executor& executor();             // handlers run here and never block
    void stop(std::chrono::milliseconds grace);
};
struct HttpRequest { std::string method, path; Query query; Headers headers;
                     std::string body; Peer peer; bool tls; };
class Responder {                     // shared_ptr; may complete later from any thread
public:
    virtual void send(int status, Headers, std::string body) = 0;
    virtual void send_file(std::filesystem::path, std::string content_type,
                           std::optional<ByteRange>) = 0;      // 206 and 416 handled
    virtual std::shared_ptr<EventStream> start_sse(Headers) = 0;
};
struct TlsIdentity { std::string cert_pem, key_pem; bool disable_tickets = false; };
struct Listen { std::string address; std::uint16_t port; std::optional<TlsIdentity> tls; };
class WsConnection {
public:
    virtual void send_text(std::shared_ptr<const std::string>) = 0;   // one buffer, N sockets
    virtual void send_binary(std::vector<std::byte>) = 0;
    virtual void close(std::uint16_t code, std::string reason) = 0;
    virtual std::size_t queued_bytes() const = 0;
};
class HttpServer {
public:
    static std::unique_ptr<HttpServer> create(Reactor&);
    void route(std::string method, std::string pattern,
               std::function<void(const HttpRequest&, std::shared_ptr<Responder>)>);
    void websocket(std::string path, WsHandlers);          // upgrade on the same port
    std::expected<Endpoint, std::string> listen(const Listen&);
    void stop(std::chrono::milliseconds grace);            // aborts streaming after grace
};
std::unique_ptr<WsClientConnection> ws_connect(Reactor&, WsClientOptions);  // SHA-256 pinning
TlsIdentity generate_self_signed_p256(std::string_view common_name);
}
```

Every server (meeting dashboard, MCP, live API and SSE, History REST, remote-engine host, loopback
MCP for agents) is an `HttpServer` on the one `Reactor`. Handlers are pure functions in their own
libraries (`agentapi::history_routes`, `meeting_web::DashboardApi`), tested without sockets. Only
`fv_net` `.cpp` files include Boost.

### 3.8 Database (`sql/db.h`, `db/database.h`)

```cpp
namespace fv::sql {
class Connection {
public:
    static Connection open(const std::filesystem::path&, OpenMode);   // ro (URI), rw, create
    Statement prepare(std::string_view sql);
    void exec(std::string_view sql);
    Transaction begin(TxMode = TxMode::Deferred);   // TxMode::Immediate where Python uses it
    void backup_to(Connection& target, const CancelToken&);
    void create_function(std::string_view name, int args, ScalarFn);
};
}
namespace fv {
class Database {                   // WAL; one writer connection behind a mutex, pooled readers
public:
    static Database open_or_create(const std::filesystem::path&);   // migrates to v18
    template <class F> auto write(F&& fn);   // fn(sql::Connection&) under the writer mutex
    template <class F> auto read(F&& fn);    // fn(sql::Connection&) on this thread's reader
    int schema_version() const;              // 18
};
}
```

`fv_db` owns the whole schema and every migration, so the schema is never a shared edit.
Repositories (`HistoryRepository`, `MeetingRepository`, `StatsRepository`) live in their
libraries and use `db/schema.h` constants. SQLAlchemy queries become the SQL text they emit.

### 3.9 Process launcher (`process/launcher.h`)

```cpp
namespace fv {
struct ProcessSpec {
    std::filesystem::path program;
    std::vector<std::string> args;            // UTF-8, quoted by the launcher
    std::filesystem::path cwd;
    std::map<std::string, std::string> env;   // from ProcessEnv::external_env()
    bool own_tree = true;                     // suspended start, job assigned, then resumed
    bool pipes = false;
    bool disown_on_clean_exit = false;
};
class Process {
public:
    virtual ~Process() = default;
    virtual std::uint32_t pid() const = 0;
    virtual void write_stdin(std::string_view) = 0;
    virtual void on_line(LineSink stdout_sink, LineSink stderr_sink) = 0;  // reader threads
    virtual std::optional<int> wait(std::chrono::milliseconds) = 0;
    virtual void kill_tree() = 0;
};
class ProcessLauncher { public: virtual ~ProcessLauncher() = default;
    virtual std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec&) = 0; };
class Shell { public: virtual ~Shell() = default;
    virtual bool open_url(std::string_view) = 0;
    virtual bool reveal(const std::filesystem::path&) = 0;
    virtual bool launch_via_explorer(const std::filesystem::path&, std::string_view args) = 0; };
}
```

Windows: `CreateProcessW` with `CREATE_SUSPENDED | CREATE_NO_WINDOW`, job assignment before
resume (a20), `CancelSynchronousIo` on pipe reader threads (a19).

### 3.10 Audio input and output (`audio/backend.h`, `audio/pcm_tape.h`)

```cpp
namespace fv::audio {
struct DeviceInfo {
    int index; std::string name;              // PortAudio's names, UTF-8
    std::string host_api;                     // "MME", "Windows WASAPI", ... (PortAudio names)
    int max_input_channels, max_output_channels; double default_rate;
    bool is_default = false;
};
struct DeviceKey { std::string name, hostapi; };   // the saved audio_input_priority shape
struct StreamConfig { int device_index; int rate = 44100; int channels = 1;
                      int frames_per_block = 1024; bool wasapi_auto_convert = true; };
struct BlockStatus { bool overflow = false, underflow = false; };
// Runs on the device thread: no allocation, no locks, no logging, no syscalls.
using CaptureFn = void (*)(void* user, const std::int16_t* frames, std::size_t count, BlockStatus);
class Stream { public: virtual ~Stream() = default; virtual bool active() const = 0;
               virtual void abort() = 0; };
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<DeviceInfo> inputs() = 0;       // never on the GUI thread
    virtual std::vector<DeviceInfo> outputs() = 0;
    virtual std::vector<LoopbackEndpoint> loopbacks() = 0;   // render endpoints (meetings)
    virtual std::expected<void, std::string> check_input(const StreamConfig&) = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string>
        open_capture(const StreamConfig&, CaptureFn, void* user,
                     std::function<void(std::string)> on_lost) = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string>
        open_loopback(const LoopbackEndpoint&, CaptureFn, void* user,
                      std::function<void(std::string)> on_lost) = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string>
        open_playback(const StreamConfig&, PlaybackSource&) = 0;
    virtual bool refresh() = 0;                         // refused while a stream is open (A05-07)
};
std::shared_ptr<Backend> make_portaudio_backend();      // loopback via WASAPI in audio/win/
std::vector<DeviceInfo> ranked_candidates(std::span<const DeviceKey>,
                                          std::span<const DeviceInfo>);   // the a05 ladder

class PcmTape {                       // append-only int16 store in 1 s chunks, one writer
public:
    void append(std::span<const std::int16_t>);         // audio-io thread only
    TapeView view(std::int64_t first, std::int64_t last) const;   // zero-copy, shares chunks
    std::int64_t frames() const;                        // atomic
    int rate() const;
};
class Resampler16k {                  // swresample to s16 mono 16 kHz: the decode-window path
public:
    explicit Resampler16k(int input_rate);
    void push(std::span<const std::int16_t>, std::vector<std::int16_t>& out);
    void finish(std::vector<std::int16_t>& out);
};
}
```

`FakeBackend` replays scripted blocks, device losses and device lists dumped from real PortAudio
installs; the recorder's switch and post-roll tests port against it.

### 3.11 Events between layers and the Qt bridge (`appkit/qt_bridge.h`)

Three mechanisms, each with one job:

- **Core to core, core to runtime:** `fv::Signal<...>` members on the producing class (3.2).
  Python's assignable `on_*` callbacks in Qt-free code become `Signal`s.
- **Widget to widget, widget to its feature's presenter:** ordinary Qt signals.
- **Core to Qt:** the helpers below. They replace OpenWhisper's relay-with-token classes (a09,
  a11, a13, a18, a21, a23, a31) and the crash class those were fixing.

```cpp
namespace fv::qt {
class QtUiExecutor final : public Executor {     // GUI-thread executor; post_after uses QTimer
public:                                          // with the same Qt::TimerType as Python
    explicit QtUiExecutor(QObject* context);
};
// Runs fn on context's thread if context is still alive; otherwise drops it.
template <class Fn> void post(QObject* context, Fn&& fn);
// "Latest request wins": a result is delivered only if its token is still the newest.
class TokenGate {
public:
    std::uint64_t next();
    template <class Fn> void deliver(QObject* context, std::uint64_t token, Fn&& fn);
};
QString toQString(std::string_view utf8);  std::string toUtf8(const QString&);
}
```

The product event bus of a19 (`automation::EventBus`: sequence numbers, replay ring, gap
reporting, SSE, webhooks) is a feature, not plumbing; lower layers publish through the
`EventSink` interface in `base/events.h`, so they do not depend on `fv_automation`.

### 3.12 Application context and features (`appkit/app_context.h`, `appkit/feature.h`)

```cpp
namespace fv {
class AppContext {                                  // built in Bootstrap, outlives every window
public:
    const AppIdentity& identity(); const Paths& paths(); Clock& clock();
    SettingsStore& settings(); CredentialStore& credentials(); Database& db();
    http::Client& http(); net::Reactor& net(); audio::Backend& audio();
    speech::SpeechService& speech(); EventSink& events();
    Executor& ui(); Executor& jobs(); Executor& persistence(); Executor& io();
    ScreenRegistry& screens(); MenuRegistry& menus(); SettingsPageRegistry& settingsPages();
    ShutdownPlan& shutdown(); CancelRouter& cancel(); ActivityState& activity();
    template <class T> T& service();                // feature-provided; throws naming T
    template <class T> void provide(std::shared_ptr<T>);
};
class Feature {
public:
    virtual ~Feature() = default;
    virtual std::string_view id() const = 0;
    virtual std::vector<std::string_view> requires() const { return {}; }
    virtual void provide(AppContext&) {}            // phase 1: construct services
    virtual void install(AppContext&) = 0;          // phase 2: wire, register UI
};
}
```

Each feature library declares `fv_feature(ID meeting CLASS fv::MeetingFeature)` in its
`CMakeLists.txt`. CMake collects the declarations into a global property and writes
`generated/registry.cpp`, which constructs the features in `requires()` order, then by id. No
static registrars and no whole-archive linking. Tab, menu, tray and Settings page order comes from
constants in `appkit/order.h` (Quick Record 100, Upload 200, Walkthrough 300, Meeting 400; side
panels; File, View and Help items; tray items) and a11's destination ids in
`appkit/destinations.h`, both written once in M0.

---

## 4. Threading model

| Thread | Name | Owner | Runs |
|---|---|---|---|
| GUI | `main` | Qt | widgets, presenters, every "UI actor" core object (runtimes, controllers, state machines) through `ctx.ui()`; never blocks |
| Speech | `fv-speech` | `SpeechService` | every NeMo call: runtime load, model load, warm-up, decode; one at a time by construction |
| Device callbacks | PortAudio's | `audio::Backend` | `CaptureFn` only: copy into a preallocated SPSC ring, update atomic peak and RMS |
| Loopback | `fv-loopback` | WASAPI loopback stream | event-driven `IAudioCaptureClient` reads into an SPSC ring (meetings only) |
| Audio I/O | `fv-audio-io` | `Recorder` | drains rings into `PcmTape`s, journal and spool writes with 1 s checkpoints, device health (100 ms) |
| Recorder watch | `fv-recorder-watch` | `Recorder` (per recording) | post-roll wait, stream close, metrics |
| Hook | `fv-hotkey-hook` | `KeyboardHook` | `WH_KEYBOARD_LL` and `WH_MOUSE_LL` message loop; decides suppression inside the callback |
| Dispatch | `fv-hotkey-dispatch` | `HotkeyRuntime` | ordered delivery of matched hotkeys (OpenWhisper's `OrderedDispatcher`) |
| Focus | `fv-focus` | `FocusCapture` | foreground and UIA reads under a deadline; a hung read is abandoned and the thread replaced (at most 2) |
| Network | `fv-net-0`, `fv-net-1` | `net::Reactor` | every server and WebSocket client; handlers never block |
| Pool | `fv-pool-0..5` | `ThreadPool` | the strands of 3.2: blocking HTTP, file and DB I/O, cleanup, component work |
| Pipes | `fv-pipe-<pid>-out/err` | `Process` | child stdout and stderr readers |
| Meeting | `fv-meeting-asr`, `fv-meeting-diarize`, `fv-meeting-watchdog` | meeting libraries | ASR queue (submits to `fv-speech` and waits), ORT inference, capture health |
| Log | `fv-log` | `fv_log` | log file, failure journal, metrics file |
| Crash | `fv-crash-watch` | `CrashHandler` | pre-created thread that writes the minidump |

At idle that is about twelve threads, against OpenWhisper's interpreter threads, five executors
and a worker process per engine.

Rules for crossing threads (a worker applies them mechanically):

1. **Every class declares its affinity** in a header comment: `// Affinity: UI actor`,
   `thread-safe`, or `owned thread <name>`. A UI actor's public methods run only on `ctx.ui()`;
   tests drive them on a `ManualExecutor`.
2. **The GUI thread never waits.** Blocking APIs (`http::Client::send`, `Database::read/write`,
   `SpeechService::run`, `future.get`) call `assert_not_gui_thread()`. The only blocking allowed
   on the GUI thread is a mutex held for microseconds by design.
3. **Results reach widgets only through `qt::post`, `TokenGate` or queued signals of GUI-thread
   QObjects.** A worker never holds or captures a widget pointer.
4. **Realtime callbacks** (device callbacks, the LL hook callback) touch only preallocated memory
   and atomics. `testkit::RealtimeGuard` fails a debug test when such a callback allocates. Ring
   overflow maps to OpenWhisper's 512-block semantics ("Recording storage cannot keep up").
5. **Only `fv-speech` touches a `Recognizer`**, and it reads `last_error` inside the same job.
   This replaces CLAUDE.md's "only through EngineWorker's queued signals"; M0.1 updates CLAUDE.md.
6. **No lock is held while calling out**: no `Signal::emit`, callback, slow log sink or backend
   call under a mutex another thread needs.
7. **Lock order**, where several locks are needed, is written in `appcore/locks.h`: job slot,
   capture, meeting runtime, meeting store, database writer. Multi-lock acquisitions use
   `std::scoped_lock`.
8. **Subscribers run on their executor and only enqueue** (settings watchers, meeting store
   fan-out, event bus).
9. **Every wait takes a `CancelToken`**; shutdown cancels the root source first.
10. **Exceptions never cross threads raw**: they travel in results; Qt slots, event filters and
    callbacks from C code catch everything and log the type.

Shutdown keeps a03's order: stop accepting work, cancel the batch download and Auto Mode, stop
hooks and capture, stop servers (1 s grace for streaming responses), cancel queued speech jobs and
let the current window finish, drain `persistence`, close the database, flush the log. Recognizers
are destroyed; runtime libraries are deliberately never freed. After an updater hand-off the
process ends with `std::_Exit` so no static destructor touches the runtime.

---

## 5. Speech backends

### 5.1 One interface, three implementations

```cpp
namespace fv::speech {
struct AudioClip {                       // mono float at 16 kHz unless rate says otherwise
    std::shared_ptr<const std::vector<float>> samples;
    std::size_t begin = 0, end = 0;
    int rate = 16000;
    double offset_s = 0.0;               // where begin sits in the recording (source time)
};
enum class Lane { Final, RemoteClient, Meeting, Incremental, Preview, Background, Warmup };
struct DecodeJob {
    std::variant<AudioClip, std::filesystem::path> input;   // path only for cloud uploads
    RecognitionOptions options;          // language, speech contexts (outlive the request)
    Lane lane = Lane::Final;
    std::string coalesce_key;            // a newer job with this key replaces a queued one
    CancelToken cancel;
};
struct DecodeResult { Transcript transcript; double decode_s = 0; std::uint64_t generation = 0; };
struct Capabilities { bool windows = true; bool live_preview = true; bool accepts_files = false;
                      double max_window_s = 30.0; std::vector<std::string> languages; };
enum class ReadyState { Ready, Loading, Warming, ModelMissing, RuntimeMissing, NeedsKey,
                        Offline, Failed };
struct Readiness { ReadyState state; std::string device_info; std::string message; };

class Backend {                          // load/decode/release run on the backend's lane thread
public:
    virtual ~Backend() = default;
    virtual std::string_view id() const = 0;          // "parakeet", "api", "remote"
    virtual Capabilities capabilities() const = 0;
    virtual Readiness load(const CancelToken&) = 0;
    virtual DecodeResult decode(const DecodeJob&) = 0; // throws EngineError (full sentence)
    virtual void release() = 0;
};

class SpeechService {                    // thread-safe facade; owns the "fv-speech" thread
public:
    SpeechService(BackendFactory, Executor& notify);
    JobHandle submit(DecodeJob, Executor& reply_on,
                     std::function<void(std::expected<DecodeResult, EngineError>)>);
    DecodeResult run(DecodeJob);         // blocking, for worker threads; asserts not GUI
    void select(Slot, BackendConfig);    // debounced reload lives in appcore::EngineReloadMachine
    EngineLease lease(std::string_view holder);   // blocks reloads while held
    void release_local(Slot);  void restore_local(Slot);   // meeting model lease, 5.4
    Readiness readiness(Slot) const;
    Signal<Slot, Readiness> readiness_changed;
    Signal<bool> busy_changed;
};
}
```

- **`ParakeetBackend`** (`fv_speech`) wraps the existing `fv::Recognizer` and runs on
  `fv-speech`. Input is 16 kHz float made exactly as Python makes it (5.2), not the capture rate,
  so a C++ decode and a Python decode of the same WAV on the same runtime and model receive the
  same samples and can be compared.
- **`CloudSpeechBackend`** (`fv_speech`; OpenAI, Groq, own OpenAI-compatible server) runs on a
  pool strand 3 wide (a14's `CHUNK_UPLOAD_CONCURRENCY`), uploads multipart through
  `http::Client` from the saved WAV, splits files over 23 MB with a05's silence-aware splitter,
  aborts the reply on cancel and does not retry, as Python.
- **`RemoteSpeechBackend`** (`fv_remote`) sends the same 16 kHz windows as float32 frames on the
  OpenWhisper wire, keeps the busy-retry and reconnect rules, and abandons a request on cancel
  while keeping the connection. It mirrors Python, where `RemoteSpeechBackend` subclasses the
  local backend and receives the same windows.

The a03 backend table is a `BackendFactory` keyed by `selected_model`: `parakeet`, `api`,
`remote`. Nemotron and every other dropped engine has no key.

### 5.2 Audio into the engine: two resamplers, as in Python

OpenWhisper uses two different resamplers, and parity needs both:

- **Decode windows** (final dictation, uploads, incremental dictation, retranscribe, remote) go
  through PyAV's `AudioResampler(format="s16", layout="mono", rate=16000)`, which is FFmpeg's
  swresample, then `WindowSplitter` (30 s maximum, cut after the quietest 0.1 s block past 24 s).
  `local_asr/audio.py` says incremental dictation "relies on matching `windows()` bit for bit".
  FioriVoce uses `audio::Resampler16k` on swresample for this path, never `fft_resample`.
- **Previews, meeting ASR chunks, meeting spool and diarization** go through `fft_resample`
  (numpy's pocketfft, `streaming_transcriber.py`), ported in `fv_dsp` on the vcpkg `pocketfft`
  header.

### 5.3 Scheduling on the speech thread

The queue is ordered by lane, then by submission. The order is the policy:
`Final > RemoteClient > Meeting > Incremental > Preview > Background > Warmup`.

- **Final** always runs next, which is OpenWhisper's "the final pass starts at once" without a
  second engine.
- **Preview** jobs carry `coalesce_key = "preview:<job id>"`, so only the newest window waits and
  preview latency never grows.
- **Warmup** runs only when nothing else is queued.
- **Uploads** are `Final` jobs submitted window by window, so a long upload never holds dictation
  for more than one window.
- **Meeting** chunk decodes use `Meeting`; the offline re-decode at End uses `Background`.
  Dictation is refused during a live meeting by a03's gates anyway, and once capture ends a
  dictation's windows overtake the re-decode.
- **RemoteClient**: a Host Mode server submits its clients' requests here, so the host can
  dictate locally while serving.

### 5.4 Live preview and incremental dictation

`speech::WindowPreview` (a UI actor; port of `StreamingTranscriber`) watches the active
recording's `PcmTape`. Every 3.0 s of new audio it takes a window of 0.75 s overlap plus the new
audio, applies `fft_resample` to 16 kHz as Python does, and submits it on `Preview`. Results join
with the ported `append_preview_text` (bounded seam de-duplication). Stop never decodes the
preview tail; `finalize_preview` decodes the kept tail only if the final transcript comes back
empty, exactly as OpenWhisper's lazy tail. Cloud and remote backends report `live_preview = false`
unless the remote host advertises it, with `preview_unavailable_reason`'s strings.

`speech::IncrementalSession` feeds the tape through its own `Resampler16k` into a
`WindowSplitter` and submits each completed window on `Incremental`. At stop only the remaining
windows go in as `Final`. The CRC of the tape against the saved WAV's data chunk stays as the
safety check before reuse. Invalidation rules (engine reload, mic switch, language or vocabulary
change, cancel, meeting start) are ported from a05.

### 5.5 Cancel

The C ABI has no abort, so a decode is never longer than one window: every input longer than
`max_window_s` is decoded window by window, with the token checked between windows, and queued
windows of a cancelled job are dropped at once. Worst-case cancel latency is one 30 s window.
Cancelled jobs complete with `EngineError("Transcription canceled")`. Results carry the engine
generation captured at submit; anything from an older generation is dropped (a14's generation
protocol, without the worker process).

### 5.6 Sharing one Recognizer

There is one `Recognizer` per (runtime, model, device), owned by the service, with at most two
resident. Dictation, uploads, rule dictation, walkthrough sections, meetings and the remote host
all submit to the same `SpeechService`.

- **Same model for meeting and dictation** (the default): the meeting takes an `EngineLease`, so
  no reload starts under it, and nothing is unloaded. OpenWhisper's release-and-reload after every
  meeting disappears; the status "Released while Meeting Mode runs" is not shown, because nothing
  was released. This is a deliberate difference.
- **`meeting_speech_separate` with a different model**: the service loads the meeting model and
  releases the dictation model for the meeting's duration through `release_local` and
  `restore_local`. That keeps OpenWhisper's memory profile and its messages.

### 5.7 Runtime and model lifetime

- **One runtime per process.** The CPU and CUDA builds of NeMo-Speech.cpp ship dependencies with
  the same names (`ggml.dll`, `ggml-base.dll`, `ggml-cpu.dll`, `llama.dll`, checked in
  OpenWhisper's `asr-nvidia-cuda` component). The Windows loader reuses an already loaded module
  of the same name, so a second runtime would bind to the first one's ggml. The first successful
  load fixes the runtime for the process.
- **Switching the runtime** (CPU build to CUDA build, or an update or removal of the loaded
  runtime's component) is saved as `fv_speech_runtime` and applied at the next start, with
  "Restart FioriVoce to switch to the GPU runtime." The component store refuses to touch the
  loaded runtime's directory and stages the change; a14's startup recovery settles it before any
  load. The CPU-or-GPU *device* choice inside the CUDA build needs no restart.
- **Models and devices** within a runtime: the `Recognizer` is destroyed and recreated on
  `fv-speech` (destroy is safe; only `FreeLibrary` is not). The 400 ms debounce (re-armed while
  recording or transcribing), warm-up after load and the reload state machine live in
  `appcore::EngineReloadMachine`, golden-trace tested against `test_engine_warmup.py`. Warm-up is
  one short non-silent decode, never queued behind another job.
- **Auto device**: `code: "unsupported_gpu"` still drives the CPU fallback.
- **Crash containment replaces process isolation.** The unhandled-exception filter records the
  faulting module. If it is in the NeMo or ggml DLLs, the next start shows "The speech engine
  crashed" and offers the CPU runtime, and the minidump goes into the diagnostics export.

---

## 6. Third-party choices

All are added to `vcpkg.json` in M0.1 against the pinned baseline; workers do not add
dependencies.

| Need | Choice | Runner-up | Why |
|---|---|---|---|
| Microphone capture and playback | **PortAudio 19.7** (vcpkg) | miniaudio 0.11.26 | sounddevice is PortAudio, so device names, host-API names, the per-host-API rows of the Microphones dialog, MME 31-character truncation, the Sound Mapper fallback, WASAPI auto-convert and `check_input_settings` behave as in OpenWhisper, and imported `{name, hostapi}` keys match without a rewrite. miniaudio would show one WASAPI row per device and need a lossy key migration. |
| WASAPI loopback (meetings) | **Native WASAPI** `IAudioClient` with `AUDCLNT_STREAMFLAGS_LOOPBACK`, through wil, in `audio/win/` | miniaudio loopback device | vcpkg's PortAudio 19.7 has no `[Loopback]` devices, and OpenWhisper's own Windows fallback (`soundcard`) is exactly this API. About 400 lines. |
| HTTP and WebSocket server, WebSocket client | **Boost.Beast and Asio 1.92** with OpenSSL | cpp-httplib plus IXWebSocket | One stack covers Range and 206, SSE, WebSocket upgrade on the dashboard port, a TLS WebSocket host with pinned clients, and `SSL_CTX_set_num_tickets(0)`. Two libraries would bring two threading models and no WebSocket on the HTTP port. Compile cost is fenced inside `fv_net`. |
| HTTP client | **libcurl 8.23** (Schannel) | QNetworkAccessManager | The callers are Qt-free worker code that blocks, as httpx did; curl keeps them Qt-free and testable, uses the OS trust store, resumes with Range, streams bodies, and pins an address with `CURLOPT_RESOLVE` for the webhook SSRF guard. QNAM would put Qt and an event-loop thread under every core library. |
| TLS and certificates | **OpenSSL 3.6** | Schannel/CNG | Self-signed P-256 generation and pinning for a17, and the TLS context for Beast. |
| JSON | **nlohmann-json 3.12 `ordered_json`** | QJsonDocument | Insertion order and the int/float distinction are load-bearing (a10, a26, a28); QJsonObject sorts keys and merges int with double. |
| SQLite | **sqlite3 3.54 C API** (`fts5`, `json1`) | QtSql QSQLITE | Qt-free cores, online backup API, custom functions, explicit `BEGIN IMMEDIATE`, FTS5 certainty. |
| Zip | **minizip-ng 4.1** with default features off, `zlib` only | miniz 3.1.2 | Streaming Zip64 read and write with progress and cancel for multi-GB backups, OOXML read and write, component zips; zlib is then also the gzip for tar.gz runtimes. |
| .docx writing | **minizip-ng plus pugixml 1.16**, OOXML parts templated from python-docx output | QXmlStreamWriter | The same XML library reads DOCX, PPTX and XLSX for the Qt-free knowledge-folder search; golden XML comes from python-docx. |
| PDF writing | **QPdfWriter and QTextDocument** (QtGui) | libharu | a22's pagination is defined in terms of QTextDocument layout; its workarounds port line by line. |
| PDF text extraction (knowledge folder) | **PDFium** prebuilt DLL (pdfium-binaries), headers vendored, loaded by path as a component on first use | own extractor | pypdf decrypts empty-password PDFs and reads xref and object streams and CID fonts; a 600-line extractor would not. PDFium is not in vcpkg. Without it, PDFs are skipped and logged. |
| Markdown | **QTextDocument::setMarkdown** (Qt's md4c) | md4c 0.6 | OpenWhisper renders through the same reader, so its styling passes port directly. |
| File decoding and decode-window resampling | **FFmpeg 9.0.2** (`avformat`, `avcodec`, `swresample`; default features off; LGPL, shared DLLs) | Media Foundation | PyAV is FFmpeg: the six accepted types (WAV, MP3, M4A, OGG, FLAC, WMA) decode with the same decoders, and swresample gives the 16 kHz windows bit-compatible with Python (5.2). Media Foundation would change both. One slow build, absorbed by the binary cache. |
| FFT and `fft_resample` | **pocketfft** (vcpkg, `pocketfft_hdronly.h`) | kissfft | numpy 2.x uses this C++ pocketfft, so `fft_resample` and fbank stay bit-near Python on arbitrary lengths. |
| Speaker embeddings | **ONNX Runtime** official release DLL at the version Python pins (1.29.0), C API header vendored, loaded by path as a component | vcpkg `onnxruntime` 1.31.1, linked | Same pattern as NeMo: downloaded with the WeSpeaker model on first meeting, never on the build path; the vcpkg port builds ORT, protobuf, abseil and onnx from source in every cold worktree. CPU EP, 1 thread. |
| Credentials | **Windows Credential Manager** (`wincred.h`) | qtkeychain | qtkeychain depends on Qt (vcpkg would build Qt) and loses keyring's target naming and lookup-only reads. |
| Crash reporting | **Own handler**: `SetUnhandledExceptionFilter`, `MiniDumpWriteDump` from a pre-created thread, terminate, purecall, invalid-parameter and SIGABRT hooks, text crash log | crashpad | No handler process or upload server; a local minidump and log match OpenWhisper's `faulthandler` contract. |
| Global hotkeys and input injection | **`WH_KEYBOARD_LL`, `WH_MOUSE_LL` and `SendInput`**; key names generated from Python's `keyboard` library | `RegisterHotKey` | Push-and-hold, suppression and keypad keys need key-up events and per-key suppression. |
| UI Automation, COM | **wil** | raw COM | RAII for COM, handles and errors in `fv_focus` and `audio/win`. |
| Screenshots (Snap) | **`QScreen::grabWindow(0)`** per screen, `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` for our overlays, Win32 window rectangles | Windows.Graphics.Capture (cppwinrt) | What OpenWhisper's Snap does; WGC stays in reserve for HDR displays. |
| Regex and Unicode | **PCRE2 10.49** (UTF and UCP) plus tables generated from Python 3.12's `unicodedata` (Unicode 15.0) | ICU, utf8proc | Python patterns use lookbehind; generated tables match Python's `casefold` and categories exactly, where libraries carry a newer Unicode. |
| Logging | **Own facade and sinks** (about 400 lines) | spdlog 1.17 | The failure journal, metrics and never-block semantics need custom sinks either way. |
| Single instance | **QLockFile and QLocalServer** keyed as a01 specifies | named mutex and pipe | a01 already specifies this port on every platform; QtNetwork is installed. |
| Tests | **Catch2 3.16** for every test target, Qt Test linked only for `QTest::mouseClick` and `QSignalSpy` | Qt Test runner | One runner, one filter syntax and one CTest discovery for every executable. |

Not used: Qt Network beyond QLocalServer, Qt Sql, Qt Concurrent, Qt Quick, spdlog, fmt (use
`std::format`), simdjson, cmark, md4c directly, libarchive, libsamplerate, speexdsp, libsndfile,
opus, drogon, crow, uWebSockets, libhv, mdns (discovery is plain UDP in `fv_net`), utf8proc, ICU,
kissfft, miniaudio.

---

## 7. Data and identity

### 7.1 Folder layout (`%LOCALAPPDATA%\FioriVoce`, or `FIORIVOCE_DATA_DIR`)

```
fiorivoce_settings.json          OpenWhisper's keys and JSON shape (indent 2, insertion order)
fiorivoce.db (-wal, -shm)        OpenWhisper schema v18, unchanged, plus fv_* tables
recorded_audio.wav  .recorded_audio-recovery\<32hex>\{active.lock,session.json,audio.pcm,discarded}
recordings\  meetings\<id>\  captures\  remote_records\
scratchpad.txt  mcp_event_subscriptions.json  backup-preferences.json  .env
fiorivoce.log(.1-.3)  fiorivoce.crash.log  crashes\*.dmp
diagnostic-failures.json  diagnostic-metrics.json
.fiorivoce-data.lock  .fiorivoce-restore\{pending.json, journal.json, stage-*, previous-*}
components\<id>\{manifest.json, .installed, ...}  components\.cache  components\.staging
speech-models\<key>\{installed.json, *.gguf}
models\speaker-id\voxceleb_resnet34_LM.onnx
updates\  tls\
```

Every name without the product in it is OpenWhisper's, unchanged. Formats inside are
OpenWhisper's byte for byte where tests or the importer care: journal `session.json`, WAV `LIST`
and `bext` tags (with `ISFT` and originator "FioriVoce"), component manifests and sentinels,
`installed.json`, walkthrough manifests (Python float `repr`, ISO offsets), meeting spool
sidecars. Install folder: `%LOCALAPPDATA%\Programs\FioriVoce`.

### 7.2 Settings format

One JSON object, `indent=2`, trailing newline, insertion order kept, OpenWhisper's keys with
their exact kinds and fallback rules. Keys that served only dropped engines (`whisper_*`,
`custom_whisper_models`, `meeting_whisper_model`, …) stay declared with `dropped = true`: they are
preserved on rewrite and never read or shown, so an imported file round-trips. `selected_model`
accepts `parakeet`, `api` and `remote`; any other stored value reads as the default `parakeet`,
as `ChoiceSetting` already does. FioriVoce-only keys carry the prefix `fv_` so they can never
collide with a future OpenWhisper key: `fv_speech_runtime` (cpu, cuda), `fv_runtime_dir` (explicit
override), `fv_import` (`{source, at, version}`). Today's `QSettings` `engine/*` entries are
dropped.

### 7.3 Database schema

Schema v18 exactly as OpenWhisper's `services/database.py` and `services/models.py` create it:
tables, `AUTOINCREMENT`, FTS5 external-content tables and triggers, the
`meeting_segment_delivery` trigger, timestamp string formats, booleans as 0 and 1. Migrations v6
to v18 are ported so any OpenWhisper database opens; OpenWhisper's v5 and v17 fixture files
replay them. A golden test compares `sqlite_master` of a fresh C++ database with one OpenWhisper
made. FioriVoce never alters these tables or bumps `schema_version`. FioriVoce-only data goes in
tables prefixed `fv_`, with their own `fv_schema_version` table; import ignores them and backup
carries them.

### 7.4 Backup format

`.fvbackup` has OpenWhisper's layout and validation rules (manifest JSON compact, `data/<owned
path>` members, Zip64, checksums, `PATH_MARKER = "__OW_BACKUP_DATA__/"` kept as a format
constant), with format id `fiorivoce-backup` version 1 and members named from `identity()`
(`data/fiorivoce_settings.json`, `data/fiorivoce.db`). The reader accepts both
`fiorivoce-backup` and `openwhisper-backup`, mapping member names through `legacy_identity()`.
OpenWhisper cannot read a `.fvbackup`: its validator requires `data/openwhisper_settings.json`
and `data/openwhisper.db`.

### 7.5 Import from OpenWhisper

The importer is the restore pipeline pointed at a live folder (`backup/import.h`), offered by
guided setup and by Settings > Backup:

1. **Locate** `OPENWHISPER_DATA_DIR`, then `%LOCALAPPDATA%\OpenWhisper`, or a folder the user
   picks.
2. **Take OpenWhisper's data lease exclusively** (`.openwhisper-data.lock`, `LockFileEx`) for the
   whole staging. A running OpenWhisper holds it shared, so failure means it is running, and the
   user is asked to close it. While staging, OpenWhisper cannot start.
3. **Stage** with `backup::stage_from_data_root(legacy_identity(), source, identity())` straight
   into `.fiorivoce-restore/stage-*` (no zip): SQLite online backup of `openwhisper.db` as
   `fiorivoce.db`; settings parsed through the key table and mapped (`selected_model`, unknown
   keys kept); recordings, meetings, captures, scratchpad and recovery journals hard-linked on the
   same volume, copied otherwise; meeting `spool_dir` and absolute history paths rebased by the
   existing restore code.
4. **Credentials**, after the user ticks "Copy API keys": every name OpenWhisper's code reads
   (`OPENAI_API_KEY`, `OPENROUTER_API_KEY`, `GROQ_API_KEY`, `OPENCODE_*`, `TYPESAFE_API_KEY`, MCP
   token, event and webhook secrets, remote-engine token) is read from service `OpenWhisper` and
   written under `FioriVoce`; `OPENWHISPER_*` names become `FIORIVOCE_*`.
5. **Apply** at restart (exit code 73) under FioriVoce's exclusive lease, with the existing
   journal and rollback. Record `fv_import`. Import is one-way and repeatable.

A `.owbackup` file goes through the normal restore page with the same mapping.

### 7.6 What is shared with an OpenWhisper install

Only downloaded speech runtimes and models, and only at adoption. `models::discover_openwhisper`
lists OpenWhisper's `components\` and `speech-models\`; a tree is accepted when its sentinel,
manifest and `installed.json` validate and the catalog's SHA-256 matches. On first use FioriVoce
hard-links the files into its own folders (copies across volumes), skipping the embedded Python
files of OpenWhisper's runtime component, and loads from its own path. A loaded hard link does
not stop OpenWhisper deleting its own link, so OpenWhisper's swap and rollback keep working.
FioriVoce never writes into OpenWhisper's folders. Settings, database, credentials, logs, locks
and mutex names all differ. The remote engine shares nothing on disk; it speaks the OpenWhisper
wire (`wss://host:47821/openwhisper/v1/engine`, `OWHISPER?1` discovery, SAS strings) from
`remote/wire.h`, which is deliberately independent of `AppIdentity`. The MCP server name, client
header, event types and dashboard storage keys become FioriVoce's.

---

## 8. UI architecture

### 8.1 Theme pipeline

`src/ui_theme/resources/theme.qss` is OpenWhisper's 6,119-line sheet copied verbatim (source
commit in `resources/SOURCE`; 636 object names), with the 59 Tabler SVGs, app icons and
`check.svg` at the same relative paths, compiled into `:/ui_theme/`. `fv::ui::ThemeManager` runs
OpenWhisper's order:

1. rewrite `url(ui_qt/assets/...)` to `url(:/ui_theme/assets/...)` and resolve `@icon-suffix`
   first, because the suffix sits inside URLs;
2. substitute `@tokens` from `Palette` (96 tokens, dark and light);
3. apply `scale_qss_fonts` with `pyport::round` at 90, 100, 115 and 130 %;
4. set the application stylesheet, the `QPalette` and the app font (Segoe UI, 14 px at 100 %).

The resolved sheet is golden-tested: its SHA-1 equals the reference dump
`app-<theme>-s<scale>.qss` for both themes and all four scales. Unknown tokens are logged and left
unresolved, never thrown from a style path. "Match system" follows
`QStyleHints::colorScheme` and sets it with `setColorScheme`. The base style is `windows11`, set
after `QApplication` exists, wrapped by `SnappyTooltipStyle`. Painted widgets (progress bars,
switches, overlays, charts) read a typed `Theme` struct of `QColor`s instead of parsing token
strings per paint. FioriVoce-only rules go in `src/<dir>/resources/<dir>.qss`, appended in sorted
directory order; `theme.qss` is never edited.

**Qt floor 6.8 LTS.** `setColorScheme` needs 6.8 and the `windows11` style that the references
pin needs 6.7; workers then write no version branches. The 6.5 floor bought nothing on Linux,
where Ubuntu 24.04 ships 6.4. Development and references use the installed 6.10.3, which matched
PyQt6 6.11.0 references at 0 differing pixels in the a33 trial; moving the kit to 6.11.x removes
the minor-version risk and is recommended. Every reference sidecar records the Qt version.

### 8.2 Shared widgets

`fv_ui_widgets` ports a02's widgets with OpenWhisper's class names, in two packages before any
page uses them (M0.9 leaf widgets, M2 composite widgets): `Button`, `PrimaryButton`,
`SuccessButton`, `DangerButton`, `WarningButton`, `HotkeyHintFilter`, `Card`, `HeaderCard`,
`ControlPanel`, collapsible card (300 ms InOutCubic), `WrappedLabel`, `ElidingLabel`,
`DecodeLabel`, `SettingsSwitch`, `SegmentedBar`, `TileBase`, `SettingTile`, `FieldTile`,
`InfoTile`, `NavRail`, `FieldHelpCard`, `FeatureBadge`, `EmptyState`, `ElidingComboBox`,
`NoWheelComboBox`, `NoWheelSpinBox`, `SearchableComboBox`, `AnimatedProgressBar`,
`TabbedContentWidget`, `ButtonRow`, `Reflow`, `FlowLayout`, `StudioSplit`, `Stage`,
`ScrollingBody`, `UpdateNoticeButton`, context-menu helper, `revealInFileManager`, `DragToMove`.
Hotkey capture fields live in `fv_ui_hotkeys`.

### 8.3 PyQt classes to C++ classes

- A Python class `Foo(QFrame)` in `ui_qt/widgets/foo_bar.py` becomes `class fv::Foo : public
  QFrame` in `src/<dir>/foo_bar.{h,cpp}`: same class name, **same Qt base class** (theme.qss
  selects on Qt class names, `#objectName` and `[property=value]`; a33 found no selector naming
  an OpenWhisper class), same file stem. Private `_Foo` helpers go into the `.cpp` in an anonymous
  namespace.
- In Qt classes, signals and methods go from snake_case to camelCase, as CLAUDE.md requires
  (`record_toggled` to `recordToggled`, `set_status` to `setStatus`); the rule is mechanical, so
  a reviewer still finds the Python name. Python properties become `x()` and `isX()`.
  `pyqtSignal(object)` payloads become registered value structs. Assignable `on_*` callbacks on
  Qt classes become signals.
- Qt-free code keeps Python's names verbatim in snake_case, like `src/engine`
  (`format_audio_duration`, `append_preview_text`).
- Layout numbers, timer intervals, easing curves and paint constants are copied literally, with
  the Python line cited in a comment when the reason is not obvious.
- Pure helper functions inside widget modules move to `fv_uimath` or the area's core library, so
  they get golden vectors. Anything with a decision table (enable rules, wording, layout maths) is
  a pure function; the widget only applies it.

### 8.4 objectName and dynamic-property discipline

- Object names are set at construction with literals copied verbatim from the Python.
- Dynamic properties are set only through `fv::ui::setStyleProperty(widget, prop::tone, value)`
  (port of `set_style_property`: no-op when unchanged, otherwise set, unpolish and polish).
  Property names are constants in `ui_theme/style_props.h`, generated in M0 from the theme census
  (`tone`, `state`, `kind`, `buttonId`, `tileId`, `selected`, `muted`, `nearCap`, `overlay`,
  `stage`, `finalizationTone`, `meetingLink`, `meetingResultAction`, `scratchpadAction`,
  `starred`, `walkthroughAction` and the rest).
- Widget-local sheets go through `fv::ui::setLocalStyleSheet(widget, tokenSource)`, which stores
  the `@token` source in a dynamic property; a theme or scale change re-resolves only the widgets
  that carry it. Colours never appear as hex in C++; sizes that scale use `fv::ui::scaled(px)`.
- `tools/lint/check_qss_names.py` fails CTest when a `#name` or `[prop]` in `theme.qss` is used by
  no C++ file and is not on `tools/lint/qss_unused.txt` (the allowlist of unported features, which
  shrinks each milestone); `check_style_props.py` fails on a raw `setProperty` or `setStyleSheet`
  in `ui_*` directories or a property name not in `style_props.h`.
- The top-level-window property `openwhisperHotkeyWindow` becomes `fiorivoceHotkeyWindow`
  (internal).

### 8.5 Wiring controllers and widgets

Widgets never call services: they emit intent signals and expose setters, as in Python. Each
feature has a presenter QObject (for example `UploadPresenter`) created by its `install()`, which
connects widget signals to runtime calls and runtime `Signal`s to widget setters through
`qt::post`. Runtimes are Qt-free UI actors on `ctx.ui()`, reached through `ctx.service<T>()`.

OpenWhisper's 2,930-line `UIController` is split along its own seams, each its own file:
`UiController` (shell glue), `OverlayRouter`, `SettingsOpener`, `DownloadProgressPresenter`,
`UpdateHandoff`, plus the meeting and walkthrough glue inside their feature libraries. a03's
wiring tables become per-feature `install()` code plus one test per feature, "<feature> connects
every signal", which walks each widget's `metaObject()` and fails on an unconnected signal not
listed as intentionally unused. An app-level smoke test opens every screen and Settings page
offscreen. Screens registered in `ScreenRegistry` are created lazily and replay the latest queued
`updateScreen` calls on creation, as `MainWindow.update_screen` does. The Settings window's staged
idle build is dropped; pages stay lazily built on first visit.

### 8.6 Where strings live

User-visible strings stay at their use site, verbatim from Python, as UTF-8 (`/utf-8` is on):
`QStringLiteral(u"…")` in Qt code, `std::string` literals in core code, typographic characters
(… — → · ✓) copied, never retyped. "OpenWhisper" in copy becomes the product name from
`identity().product`. Strings that core logic decides (statuses, refusals, readiness, batch lines,
consent text, error sentences) live in the core library as `constexpr std::string_view` or builder
functions, golden-tested with the replay driver's rebrand map. Large text data (cleanup prompts,
catalogs, model and component rows, meeting prompts, keyboard names) is generated from the Python
source into `src/<dir>/data/*.json` and embedded. `tools/lint/check_brand.py` rejects the literal
"OpenWhisper" outside `base/identity.cpp`, `backup/import*`, `remote/wire.h` and tests. No `tr()`:
OpenWhisper is English-only. The webui is rebranded in its source (page title, missing-bundle
notice, `openwhisper.meeting.*` storage keys to `fiorivoce.meeting.*`) and rebuilt with Node at
development time; `webui/dist` is committed and served from the install folder.

---

## 9. Testing and parity gates

### 9.1 Test targets and layout

- `tests/<dir>/` builds `fv_<dir>_tests` (Catch2) linked against `fv_<dir>` and `fv_testkit`,
  discovered with `catch_discover_tests`, labelled `<dir>` plus kind labels: `unit`, `golden`,
  `widget`, `screenshot`, `decode`, `platform`. `decode` tests need `FIORIVOCE_TEST_RUNTIME_DIR`,
  `FIORIVOCE_TEST_MODEL` and `FIORIVOCE_TEST_WAV`; `platform` tests touch real hooks or devices
  and are skipped in CI.
- Widget tests use `fv_testkit`'s Qt main: an offscreen `QApplication` with pinned fonts, DPI and
  base style. CTest sets the environment (`QT_QPA_PLATFORM=offscreen`, `QT_PLUGIN_PATH`,
  `QT_QPA_FONTDIR`, `FIORIVOCE_DATA_DIR` per test), so no test puts a dialog on the screen.
- Test names keep OpenWhisper's: `TEST_CASE("repeat stop keeps the first deadline",
  "[test_recorder_postroll]")` with `// from tests/test_recorder_postroll.py::test_...`.
  Parametrized tests use `GENERATE` over the same tables.
- `tests/<dir>/PARITY.json` maps every OpenWhisper test file the package covers, function by
  function, to `ported`, `golden`, `dropped` or `pending` (with a reason and the finishing
  package). `tools/parity/parity_report.py` aggregates them into `docs/port/parity-report.md`
  over the 548 OpenWhisper test files; there is no shared ledger file.
- Fixtures: `tests/fixtures/` holds Python-made `openwhisper.db` at v5 and v17, Python-made
  `.owbackup`, sample WAVs, dumped PortAudio device lists, and recorded MCP, dashboard, FastAPI
  and remote-engine transcripts. C++ seams mirror Python's fakes: `FakeClock`, in-memory
  `SettingsStore`, `MemoryCredentialBackend`, `FakeHttpClient`, `audio::FakeBackend`,
  `speech::FakeBackend`, `FakeProcessLauncher`, null focus service.

### 9.2 Golden vectors

`tools/parity/golden_vectors.py` takes curated cases from `tools/parity/cases/<module>.py` (one
file per Python module, so packages never collide); `trace_plugin.py` harvests inputs from
OpenWhisper's own tests. Output goes to `tests/golden/<python.module>.json` with the OpenWhisper
commit inside. `fv_golden` replays them:

```cpp
golden::for_each("services.format_utils", "format_audio_duration",
    [](const golden::Case& c) { CHECK(format_audio_duration(c.arg<double>(0)) == c.result<std::string>()); });
```

Comparators: exact, rebranded string, float with a per-file tolerance (1e-6 absolute for float32
DSP), `raises`. Resampled PCM from PyAV's swresample and from `fft_resample` are golden vectors
too. The package that ports a module generates and commits its vectors; a changed vector is
reviewed as a behaviour change, and vectors are regenerated only when the reference commit moves.

### 9.3 Reference screenshots

`tools/parity/render_reference.py` gets one builder file per area (`tools/parity/builders/aNN.py`)
with fixed fake data, pinned clock, time zone, seeds, focus, default button and size, and
animations at their end values (or fixed times for painted animations). Pins, both sides:
offscreen, `QT_QPA_FONTDIR=%WINDIR%\Fonts`, `QT_FONT_DPI=96`, `QT_SCALE_FACTOR` 1 (and 2 for the
HiDPI set), `windows11` base style set after `QApplication` exists, never move the mouse. Output:
`tests/reference/<widget>/<theme>-<style>-dpr<n>-s<scale>.{png,json}`. The C++ test builds the
same widget with the same data and calls `screenshot::expect_matches(widget, "<widget>",
"<variant>")`: first the tree (class, objectName, properties, geometry, text, visibility) for a
readable failure, then pixels after converting both to ARGB32, writing a magenta diff PNG into the
build tree. Default tolerance is 0 differing pixels; an exception needs a `.tolerance.json` with a
reason the lead approved. Variants: dark and light at 100 %, plus dark at 130 % for top-level
windows, Settings pages and anything with reflow. Screenshot tests run on Windows with the
recorded Qt kit; on another kit they skip with a message, and the tree comparison still runs.

### 9.4 Where the harness lives

`out/parity-scratch` moves to `tools/parity/`: `ow_isolation.py`, `classify_tests.py`,
`coverage_table.py`, `golden_vectors.py`, `trace_plugin.py`, `render_reference.py`,
`record_transcripts.py`, `parity_report.py`, `classification.json` and `reference.json` (the
OpenWhisper commit and Python path). The C++ replay code from `replay/` becomes `fv_golden` and
`fv_screenshot` in `tests/support/`. Generated data goes to `tests/golden` and `tests/reference`
when committed, else to `out/parity`. Environment: `FIORIVOCE_OW_ROOT` and `FIORIVOCE_OW_PYTHON`.
Scripts never write into the OpenWhisper checkout (a33's isolation recipe).
`tools/parity/README.md` gives the commands.

### 9.5 What a work package must pass before merge

1. From the merge result on the wave's integration branch, `cmake --workflow --preset
   windows-ci` passes: configure, build with warnings as errors on our targets, every test.
2. `decode` tests run with the `FIORIVOCE_TEST_*` variables when the package touched `src/engine`
   or `src/speech`.
3. The core libraries it touched build and pass on `linux-core` (run per wave in WSL Ubuntu).
4. `clang-format --dry-run -Werror` is clean; `lint.layering`, `lint.ownership` (every changed
   file is inside the package's owned directories, its `tests/<dir>`, its golden cases, builders
   and references), `lint.qss_names`, `lint.style_props` and `lint.brand` pass.
5. Every golden-vector candidate the map lists for its modules has a vector file and a passing
   replay.
6. Every OpenWhisper test file the map assigns to its modules is in its `PARITY.json`; at most
   10 % of the test functions are `pending`, each with a reason and a finishing package.
7. Every new widget, dialog or page has its screenshot variants passing.
8. No new third-party dependency, settings key, style property or edit to a frozen file.

---

## 10. Build

### 10.1 Module discovery

`cmake/FioriVoceModules.cmake` (frozen after M0.1) defines the only functions a package calls.
Each `src/<dir>/CMakeLists.txt` is one call, owned by the directory's package:

```cmake
fv_module(KIND core
          DEPS fv_base fv_pyport
          THIRD_PARTY PCRE2::8BIT
          WIN32_LIBS user32)
```

- `fv_module` derives the target from the directory, globs `*.cpp`, `*.h`, `detail/`, `testing/`,
  and `win/` or `posix/` with `CONFIGURE_DEPENDS`, applies warnings, `/utf-8`, the kind's PCH and
  the include root, records `DEPS` for the layering lint, and for `ui`, `feature` and `qt` kinds
  runs AUTOMOC and `qt_add_resources` on `resources/`. A directory with headers only becomes an
  `INTERFACE` library, which is how M0.1's contract headers exist before their implementations.
- The root file globs `src/*/CMakeLists.txt` and `tests/*/CMakeLists.txt` and adds them sorted;
  `target_link_libraries` resolves names at generate time. A deferred check
  (`cmake_language(DEFER CALL ...)`) fails configure when a `core` module depends on a non-core
  one or links a `Qt6::` target.
- `fv_feature(ID ... CLASS ...)` appends to a global property; at the end of configure
  `file(GENERATE)` writes `generated/registry.cpp`, compiled into `fiorivoce`. `fv_embed(...)`
  turns `data/*.json` into a constexpr table.
- `fv_test(DEPS fv_testkit fv_golden)` in `tests/<dir>/CMakeLists.txt` builds `fv_<dir>_tests`,
  labels it and sets the environment.
- Adding a module, a feature, a test or a file never edits a file another package owns.

Lead-owned files, edited only by milestone-start packages: root `CMakeLists.txt`,
`CMakePresets.json`, `vcpkg.json`, `cmake/*`, `settings/keys.inc`, `appkit/order.h`,
`appkit/destinations.h`, `ui_theme/style_props.h`, `ui_theme/resources/theme.qss`, `CLAUDE.md`,
`BUILDING.md`. A contract that turns out wrong is fixed by an "interface fix" package at a wave
boundary, never inside a worker's package.

### 10.2 Build-time controls

- Two shared PCHs, `fv_pch_core` (standard headers, `<format>`, `nlohmann/json.hpp`) and
  `fv_pch_qt` (QtCore, QtGui, QtWidgets), reused with `REUSE_FROM`.
- Beast, Asio, OpenSSL, curl, sqlite3, PortAudio, FFmpeg and Windows SDK headers never appear in
  public headers (pimpl), so they never enter another library's compile. Beast and Asio compile
  once with `BOOST_BEAST_SEPARATE_COMPILATION` and `BOOST_ASIO_SEPARATE_COMPILATION`.
- No unity builds: anonymous-namespace helpers ported from Python modules collide.
- Qt deployment runs once: an `fv_qt_runtime` custom target copies Qt DLLs and plugins into `bin/`
  and every Qt executable depends on it, instead of `windeployqt` after each executable.
- `sccache` is used as `CMAKE_CXX_COMPILER_LAUNCHER` when on `PATH`.
- A worker's inner loop: `cmake --build --preset windows-debug --target fv_<dir>_tests` and
  `ctest --preset windows-debug -L <dir>`. Budget: under 60 s incremental for one package; a cold
  build of the finished app under 10 minutes on 16 cores.

### 10.3 vcpkg additions (M0.1, then frozen)

`nlohmann-json`, `pcre2`, `sqlite3[fts5,json1]`, `curl` (default `ssl`, which is Schannel on
Windows), `boost-beast`, `boost-asio`, `openssl`, `portaudio`, `ffmpeg[avcodec,avformat,swresample]`
with `default-features: false`, `pocketfft`, `minizip-ng[zlib]` with `default-features: false`,
`zlib`, `pugixml`, `wil` (Windows only), `catch2` (moves from FetchContent). Not added:
onnxruntime and PDFium (loaded by path), anything Qt. M0.1 sets `VCPKG_DEFAULT_BINARY_CACHE` to a
shared folder outside the worktrees (`D:\vcpkg-cache`, set by `scripts/setup-windows.ps1`) and
pre-builds every port once, so a fresh worktree restores FFmpeg, Boost and OpenSSL as archives.

### 10.4 Presets

Keep `default` (no vcpkg, FetchContent Catch2, builds only `fv_engine`, `fiorivoce-cli`'s engine
smoke path and their tests, as CLAUDE.md's commands expect), `windows`, `linux`, `linux-asan` and
the workflow presets. Add `windows-ci` (RelWithDebInfo, warnings as errors, lints as tests,
screenshot tests on), `windows-core` (`FIORIVOCE_BUILD_APP=OFF`, cores and their tests only, the
fastest loop for core packages), `linux-core` (`FIORIVOCE_BUILD_APP=OFF`, every `core` module and
its tests with GCC), a `parity` test preset (labels `golden` and `screenshot`, pinned environment),
and matching workflow presets.

---

## 11. Milestones

Every milestone ends in an app that starts, runs and can be demonstrated, with the tree green.

| M | Name | Areas | Demo |
|---|---|---|---|
| M0 | Foundation | a01 core and shell frame, a02 theme pipeline and leaf widgets, a10 settings and credentials, a33 harness, a34 build | Splash, then the themed frameless main window (title bar, menus, footer, four lazy tabs), tray, close to tray, single instance; settings and logs in `%LOCALAPPDATA%\FioriVoce`; the dev Upload tab still transcribes a WAV through today's `EngineWorker`; `fiorivoce-cli --version` and `--diagnostics`; title bar, footer and EmptyState pass screenshot parity |
| M1 | Dictation | a03 core and controller, a04 transcription and hotkey runtimes, a05 recorder, devices and WAV, a06 Windows hooks, paste and focus, a07 overlay (Bars, Classic), a08 Quick Record, a09 database, history core and sidebar, a14 catalog, store and speech service | Press `kp *` in Notepad, speak, the Bars overlay reacts, press again: Parakeet text is pasted, appears in Quick Record and the History sidebar and plays back; runtime and model adopted from an OpenWhisper install or picked once |
| M2 | Everyday | a05 rest (preview, incremental, splitting), a08 rest (Upload, batch, scratchpad, viewer), a09 rest (entry dialog, export, calendar, Stats), a11 (shell and General, Recording, Appearance, Hotkeys, API keys pages), a14 rest (downloads, consent, GPU, cloud backend), a15, a16 guided setup, a06 rest (Hotkeys page, mouse buttons), a07 rest (all looks, moments) | Live preview while dictating; long incremental dictation; batch upload locally and through Groq; calendar, export and Stats; download the CUDA runtime and a model from Downloads; first-run guided setup |
| M3 | Data safety and release | a10 backup, restore, schedule, OpenWhisper import; a16 update check and setup-exe update; a34 Inno installer, signing, CI | Install with the Inno setup, import an OpenWhisper data folder, back up and restore, update to a newer build |
| M4 | Text AI and personalization | a12, a13, Command Mode and transforms (a04), a11 remaining pages | AI cleanup with profiles and levels, dictionary, snippets, app styles, Command Mode, transforms, learned rules |
| M5 | Automation, agents and MCP | a19, a20, a21, installed agents and process ownership, `--api`, `--channel` | "Route by voice" to a webhook; hooks; live API with SSE; Claude Code reads history over MCP and the channel; `fiorivoce-cli --api` |
| M6 | Walkthroughs | a22, a23, a04 walkthrough runtime, documents, Auto Mode | Record a walkthrough with Snap and live drawing, review the storyboard, export PDF, Word and Markdown, AI write-up through Claude Code |
| M7 | Meetings | a24, a25 (mic and WASAPI loopback, ASR, diarization), a28, a30 server and webui, a31, a04 meeting runtime, transcript-only parts of a29 | A meeting with mic and system audio, live captions in the browser dashboard and on a guest's phone, speaker labels, exports, kill and recover, past meetings |
| M8 | Meeting intelligence | a26, a27 native agent loop, a29 rest (reports, review, live signals, voice commands, knowledge folder), a12 TypeSafe and judge | Live insights and a final report through OpenRouter; a report through an installed agent; voice commands in a meeting |
| M9 | Remote engine | a17, a18, Host Mode in a01 | FioriVoce dictates through an OpenWhisper host and serves an OpenWhisper client |
| M10 | Native updater and parity sweep | a16 transactional updater (`fiorivoce-updater`), a34 rest, parity report at 100 % for kept features, performance budgets | A signed release updates itself, rolls back after a forced failure, and passes the health gate |

### 11.1 Work packages for M0

| ID | Name | Owned directories | Depends on | Est. C++ lines |
|---|---|---|---|---|
| M0.1 | skeleton-and-contracts | root `CMakeLists.txt`, `cmake/`, `CMakePresets.json`, `vcpkg.json`, `scripts/setup-windows.ps1`, `tools/lint/`, `tests/CMakeLists.txt`, `tests/engine/` (moved from `tests/*.cpp`), `src/engine/CMakeLists.txt`, the header-only contracts of section 3 in `src/{base,settings,credentials,http,net,sql,process,audio,speech,appkit}/`, `appkit/order.h`, `appkit/destinations.h`, `CLAUDE.md`, `BUILDING.md` | — (runs alone) | 1,900 (headers 1,200; CMake and Python 700) |
| M0.2 | base | `src/base` (implementations), `tests/base` | M0.1 | 2,400 |
| M0.3 | pyport | `src/pyport`, `tests/pyport`, `tools/gen/unicode_tables.py` | M0.1 | 2,700 |
| M0.4 | parity-harness | `tools/parity/`, `tests/support/`, `tests/golden/` (format_utils, fuzzy_match seeds) | M0.1 | 1,500 plus moved scripts |
| M0.5 | log | `src/log`, `tests/log` | M0.2 | 1,500 |
| M0.6 | settings | `src/settings` (implementations), `tests/settings`, `tools/gen/settings_keys.py` | M0.2, M0.3 | 3,200 |
| M0.7 | credentials | `src/credentials` (implementations), `tests/credentials` | M0.2 | 1,000 |
| M0.8 | ui-theme | `src/ui_theme`, `src/uimath`, `tests/ui_theme`, `tests/uimath`, `ui_theme/style_props.h` | M0.2, M0.3 | 2,400 |
| M0.9 | appkit | `src/appkit` (implementations), `tests/appkit` | M0.2 | 1,300 |
| M0.10 | widgets-leaf | `src/ui_widgets`, `tests/ui_widgets`, `tests/reference/` for its widgets, `tools/parity/builders/a02.py` | M0.4, M0.8 | 2,700 |
| M0.11 | shell-and-cli | `src/ui_shell` (including `dev/` with today's bench), `src/app`, `src/cli`, `tests/ui_shell`, `tests/cli`; retires `tests/app` | M0.5, M0.6, M0.7, M0.9, M0.10 | 3,700 |

Waves: {M0.1} → {M0.2, M0.3, M0.4} → {M0.5, M0.6, M0.7, M0.8, M0.9} → {M0.10} → {M0.11}.
About 24k lines.

### 11.2 Work packages for M1

| ID | Name | Owned directories | Depends on | Est. C++ lines |
|---|---|---|---|---|
| M1.1 | appcore | `src/appcore` | M0 | 1,400 |
| M1.2 | dsp | `src/dsp` (`fft_resample`, `WindowSplitter`, `split_point`, level dB) | M0 | 1,000 |
| M1.3 | audio | `src/audio` (PortAudio backend, device ladder, `PcmTape`, `Resampler16k`, `AudioFileDecoder`, WAV writer and tags, `Player`; loopback stub until M7) | M0 | 2,800 |
| M1.4 | sql-db | `src/sql`, `src/db` (schema v18, migrations v6 to v18, `fv_*` tables) | M0 | 2,300 |
| M1.5 | hotkeys | `src/hotkeys` | M0 | 2,900 |
| M1.6 | focus-paste | `src/focus`, `src/clipboard` | M0 | 2,500 |
| M1.7 | http-download-archive | `src/http`, `src/download`, `src/archive` | M0 | 2,000 |
| M1.8 | overlay | `src/ui_overlay` (state machine, Bars, Classic, placement, notices), overlay maths in `src/uimath` | M0 | 3,000 |
| M1.9 | quick-record | `src/ui_record` (Quick Record tab, engine card, transcript pane, compact controller shell) | M0 | 2,300 |
| M1.10 | recorder | `src/recorder` | M1.2, M1.3 | 2,200 |
| M1.11 | models | `src/models` (catalog, component store, model cache, runtime resolver, OpenWhisper discovery and adoption) | M1.7 | 2,500 |
| M1.12 | speech | `src/speech` (`SpeechService`, lanes, `ParakeetBackend`, windows, cancel, generations, leases) | M1.2 | 2,000 |
| M1.13 | history-core | `src/history` | M1.4 | 1,700 |
| M1.14 | dictation | `src/dictation` (job slot, transcription and hotkey runtimes, pipeline without cleanup, delivery, failure retention) | M1.1, M1.5, M1.6, M1.10, M1.12, M1.13 | 3,300 |
| M1.15 | history-sidebar | `src/ui_history` (sidebar, card, edge tab, entry dialog, playback) | M1.13, M1.3 | 2,600 |
| M1.16 | controller | `src/ui_controller` (UiController, OverlayRouter, ApplicationController, readiness, runtime and model setup, shutdown), retires `src/ui_shell/dev/` | all of M1 | 2,800 |

Waves: {M1.1 to M1.9} → {M1.10, M1.11, M1.12, M1.13} → {M1.14, M1.15} → {M1.16}. M1.16 ends with an
end-to-end test: the fake audio backend plays a WAV, a scripted hotkey sequence starts and stops,
a fake recognizer returns text, and the test asserts the paste, the history row and the overlay
state trace. About 37k lines. Later milestones are cut the same way, in 3 to 4 waves of 4 to 9
packages; the total, about 205k lines, matches the sum of the map estimates.

---

## 12. Risks, and what the native app should do better

### 12.1 Risks

| Risk | Mitigation |
|---|---|
| Python semantics drift (rounding, code points, casefold, `\w`, `int()`, `fromisoformat`, float repr) | `fv_pyport` lands first; golden vectors cover every pure function; lint flags `std::round`, `std::lround`, `std::regex` and `.size()` on user text. PCRE2's `\w` uses a newer Unicode than Python 3.12; vectors show where it matters. |
| Contracts written in M0 are wrong and many packages depend on them | They carry only what the maps already show; members may be added, changes go through a wave-boundary interface-fix package. |
| The engine has no decode abort | Window-bounded decodes; worst case one 30 s window. |
| In-process native crash takes down the app (no worker isolation) | Minidump with faulting module, CPU-runtime offer on next start, journal recovery of the recording (a05) and meeting recovery (a24). An out-of-process engine host could later sit behind `speech::Backend` unchanged. |
| Runtime switching needs a restart | Explicit prompt, staged switch, documented difference. |
| One Recognizer for meeting chunks and previews falls behind on CPU-only machines | Preview lane is coalescing and drop-only; a25's backlog refill from SQLite handles lag. |
| PortAudio host-API set differs from sounddevice's bundled build | Verify in M1.3; the matching ladder's name-only rung keeps imported keys usable; a dumped device-list fixture replays through it. |
| swresample or pocketfft output differs from PyAV's or numpy's | Golden PCM vectors from Python; stated tolerance; transcripts compared on the decode tests. |
| Screenshot gate depends on Qt kit version, Segoe UI and pins | Recorded kit; Windows-only gate; tree comparison everywhere; move to 6.11.x. |
| Protocol parity without SDKs (MCP's two revisions, FastAPI 422/404/405 bodies, dashboard, remote wire) | Transcripts recorded from running Python servers and peers, replayed against the pure handlers and the C++ client. |
| Hook removal by Windows, UIA hangs | Hook callback does no I/O or locking; watchdog re-hook kept; abandonable focus thread. |
| Document fidelity (PDF pagination, DOCX that Word accepts) | Line-by-line port of pagination workarounds; golden XML from python-docx; each release's DOCX opened in Word and LibreOffice. |
| Build cost of FFmpeg and Boost in many worktrees | Shared binary cache, fenced headers, PCH; FFmpeg built once in M0.1. |
| Feature registration hides wiring mistakes | Per-feature "connects every signal" tests and the app-level smoke test. |
| Runtime-loaded DLLs collide by name (`onnxruntime.dll` also exists in System32; NeMo ships its own `msvcp140.dll`) | Load by full path with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`; log `GetModuleFileName` of each loaded dependency in diagnostics. |
| Licensing | FFmpeg LGPL (shared, decoders only), Boost BSL, OpenSSL Apache-2.0, PDFium BSD and Apache, ONNX Runtime MIT, PortAudio MIT, Tabler MIT; notices generated from vcpkg copyright files. |
| The webui rebrand needs Node at development time | Vendored source with committed `dist` and a freshness check in CI. |

### 12.2 Where a different owner answer changes the design

- **Identity**: if FioriVoce shared OpenWhisper's data folder and names, only `identity()`
  changes, the importer becomes a no-op, and runtime sharing needs a cross-process lock instead of
  adoption.
- **Remote wire compatibility**: if dropped, only `remote/wire.h` changes.
- **Native agent loop**: if the Node and Bun sidecars were kept, `meeting_agent` gains an NDJSON
  supervisor over `ProcessLauncher`; Node and Bun join the component catalog and CI; M8 grows by
  the packaging work.
- **Two-way backups**: if OpenWhisper must read FioriVoce backups, the writer switches to
  `legacy_identity()` member names and format id; nothing else changes.
- **Identical microphone rows not required**: `make_portaudio_backend()` could become miniaudio
  behind `audio::Backend` without touching the recorder.

### 12.3 What the native app should do better (each keeps the visible behaviour, each ledgered)

- **Startup**: painted window in under 400 ms on the build machine; model load, database open and
  scans after first paint; no lazy-import machinery or staged Settings build.
- **Stop to paste**: no worker-process hop or temporary `.f32` files; incremental windows already
  decoded; the final pass pre-empts previews. Target p95 under 300 ms after post-roll for a 10 s
  dictation on the GPU runtime.
- **Capture**: lock-free callbacks; `input_overflows` and `dropped_frames` should read 0 in normal
  use.
- **Meetings**: no reload after a meeting when the models match.
- **Cancel**: within one window instead of killing a worker; HTTP calls abort their replies.
- **Processes**: job objects own every child, with tree kill and disown semantics.
- **Single instance on Windows too**: a second launch raises the first window.
- **Idle footprint**: about twelve threads and no interpreter; target idle RSS under 150 MB without
  a model.
- **Downloads**: resumable with speed and ETA; hard-link adoption of OpenWhisper's model and
  runtime; component state from a few `stat` calls.
- **Diagnostics**: minidumps with the faulting module, crash-to-CPU-runtime recovery, a GUI hang
  detector that logs the stuck thread, all without message text.
- **History search**: a registered Unicode case-folding SQL function can extend case-insensitive
  search beyond ASCII, behind a test that ASCII behaviour is unchanged; off until the owner opts in.

---

## 13. Contested decisions

Each entry: the options as proposed, the choice, and the reason. "Checked" marks a claim verified
against the Python, the vcpkg ports or the machine while merging.

1. **Directory layout and target granularity.** Options: mirror Python's tree in two libraries
   (`fv_core`, `fv_qt`); nested `src/core/<lib>/<module>` and `src/app/<lib>` with about 33
   layered libraries; flat `src/<dir>`, one target per directory, about 55 libraries. **Chosen:
   flat, one target per directory.** A worker's test executable then links only its closure, so
   parallel workers on one 16-core machine do not each compile the whole port; ownership and
   layering lints become trivial; two libraries would make every worker build everything merged so
   far.
2. **Naming inside Qt classes.** Options: Python snake_case verbatim everywhere (amending
   CLAUDE.md); camelCase in Qt classes, Python names verbatim in core. **Chosen: camelCase in Qt,
   snake_case verbatim in core.** CLAUDE.md already requires Qt naming in the app, and the
   snake-to-camel mapping is mechanical, so grep still works.
3. **Qt in non-UI modules.** Options: 21 Python modules that import PyQt6 compile into a Qt
   library at their mirrored paths; all domain code Qt-free. **Chosen: Qt-free.** Those modules
   use Qt only for timers and signals, which the executor and `Signal` replace, and Qt-free code
   tests with Catch2 alone and builds on Linux.
4. **Home of the a04 runtimes.** Options: Qt modules as in Python; QObject runtimes in a Qt
   library over a Qt-free core; Qt-free UI actors on an injected UI executor. **Chosen: Qt-free UI
   actors.** a04 and a03 are the most test-pinned logic (warm-up traces, job slot), and a
   `ManualExecutor` replays them deterministically; Qt only adapts through presenters.
5. **Scope of the Python-semantics library.** Options: a full `fv::py` covering threading,
   executors, subprocess and logging as well as text; text and number semantics only. **Chosen:
   text and numbers only (`fv_pyport`).** Only observable results need Python's semantics;
   reproducing Python's threading and subprocess APIs would carry the GIL-era structure into C++.
6. **Third-party wrappers shaped like Python packages** (`pylib/httpx`, `pylib/fastapi`,
   `pylib/sounddevice`) versus purpose-built interfaces. **Chosen: purpose-built interfaces**
   (section 3). They are smaller, faked easily, and do not promise httpx or FastAPI semantics
   nobody needs; the FastAPI error bodies that matter are pinned by recorded transcripts instead.
7. **Audio library.** Options: PortAudio plus native WASAPI loopback; miniaudio for everything.
   **Chosen: PortAudio plus native WASAPI loopback.** The Microphones dialog lists one row per
   PortAudio host API, and saved keys, MME truncation and the Sound Mapper fallback are
   PortAudio-shaped (a05); loopback needs hand-written WASAPI either way. Checked: the vcpkg
   PortAudio port is 19.7 with no loopback devices.
8. **HTTP client.** Options: libcurl; QNetworkAccessManager on its own thread plus a Beast client
   for pinned addresses. **Chosen: libcurl.** Callers are Qt-free blocking worker code, and one
   stack covers pinning; QNAM would bring Qt under the cores plus a second client.
9. **Upload decoding.** Options: FFmpeg; Media Foundation. **Chosen: FFmpeg.** The claim that
   FFmpeg is not available at the baseline is wrong (checked: `ffmpeg` 9.0.2 is in the ports
   tree), and Python decodes and resamples with PyAV, which is FFmpeg.
10. **What audio the engine receives.** Options: `fft_resample` to 16 kHz shared by the live and
    file paths; the capture rate, letting the runtime resample. **Chosen: swresample to s16 mono
    16 kHz for decode windows, `fft_resample` for previews, meeting chunks, spool and diarization.**
    Checked: `local_asr/audio.py` and `incremental_dictation.py` use PyAV's `AudioResampler`, and
    only previews and meeting code use `fft_resample`; both proposals' premises were wrong, and
    the capture-rate option would feed the runtime different samples from Python.
11. **FFT library.** Options: pocketfft; kissfft. **Chosen: pocketfft.** numpy 2.x uses this
    C++ pocketfft, and arbitrary lengths are supported. Checked: vcpkg `pocketfft` installs
    `pocketfft_hdronly.h`.
12. **Unicode tables.** Options: generated from Python 3.12's `unicodedata`; utf8proc. **Chosen:
    generated.** They match Python's Unicode 15.0 exactly and add no dependency.
13. **Zip library.** Options: minizip-ng; miniz. **Chosen: minizip-ng with default features off
    and zlib only.** Streaming Zip64 with progress and cancel for multi-GB backups, and zlib is then
    shared with tar.gz. Checked: its default features pull bzip2, lzma, zstd and crypto, so they
    are turned off.
14. **PDF text extraction.** Options: own extractor of about 600 lines; PDFium loaded by path.
    **Chosen: PDFium by path.** pypdf handles empty-password encryption, xref streams and CID
    fonts, which a small extractor would not. Checked: PDFium is not in vcpkg.
15. **ONNX Runtime.** Options: vcpkg port, linked; official DLL loaded by path. **Chosen: by
    path.** Checked: the vcpkg port depends on protobuf, abseil, onnx, re2 and more, all built from
    source.
16. **Speech engine API.** Options: OpenWhisper's `SpeechProcess.request(op, ...)` protocol run on
    a thread; a scheduler with seven priorities and coalescing; a service with five lanes.
    **Chosen: `SpeechService` with seven lanes and coalescing.** The op protocol exists for a
    child process that the owner dropped; the lane order must also cover the remote host and
    background re-decodes.
17. **Switching between CPU and GPU runtimes.** Options: load the second runtime beside the first;
    one runtime per process with a restart. **Chosen: one per process.** Checked: the CUDA
    component's `bin/` ships `ggml.dll`, `ggml-base.dll`, `ggml-cpu.dll` and `llama.dll`, names a
    CPU build shares, and the Windows loader reuses a loaded module of the same name.
18. **Meeting model lease.** Options: never unload and drop the "Released" status; release the
    dictation model only when the meeting model differs. **Chosen: release only when the models
    differ, an `EngineLease` otherwise.** Dictation is refused during meetings anyway, so freeing
    a second model's memory is OpenWhisper's real behaviour worth keeping.
19. **`Signal` delivery.** Options: synchronous on the emitting thread; always posted to the
    subscriber's executor; Python callbacks kept as is. **Chosen: inline when the subscriber's
    executor is current, posted otherwise.** Python's callbacks are synchronous, and golden
    traces depend on that order on one thread; posting is still needed across threads, and the
    no-lock-while-calling-out rule removes the re-entrancy argument for always posting.
20. **Feature composition.** Options: none (port `ApplicationController` wiring as written);
    CMake-generated `registry.cpp`; static registrars linked whole-archive. **Chosen: generated
    registry.** No static-initialisation order, no whole-archive linking, and a missing feature is
    a link error.
21. **UIController shape.** Options: one 2,930-line class ported line by line; split along its
    seams. **Chosen: split.** One class cannot be ported by parallel packages and would be a merge
    hotspot; per-feature wiring tests replace line-by-line comparison.
22. **Worker threads.** Options: Python's threads and executors one for one; strands on one pool.
    **Chosen: strands on one pool.** Fewer threads, and "cancel queued" versus "drain" shutdown is
    built in.
23. **Child-process pipes.** Options: overlapped I/O on the asio loop; reader threads. **Chosen:
    reader threads.** Anonymous pipes do not support overlapped I/O, and `CancelSynchronousIo`
    matches a19's behaviour; agents and hook programs are few.
24. **Database writes.** Options: a writer strand returning futures; one writer connection
    behind a mutex. **Chosen: mutex.** Python's repositories write synchronously from worker
    threads and use the results; futures would change every call site.
25. **FioriVoce-only database data.** Options: additive migration 19; `fv_*` tables with their
    own version. **Chosen: `fv_*` tables.** `schema_version` stays 18, so OpenWhisper's migration
    tests and backup validation apply unchanged.
26. **Dropped-engine settings keys.** Options: preserve on disk, undeclared; declare with a
    `dropped` flag; delete on import. **Chosen: declare with `dropped`.** The generator stays
    mechanical, unknown-key preservation is kept, and the file round-trips.
27. **Prefix for FioriVoce-only keys.** Options: `fiorivoce_`, `fv_`, none. **Chosen: `fv_`.**
    Short, and an unprefixed name could collide with a future OpenWhisper key.
28. **Backup format id.** Options: `fiorivoce-backup` with mapped member names; OpenWhisper's
    format id "so OpenWhisper can read it". **Chosen: `fiorivoce-backup`.** Checked: OpenWhisper's
    validator requires `data/openwhisper_settings.json` and `data/openwhisper.db`, so its format id
    on renamed members would be rejected anyway.
29. **Import mechanism.** Options: direct copy into the live data root; staged through the restore
    pipeline and applied at restart. **Chosen: staged.** One tested path with journal and
    rollback.
30. **OpenWhisper's lease during import.** Options: shared lock; exclusive lock. **Chosen:
    exclusive.** Checked: `services/backup_startup.py` has every running OpenWhisper hold the lease
    shared, so a shared lock cannot detect a running copy.
31. **Reusing OpenWhisper's runtime and model.** Options: model in place plus runtime copy;
    hard-link adoption with copy fallback. **Chosen: hard-link adoption.** Checked on this machine:
    with one link of a DLL mapped by a process, deleting the other link succeeds, so adoption does
    not block OpenWhisper's own cleanup, and in-place use would.
32. **Logging API.** Options: Python `%`-style logging; `std::format`. **Chosen: `std::format`.**
    Log text is not a parity surface; the failure journal records categories, not messages.
33. **Test executables.** Options: two (unit and Qt); one per library. **Chosen: one per
    library.** Workers build and run only their closure.
34. **Widget test runner.** Options: Catch2 everywhere; Qt Test for widgets. **Chosen: Catch2,
    with the Qt Test library for helpers.** One runner and one discovery.
35. **Parity ledger.** Options: one ledger file per package; one shared `parity-status.json`;
    `PARITY.json` per test directory. **Chosen: per test directory.** No shared file, and it sits
    with the tests it describes.
36. **Pending tests at merge.** Options: pending with a reason; at most 10 % pending. **Chosen:
    at most 10 %, each with a reason and a finishing package.** An unbounded pending list defers
    parity indefinitely.
37. **Authoring the 186 settings keys.** Options: hand-port in one package with golden defaults;
    generate from the Python declarations. **Chosen: generate.** Fewer transcription errors, and
    the golden defaults still check the result.
38. **Milestone order.** Options: settings and personalization second, then uploads, walkthroughs,
    meetings, automation, remote, release; everyday features and release early, then AI,
    automation, walkthroughs, meetings, remote. **Chosen: the second, with meetings split into M7
    and M8.** Walkthrough write-ups and meeting intelligence need the LLM (M4) and agents (M5)
    first, and an early installer and import let the owner run FioriVoce on real data.
39. **Widget-local stylesheets.** Options: Python's app-wide event filter re-resolving on
    `StyleChange`; a helper that stores the token source. **Chosen: the helper.** One pass per
    theme change, and a lint enforces it.
40. **Unity builds.** Options: off; on in CI. **Chosen: off.** Anonymous-namespace helpers from
    different Python modules collide.
41. **Single instance.** Options: named mutex and pipe; QLockFile and QLocalServer. **Chosen:
    QLocalServer.** a01 specifies this port on every platform.
42. **Qt kit for references.** Options: require 6.11.x; accept 6.10.3 with the version recorded.
    **Chosen: accept 6.10.3 now, move to 6.11.x when installed.** The a33 trial matched at 0
    differing pixels across the two.
43. **Nemotron.** One proposal kept a `nemotron` backend key "if the owner keeps it". **Chosen:
    no key.** The owner dropped Nemotron Streaming.
44. **Speech submission API.** Options: `std::future`; callback on a reply executor. **Chosen:
    callback on a reply executor, plus a blocking `run` for worker threads.** The GUI thread must
    never wait, and worker threads such as the meeting ASR queue keep Python's blocking style.
45. **Engine during M0.** Options: retire the bench in M0; keep it as a dev tab until the speech
    service exists. **Chosen: keep it** in `src/ui_shell/dev/` until M1.16, so M0's app still
    transcribes.
