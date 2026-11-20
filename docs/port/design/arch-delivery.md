# OpenWhisper_CPP parity port: architecture for parallel delivery

Proposal "delivery first". It answers one question above all: how do dozens of
fresh-context workers, each in its own worktree and build directory, port about
200k lines of Python into C++ without stepping on each other, without guessing
at interfaces, and without breaking the tree? Every structural choice below is
followed by the way it lowers the chance of a worker going wrong.

Inputs: `docs/port/map-summaries.json`, the 34 maps in `docs/port/map/`, the
current tree (`src/engine`, `src/app`, `src/cli`, `tests`), and the owner's
decisions and working assumptions in the task brief. Names used here are
binding for workers: when this document names a target, directory, class or
header, a work package uses exactly that name.

---

## 1. Principles

1. **One directory, one target, one owner.** Every `src/<dir>` builds exactly
   one library `fv_<dir>`, and in any wave exactly one work package owns it.
   *Why:* a merge can only conflict where two packages touch the same file, so
   disjoint ownership makes conflict-free merges a property of the plan, not of
   luck.
2. **No shared file is edited after M0.** CMake discovers modules, the app
   discovers features, the theme is copied whole, and every settings key is
   declared in M0. *Why:* the usual merge hotspots (top-level CMakeLists, a
   composition root, a key table, a stylesheet) disappear instead of being
   managed.
3. **Interfaces before implementations.** Cross-cutting interfaces (section 3)
   land in M0 with an in-package fake; a package that consumes a service codes
   against the header and tests against the fake. *Why:* a worker never waits
   for, or reimplements, a neighbour's service.
4. **Everything below the widgets is Qt-free and headless.** Runtimes,
   controllers' state machines, gates and string builders are plain C++20 with
   injected executors, clock and I/O; Qt only adapts. *Why:* a worker can prove
   behaviour with Catch2 and golden vectors in seconds, with no display, no
   event loop and no flaky timers.
5. **Python is the oracle, replayed as data.** Pure functions are pinned by
   golden vectors generated from OpenWhisper; widgets by tree dumps and
   offscreen reference screenshots. *Why:* "ported faithfully" becomes a test a
   worker can run, not a judgement a reviewer must make.
6. **Short dependency closures.** Libraries are small (1,000 to 3,000 lines),
   layers are strict, Beast/OpenSSL/curl/sqlite sit behind narrow pimpl
   headers, and each library has its own test executable. *Why:* a worker
   builds and tests only `fv_<dir>_tests` and its closure, in well under a
   minute, many times per hour.
7. **Threads are named and few; crossing them is mechanical.** Every class says
   which executor it lives on; cross-thread delivery always posts, never calls
   inline. *Why:* the Python code leaned on the GIL; explicit affinity rules are
   what stop a C++ port from inheriting races it cannot see.
8. **The speech engine is one queue.** One `SpeechService` thread owns the
   NeMo runtime, the Recognizer and every decode for dictation, preview,
   uploads, meetings, walkthroughs and the remote host. *Why:* it is the only
   design that respects "one thread at a time" and "never unload" without
   per-feature lease logic.
9. **Formats are OpenWhisper's, names are OpenWhisper_CPP's.** Settings keys, DB
   schema, backup layout and wire protocols stay byte-compatible; file names,
   data root, credential service and environment variables come from one
   `AppIdentity`. *Why:* import is a mapping, not a migration, and a worker
   never has to decide a name.
10. **Every milestone runs, and the tree is always green.** A package merges
    only when the whole tree configures, builds and passes from its merge
    commit. *Why:* parallel waves only work when wave N+1 starts from a tree
    known to be good.

---

## 2. Libraries and directory layout

### 2.1 Layout

```
CMakeLists.txt            M0-frozen; includes cmake/OpenWhisper_CPPModules.cmake
cmake/                    OpenWhisper_CPPModules.cmake (fv_module, fv_test, discovery),
                          OpenWhisper_CPPDeps.cmake (every find_package), Warnings, Qt
src/<dir>/                one target fv_<dir>; sources globbed; public headers
                          at the top level, private ones in detail/
src/<dir>/win/            Windows-only sources (compiled when WIN32)
src/<dir>/posix/          stubs or POSIX sources (compiled otherwise)
src/<dir>/testing/        fakes for the interfaces this dir defines
src/<dir>/resources/      qrc content (ui_* dirs only)
src/app/                  openwhisper-cpp.exe: main.cpp only
src/cli/                  openwhisper-cpp-cli.exe
tests/<dir>/              fv_<dir>_tests, CTest label <dir>
tests/support/{testkit,golden,screenshot}   test helper libraries
tests/golden/<python.module>.json           committed golden vectors
tests/reference/<widget>/<variant>.{png,json} committed screenshots + tree dumps
tools/parity/             golden_vectors.py, trace_plugin.py, render_reference.py,
                          ow_isolation.py, classify_tests.py, coverage_table.py
tools/lint/               check_layering.py, check_qss_names.py, check_ownership.py
webui/                    vendored React dashboard source + committed dist/
third_party/nemo_speech/  unchanged (vendored C ABI header)
third_party/onnxruntime/  vendored onnxruntime_c_api.h only (MIT)
```

Includes keep today's convention: the include root is `src/`, so a header is
`#include "history/manager.h"`. Namespace `fv` everywhere, with one nested
namespace per library where names would collide (`fv::meeting`, `fv::net`,
`fv::speech`). `tools/lint/check_layering.py` reads each module's declared
`DEPS` and fails CTest (`lint.layering`) when a file includes a header from a
library outside its closure or includes another library's `detail/`. That
check is what makes a narrow header narrow in practice.

Rejected: nesting by layer (`src/core/settings`) - it lengthens every include
and buys nothing the module `KIND` does not already enforce.

### 2.2 Kinds and what they may link

| Kind | May link Qt | May link third-party | May depend on |
|---|---|---|---|
| `core` | no | the ports named in its row below | `core` only |
| `qt` | Core, Gui (Widgets only in `appkit`) | as listed | `core`, `qt` |
| `ui` | Core, Gui, Widgets, Svg, SvgWidgets, Network (never for HTTP) | none | anything but `exe` |
| `feature` | as `ui`; linked into the app with `WHOLE_ARCHIVE` | none | anything but `exe` |
| `exe` | as needed | as needed | anything |

`core` modules must build on Linux with GCC (only `win/` sources are
Windows-only, and each has a `posix/` stub returning "not supported on this
platform"). `fv_engine` keeps its own rules from CLAUDE.md.

### 2.3 Targets

Core (Qt-free), in dependency order:

| Target | Dir | Third-party | Depends on | Holds |
|---|---|---|---|---|
| `fv_engine` | `src/engine` | (NeMo by path) | - | unchanged; Recognizer, NemoApi, SharedLibrary, wav |
| `fv_base` | `src/base` | nlohmann-json | - | identity, paths, clock, cancel, executors, Signal, log facade, fs (atomic write, signature, byte-range lease), random/ids, json helpers |
| `fv_pyport` | `src/pyport` | pcre2 | - | Python semantics: round half even, float repr/`%g`/`:.1f`, `int()`, `json.dumps`, code-point len/slice, casefold and categories (generated tables), `Regex` (PCRE2 UTF+UCP), `SequenceMatcher`, `fromisoformat` |
| `fv_log` | `src/log` | - | base | rotating file sink, failure journal, metrics, diagnostics report, startup profiler, crash handler |
| `fv_settings` | `src/settings` | - | base, pyport | `Setting<T>` kinds, all keys, `SettingsStore`, enums with labels, shared resolvers |
| `fv_credentials` | `src/credentials` | - (advapi32) | base | `CredentialStore`, WinCred backend, memory backend, dotenv, resolution order |
| `fv_sql` | `src/sql` | sqlite3[fts5] | base | RAII connection, statements, transactions, online backup, functions |
| `fv_db` | `src/db` | - | sql, pyport | the whole schema v18, migrations v6..v18, open/repair, schema constants |
| `fv_http` | `src/http` | curl | base | `HttpClient` (curl), SSE parser, multipart, error classification |
| `fv_net` | `src/net` | boost-beast/asio, openssl | base | `Reactor`, `HttpServer`, `WsServer`, `WsClient`, TLS identity (self-signed P-256), UDP socket, LAN/VPN address classification |
| `fv_process` | `src/process` | wil | base | `ProcessLauncher` (job objects, no window, pipes, tree kill, disown), `ProcessEnv`, `Shell` (open URL, reveal, ShellExecute via Explorer) |
| `fv_download` | `src/download` | - | base, http | resumable verified download (`.part`, Range, SHA-256, free space, error texts) |
| `fv_archive` | `src/archive` | minizip-ng, zlib | base | zip read/write (Zip64, streaming), tar.gz reader, safe extraction rules |
| `fv_dsp` | `src/dsp` | kissfft | base | `fft_resample`, `WindowSplitter`, level dB, moving average, split points, Kaldi fbank |
| `fv_audio` | `src/audio` | miniaudio (Win: Media Foundation) | base, dsp | `AudioBackend` (capture, loopback, playback), `DeviceCatalog` + key matching, `WavWriter` + LIST/bext tags, `AudioFileDecoder`, `Player` |
| `fv_recorder` | `src/recorder` | - | audio, dsp | `Recorder` (SPSC ring, post-roll gate, pause, mic fallback/switch), `RecordingJournal` + recovery |
| `fv_models` | `src/models` | - | download, archive, engine | component catalog (embedded JSON), component store, speech-model cache, HF consent policy, NVML probe, runtime resolver, OpenWhisper install discovery |
| `fv_appcore` | `src/appcore` | - | base, pyport | a03 core: `OverlayState`, `ActivityState` and gates, readiness messages, `EngineReloadMachine`, `PreviewThrottle`, `CancelRouter`, `ShutdownPlan`, busy reasons |
| `fv_speech` | `src/speech` | - | engine, dsp, http, appcore | `SpeechBackend`, `SpeechService` (engine thread), `ParakeetBackend`, `CloudSpeechBackend`, preview/incremental session logic, languages |
| `fv_hotkeys` | `src/hotkeys` | - (user32) | base, pyport | grammar, `keyboard`-library name table, matcher, hold state machine, conflicts, `KeyboardHook`/`MouseHook` |
| `fv_focus` | `src/focus` | wil (UIA) | base, pyport | `FocusSnapshot`, app catalogue, `site_from_title`, `join_with_context`, `prompt_block`, capture service, `SyntheticKeys`, UIA reader |
| `fv_uimath` | `src/uimath` | - | base, pyport | Qt-free UI maths: fuzzy match, overlay positions, loudness, easing/moments, ribbon, release-notes model, list reconcile, button-row/flow sizes |
| `fv_text` | `src/text` | - | settings, pyport | a12 pure core: dictionary, vocabulary, snippets, cleanup prompts/profiles/transforms, rewrite, thinking levels, endpoint profiles and catalog tables, recognition context, app styles |
| `fv_llm` | `src/llm` | - | http, text, credentials | text generation (chat, Responses, Anthropic, Google; streaming, tools), retry/deadline/cancel, model lists, key checks, local server probes, cleanup runner, fast judgments |
| `fv_agents` | `src/agents` | - | process, settings | installed agent discovery (Claude Code, Codex, OpenCode), headless drivers and ACP client, `run_reading_task` |
| `fv_history` | `src/history` | - | db, settings | history manager (recordings, retention, versions), export, calendar index, format utils, titles, dictation stats |
| `fv_dictation` | `src/dictation` | - | appcore, speech, recorder, hotkeys, focus, history, text, llm | a04 job slot and runtimes (transcription, command, hotkey, streaming), a05 dictation pipeline, uploads and batch types, scratchpad text rules |
| `fv_backup` | `src/backup` | - | db, archive, settings, credentials | backup create/inspect/stage/apply, journal, rollback, data lease, OpenWhisper import |
| `fv_update` | `src/update` | - | download, process, settings | release feed, trust rules, schedule, setup-exe hand-off |
| `fv_automation` | `src/automation` | - | net, http, process, llm, agents, credentials | a19 actions, routing, event bus, webhooks, programs, filters, live API routes, channel bridge |
| `fv_agentapi` | `src/agentapi` | - | history, net, automation | a20 History REST, MCP JSON-RPC server and tools, MCP events |
| `fv_walkthrough` | `src/walkthrough` | - | speech, agents, settings | a22 core: board, sections, manifest, library, write-up prompts and styles |
| `fv_meeting_state` | `src/meeting_state` | - | db, text | a28 schema, ops, store, corrections, repository, exports; agent and host interfaces |
| `fv_diarize` | `src/diarize` | (ONNX Runtime by path) | dsp, engine | ORT C API loader, speaker embedder, online diarizer, assign |
| `fv_meeting_capture` | `src/meeting_capture` | - | audio, recorder, speech, meeting_state | a25 capture session, watchdog, spool, ASR worker, offline pass, playback |
| `fv_meeting_agent` | `src/meeting_agent` | - | meeting_state, llm, agents, net | a26 prompts, scheduler, policy, evidence; a27 native agent loop; loopback MCP |
| `fv_meeting_intel` | `src/meeting_intel` | - | meeting_state, llm, archive | a29 reports, review, citations, live signals, recall, knowledge search |
| `fv_meeting` | `src/meeting` | - | meeting_capture, diarize, meeting_state | a24 engine, end run, refinalize, recovery |
| `fv_meeting_web` | `src/meeting_web` | - | net, meeting_state | a30 dashboard API, WS hub, static files |
| `fv_remote` | `src/remote` | - | net, speech, history, meeting_state, credentials | a17 host, client, pairing, discovery, records, `RemoteSpeechBackend` |

Qt, non-widget:

| Target | Dir | Qt | Depends on | Holds |
|---|---|---|---|---|
| `fv_documents` | `src/documents` | Gui | archive, walkthrough (pugixml) | PDF composer and pagination, Markdown composer, DOCX writer |
| `fv_appkit` | `src/appkit` | Core, Gui, Widgets | base | `QtUiExecutor`, `qtConnect` bridge, `AppContext`, `Feature` registry, `ScreenRegistry`, `MenuRegistry`, `SettingsPageRegistry` |
| `fv_clipboard` | `src/clipboard` | Gui | focus | `TemporaryClipboard` (stage, restore, prefetch, selection capture, pre-render) |

Widgets (`ui` kind unless marked feature):

| Target | Dir | Holds |
|---|---|---|
| `fv_ui_theme` | `src/ui_theme` | palette, QSS pipeline, `ThemeManager`, font scale, `WidgetStyleFilter`, tooltip style, icons, `theme.qss` and Tabler SVGs |
| `fv_ui_widgets` | `src/ui_widgets` | the ~40 shared widgets of a02 |
| `fv_ui_shell` (feature) | `src/ui_shell` | `Application`, `Bootstrap`, `SingleInstance`, `LoadingScreen`, `SystemTray`, `MainWindow`, `TitleBar`, `FooterBar`, geometry and page fit, compact view |
| `fv_ui_controller` (feature) | `src/ui_controller` | `UiController`, `OverlayRouter`, `ApplicationController` (Qt glue over appcore and dictation), notices, about, quit/busy confirms, download progress presenter |
| `fv_ui_overlay` (feature) | `src/ui_overlay` | `WaveformOverlay`, 11 looks, moments, language menu, `RecordingLookPreview` |
| `fv_ui_record` (feature) | `src/ui_record` | Quick Record, Upload File, Scratchpad, compact controller, reading window, `MicrophoneCheck`, `CaptureDelay` |
| `fv_ui_history` (feature) | `src/ui_history` | sidebar, cards, entry dialog, playback, export dialog base, calendar, stats window |
| `fv_ui_hotkeys` | `src/ui_hotkeys` | shortcut capture fields, Hotkeys page rows |
| `fv_ui_settings` (feature) | `src/ui_settings` | Settings shell, NavRail pages, search palette, binder, Home, General/Recording/Appearance/Developer, API keys, Backup page and calendar |
| `fv_ui_personalize` (feature) | `src/ui_personalize` | a13 pages and dialogs |
| `fv_ui_downloads` (feature) | `src/ui_downloads` | a15 Downloads page, Voice model page, consent and runtime dialogs |
| `fv_ui_setup` (feature) | `src/ui_setup` | a16 update dialog, notice, guided setup, OpenWhisper import page |
| `fv_ui_automation` (feature) | `src/ui_automation` | a21 Actions, Hooks, API & MCP pages, AgentPicker |
| `fv_ui_snap` (feature) | `src/ui_snap` | a22 Snap overlay, toolbar, live drawing, Win32 capture plumbing |
| `fv_ui_walkthrough` (feature) | `src/ui_walkthrough` | a23 tab, live column, storyboard, library dialog |
| `fv_ui_meeting` (feature) | `src/ui_meeting` | a31 tab, past meetings, dialogs, After End and Fast judgments pages |
| `fv_ui_remote` (feature) | `src/ui_remote` | a18 Remote page, pair dialog, Host Mode dashboard |

Executables: `openwhisper-cpp` (`src/app`, GUI subsystem, links every feature with
`WHOLE_ARCHIVE`; `--self-test-gui` checks resources and plugins offscreen),
`openwhisper-cpp-cli` (`src/cli`, console subsystem, Qt-free: `--version`,
`--diagnostics`, `--self-test`, `--api`, `--channel`, `transcribe`). Later:
`openwhisper-cpp-updater` (`src/updater`, phase 2 of a16).

Test support: `fv_testkit` (temp data root, `FakeClock`, `ManualExecutor`,
Qt offscreen main for Catch2), `fv_golden` (vector loader and comparators),
`fv_screenshot` (tree compare, pixel compare, diff images).

The current `src/app` bench (`MainWindow`, `EngineWorker`) is retired by
M0.11 and M1.16: its EngineWorker role moves into `fv::speech::SpeechService`
plus a thin Qt adapter in `ui_controller`.

### 2.4 Areas to targets

| Area | Targets |
|---|---|
| a01 app shell | base (identity, paths), log (logging, crash, diagnostics, profiler), process (`ProcessEnv`), text (app styles), ui_shell, app, cli |
| a02 UI foundation | ui_theme, ui_widgets, uimath |
| a03 UI controller | appcore, ui_controller |
| a04 runtime services | dictation (transcription, command, hotkeys, streaming), models (model/component runtime core), update (updater schedule), walkthrough, meeting, remote (their runtimes), ui_controller (Qt glue) |
| a05 recording/audio | dsp, audio, recorder, dictation (pipeline, preview, incremental, languages), history (stats), ui_record (microphone check, capture delay) |
| a06 hotkeys/paste/focus | hotkeys, focus, clipboard, ui_hotkeys |
| a07 overlays | uimath, ui_overlay |
| a08 record/upload tabs | dictation (batch types and text rules), ui_record |
| a09 history/stats | db, history, ui_history |
| a10 settings services | settings, credentials, backup |
| a11 settings dialog | ui_settings |
| a12 text AI/personalize | text, llm |
| a13 personalize UI | ui_personalize |
| a14 models/components | download, archive, models, speech |
| a15 models/downloads UI | ui_downloads |
| a16 updater/setup | update, ui_setup (updater exe later) |
| a17 remote engine services | net (transport, LAN address), remote |
| a18 remote engine UI | ui_remote |
| a19 actions/automation | automation, process, cli (`--channel`) |
| a20 agent API/MCP | agentapi, agents, process, cli (`--api`) |
| a21 actions UI | ui_automation |
| a22 walkthrough services/snap | walkthrough, documents, ui_snap |
| a23 walkthrough UI | ui_walkthrough |
| a24 meeting engine | meeting |
| a25 capture/ASR/diarize | meeting_capture, diarize, dsp (fbank) |
| a26 meeting agent | meeting_agent, agents |
| a27 sidecars | meeting_agent (native loop), llm (streaming tool calls) |
| a28 meeting state/persist | meeting_state |
| a29 meeting intelligence | meeting_intel |
| a30 meeting web server | meeting_web, net; dashboard bundle in `webui/` |
| a31 meeting Qt UI | ui_meeting |

---

## 3. Cross-cutting services and their interfaces

All of these land in M0 or the first wave of M1, each with a fake in
`<dir>/testing/`. The headers below are the binding shape; workers may add
members, never change these.

### 3.1 Identity and paths (`base/identity.h`, `base/paths.h`)

```cpp
namespace fv {
// Every name that differs between OpenWhisper_CPP and OpenWhisper, in one place.
struct AppIdentity {
    std::string_view product;          // "OpenWhisper_CPP"
    std::string_view data_dir_name;    // "OpenWhisper_CPP"
    std::string_view env_prefix;       // "OPENWHISPER_CPP_"
    std::string_view settings_file;    // "openwhisper_cpp_settings.json"
    std::string_view database_file;    // "openwhisper-cpp.db"
    std::string_view log_file;         // "openwhisper-cpp.log"
    std::string_view credential_service;  // "OpenWhisper_CPP"
    std::string_view backup_extension; // ".fvbackup"
    std::string_view instance_key_prefix; // "openwhisper-cpp-ui"
};
const AppIdentity& identity();         // OpenWhisper_CPP
const AppIdentity& legacy_identity();  // OpenWhisper, used only by import and wire names

class Paths {
public:
    static Paths resolve(const AppIdentity&, const EnvReader&);  // a01 data_root rules
    std::filesystem::path data_root() const;        // OPENWHISPER_CPP_DATA_DIR wins
    std::filesystem::path local_app_dir() const;    // %LOCALAPPDATA%\OpenWhisper_CPP
    std::filesystem::path components_root() const;
    std::filesystem::path speech_models_root() const;
    std::filesystem::path user_file(std::string_view name) const;  // data_root()/name
    std::filesystem::path env_file() const;
};
}
```

Workers never write a product name, file name or environment variable name as
a literal; they take it from `identity()`. User-visible strings are the
exception (section 8.6).

### 3.2 Clock, executors, cancel, signals (`base/clock.h`, `base/executor.h`, `base/signal.h`)

```cpp
namespace fv {
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::steady_clock::time_point steady() const = 0;
    virtual std::chrono::system_clock::time_point utc() const = 0;
    virtual const std::chrono::time_zone* zone() const = 0;  // tests pin it
};
Clock& system_clock();

using Task = std::function<void()>;
class TimerHandle { public: void cancel(); bool active() const; /* RAII: cancels on destroy */ };

class Executor {
public:
    virtual ~Executor() = default;
    virtual void post(Task) = 0;
    virtual TimerHandle post_after(std::chrono::milliseconds, Task) = 0;
    virtual bool is_current() const = 0;
};

enum class ShutdownMode { CancelQueued, Drain };
class ThreadPool final : public Executor {     // named threads, fixed size
public:
    ThreadPool(std::string name, int threads);
    void shutdown(ShutdownMode, std::chrono::milliseconds wait);
};

class CancelToken {                             // copyable view of a CancelSource
public:
    bool cancelled() const;
    bool wait_for(std::chrono::milliseconds) const;  // true when cancelled
    Subscription on_cancel(Task);               // runs once, on the cancelling thread
};
class CancelSource { public: CancelToken token() const; void cancel(); };

// Thread-safe multicast. Delivery is always posted to the subscriber's
// executor, never inline, so emitting under a lock cannot re-enter.
template <class... Args> class Signal {
public:
    Subscription connect(Executor&, std::function<void(Args...)>);
    void emit(const Args&...) const;
};
}
```

Fakes: `FakeClock` (settable, advances timers on `ManualExecutor`),
`ManualExecutor` (`run_all()`, `advance(ms)`). Python's `QTimer.singleShot`
becomes `executor.post_after`; the Qt implementation (`QtUiExecutor` in
appkit) uses `QTimer` with the same `Qt::TimerType`.

### 3.3 Settings store (`settings/setting.h`, `settings/keys.h`, `settings/store.h`)

```cpp
namespace fv {
using Json = nlohmann::ordered_json;   // insertion order kept: byte-compatible files

template <class T> class Setting {     // Bool, Int(strict/valid/clamp), Float, Str,
public:                                // Choice, IntChoice, List, Dict, Optional*, Raw
    std::string_view key() const;
    T read(const Json& snapshot) const;            // unset or invalid -> default
    std::optional<T> stored(const Json& snapshot) const;  // never the default
    T default_value() const;
};

namespace keys {                       // every OpenWhisper key except dropped ones,
inline const BoolSetting kMinimizeTray{"minimize_tray", true};   // declared in M0
inline const ChoiceSetting kUiTheme{"ui_theme", "dark", UiTheme::choices};
// ... about 170 keys, plus OpenWhisper_CPP's own: kParakeetRuntimeDir, kParakeetRuntimeVariant
}

class SettingsStore {
public:
    explicit SettingsStore(std::filesystem::path file);
    std::shared_ptr<const Json> snapshot() const;         // cached, copy-free
    template <class T> T get(const Setting<T>&) const;
    Json update(const Json& patch, std::span<const std::string_view> remove = {});
    Json mutate(const std::function<void(Json&)>&);       // throws -> nothing written
    Subscription watch(std::vector<std::string_view> keys, Executor&,
                       std::function<void(const Json& changed)>);
    bool is_new_install() const;
};
}
```

The 186 OpenWhisper declarations are ported in one package (M0.7) with a
golden vector of the defaults table and the `JUNK` battery, so no later
package edits `keys.h`. Resolvers live with their consumers
(`dictation/resolve.h`, `meeting_state/resolve.h`); only resolvers used by
three or more libraries live in `settings/resolve.h`.

### 3.4 Logging and diagnostics (`base/log.h`, `log/*`)

```cpp
namespace fv::log {
enum class Level { Debug, Info, Warning, Error, Critical };
struct Record { Level level; std::string_view logger; std::string_view message;
                std::string_view exception_type; std::source_location where; };
class Sink { public: virtual ~Sink() = default; virtual void write(const Record&) = 0; };
void add_sink(std::shared_ptr<Sink>);
void write(Level, std::string_view logger, std::string_view message,
           std::string_view exception_type = {});
}
#define FV_LOG_INFO(logger, ...)  ::fv::log::write(::fv::log::Level::Info, logger, std::format(__VA_ARGS__))
```

The facade sits in `fv_base` so every library can log from day one; the sinks
(`RotatingFileSink` 5 MiB x 3, `FailureJournalSink` that never formats the
message, console sink), `MetricsCapture`, `DiagnosticsReport`,
`StartupProfiler` and `CrashHandler` live in `fv_log`. Logger names keep
OpenWhisper's dotted component names (`services.recorder`, `ui_qt.settings`) so
the failure journal's component mapping ports unchanged. Rule: dictated text,
selections, prompts, keys and URLs with queries never reach a log call.

### 3.5 Credentials (`credentials/store.h`)

```cpp
namespace fv {
struct CredentialStatus { bool available; std::string backend_name; std::string reason; };
class CredentialBackend {
public:
    virtual ~CredentialBackend() = default;
    virtual std::optional<std::string> get(std::string_view service, std::string_view name) = 0;
    virtual void set(std::string_view service, std::string_view name, std::string_view value) = 0;
    virtual bool remove(std::string_view service, std::string_view name) = 0;
    virtual CredentialStatus probe() = 0;
};
class CredentialStore {        // caches, serializes backend calls, never logs values
public:
    CredentialStore(std::shared_ptr<CredentialBackend>, const AppIdentity&);
    std::optional<std::string> get(std::string_view name);
    void set(std::string_view name, std::string_view value);
    bool remove(std::string_view name);
    CredentialStatus status();
};
enum class CredentialSource { Store, Environment, DotEnv, None };
struct Resolved { std::optional<std::string> value; CredentialSource source; };
Resolved resolve_credential(CredentialStore&, const EnvReader&, const DotEnv&, std::string_view name);
std::string mask_key(std::string_view key);
}
```

Windows backend: `CredReadW`/`CredWriteW` with keyring's WinVault target
naming (`<name>@<service>`, falling back to `<service>`), verified against the
keyring source by a golden test. Variable names are OpenWhisper_CPP's
(`OPENWHISPER_CPP_MCP_TOKEN`); provider names stay standard (`OPENAI_API_KEY`).

### 3.6 HTTP client (`http/client.h`)

```cpp
namespace fv::http {
struct Request {
    std::string method = "GET", url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds connect_timeout{10'000}, total_timeout{60'000};
    bool follow_redirects = false;
    std::optional<std::string> pinned_address;   // CURLOPT_RESOLVE: SSRF guard keeps SNI
    bool bypass_proxy = false;
    std::optional<std::uint64_t> range_from;
    std::size_t max_body_bytes = 64u << 20;
};
struct Response { int status = 0; std::vector<std::pair<std::string,std::string>> headers;
                  std::string body; std::string effective_url; };
class BodySink { public: virtual ~BodySink() = default; virtual bool write(std::span<const char>) = 0; };
enum class ErrorKind { Timeout, Dns, Connect, Tls, Cancelled, TooLarge, Protocol };
struct Error { ErrorKind kind; std::string message; };

// Blocking; call from a worker thread, never from the UI executor.
class Client {
public:
    virtual ~Client() = default;
    virtual std::expected<Response, Error> send(const Request&, const CancelToken&,
                                                BodySink* stream = nullptr) = 0;
};
std::shared_ptr<Client> make_curl_client();   // one shared curl share handle
}
```

### 3.7 HTTP and WebSocket server, WS client (`net/*`)

```cpp
namespace fv::net {
class Reactor {                 // owns the asio io_context and its threads ("net")
public:
    explicit Reactor(int threads = 2);
    Executor& executor();       // handlers run here; never block in them
    void stop(std::chrono::milliseconds grace);
};
struct HttpRequest { std::string method, path; Query query; Headers headers;
                     std::string body; Peer peer; };
class Responder {               // shared_ptr; may complete later from any thread
public:
    virtual void send(int status, Headers, std::string body) = 0;
    virtual void send_file(std::filesystem::path, std::string content_type,
                           std::optional<ByteRange>) = 0;      // 206/416 handled
    virtual std::shared_ptr<EventStream> start_sse(Headers) = 0;
};
struct TlsIdentity { std::string cert_pem, key_pem; bool disable_tickets = false; };
struct Listen { std::string address; std::uint16_t port; std::optional<TlsIdentity> tls; };
class WsConnection {
public:
    virtual void send_text(std::string) = 0;
    virtual void send_binary(std::vector<std::byte>) = 0;
    virtual void close(std::uint16_t code, std::string reason) = 0;
    virtual std::size_t queued_bytes() const = 0;
};
class HttpServer {
public:
    static std::unique_ptr<HttpServer> create(Reactor&);
    void route(std::string method, std::string pattern,
               std::function<void(const HttpRequest&, std::shared_ptr<Responder>)>);
    void websocket(std::string path, WsHandlers);   // upgrade on the same port
    std::expected<Endpoint, std::string> listen(const Listen&);
    void stop(std::chrono::milliseconds grace);     // aborts streaming after grace
};
std::unique_ptr<WsClientConnection> ws_connect(Reactor&, WsClientOptions);  // pin-before-token
TlsIdentity generate_self_signed_p256(std::string_view common_name);
}
```

Every server in the app (meeting dashboard, MCP, live API and SSE, History
REST, remote engine host, loopback MCP for agents) is an `HttpServer` on the
one `Reactor`. Handlers are pure functions in their own libraries
(`agentapi::history_routes`, `meeting_web::DashboardApi`), so they are tested
without sockets.

### 3.8 Database (`sql/db.h`, `db/database.h`)

```cpp
namespace fv::sql {
class Connection {
public:
    static Connection open(const std::filesystem::path&, OpenMode);  // ro, rw, create
    Statement prepare(std::string_view sql);
    void exec(std::string_view sql);
    Transaction begin(TxMode = TxMode::Deferred);   // TxMode::Immediate where Python uses it
    void backup_to(Connection& target, const CancelToken&);
    void create_function(std::string_view name, int args, ScalarFn);
};
}
namespace fv {
class Database {                 // one writer connection behind a mutex, pooled readers
public:
    static Database open_or_create(const std::filesystem::path&);  // migrates to v18
    template <class F> auto write(F&& fn);  // fn(sql::Connection&) under the writer lock
    template <class F> auto read(F&& fn);   // fn(sql::Connection&) on a reader
    int schema_version() const;
};
}
```

`fv_db` owns the whole schema and every migration (history, stats, meeting
tables, FTS5 triggers, `models.py` tables) so the schema is never a shared
edit. Repositories (`HistoryRepository`, `MeetingRepository`,
`StatsRepository`) live in their libraries and use `db/schema.h` constants.

### 3.9 Process launcher and shell (`process/launcher.h`, `process/shell.h`)

```cpp
namespace fv {
struct ProcessSpec {
    std::filesystem::path program;
    std::vector<std::string> args;          // UTF-8, quoted by the launcher
    std::filesystem::path cwd;
    std::map<std::string, std::string> env; // from ProcessEnv::external_env()
    bool own_tree = true;                   // job object: closing it kills the tree
    bool pipes = false;
    bool disown_on_clean_exit = false;
};
class Process {
public:
    virtual std::uint32_t pid() const = 0;
    virtual void write_stdin(std::string_view) = 0;
    virtual void on_line(LineSink stdout_sink, LineSink stderr_sink) = 0;  // reader threads
    virtual std::optional<int> wait(std::chrono::milliseconds) = 0;
    virtual void kill_tree() = 0;
};
class ProcessLauncher {
public:
    virtual ~ProcessLauncher() = default;
    virtual std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec&) = 0;
};
class Shell {                                // open URL, reveal file, ShellExecute
public:
    virtual bool open_url(std::string_view) = 0;
    virtual bool reveal(const std::filesystem::path&) = 0;
    virtual bool launch_via_explorer(const std::filesystem::path&, std::string_view args) = 0;
};
}
```

Windows: `CreateProcessW` with `CREATE_SUSPENDED | CREATE_NO_WINDOW`, job
assignment before resume (a20), `CancelSynchronousIo` on pipe threads (a19).
`QProcess` is rejected because it cannot own a job object before the child
runs, which a19 and a20 depend on.

### 3.10 Audio input and output (`audio/backend.h`)

```cpp
namespace fv::audio {
struct DeviceInfo {
    std::string id;              // backend-native id, stable within a session
    std::string name;            // endpoint friendly name, UTF-8
    std::string host_api;        // "Windows WASAPI", "MME", ... (PortAudio names)
    int channels = 1, default_rate = 48000;
    bool is_default = false, is_loopback = false;
};
struct DeviceKey { std::string name, hostapi; };   // the saved audio_input_priority shape
struct StreamConfig { std::string device_id; int rate = 44100; int channels = 1;
                      int frames_per_block = 1024; bool loopback = false; };
struct BlockStatus { bool overflow = false, underflow = false; };
// Runs on the audio thread: no allocation, no locks, no logging.
using CaptureFn = void (*)(void* user, const std::int16_t* frames, std::size_t count, BlockStatus);
class Stream { public: virtual ~Stream() = default; virtual bool active() const = 0;
               virtual void abort() = 0; };
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<DeviceInfo> inputs() = 0;
    virtual std::vector<DeviceInfo> loopbacks() = 0;
    virtual std::vector<DeviceInfo> outputs() = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string>
        open_capture(const StreamConfig&, CaptureFn, void* user,
                     std::function<void(std::string)> on_lost) = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string>
        open_playback(const StreamConfig&, PlaybackSource&) = 0;
    virtual Subscription on_devices_changed(Executor&, Task) = 0;
};
std::shared_ptr<Backend> make_miniaudio_backend();
// The a05 matching ladder: saved keys (any OpenWhisper host API) -> current devices.
std::vector<DeviceInfo> ranked_candidates(std::span<const DeviceKey> priority,
                                          std::span<const DeviceInfo> devices);
}
```

`FakeBackend` replays scripted blocks, losses and device changes, which is
how the recorder's switch and post-roll tests port.

### 3.11 Events between layers

There are three kinds, and each has one mechanism:

- **Core to core and core to UI:** `fv::Signal<...>` members on the producing
  class, documented in its header. The producer owns the event types.
- **UI to UI:** ordinary Qt signals between widgets and their feature's
  controller.
- **Core to Qt:** `fv::qtConnect(QObject* context, signal, slot)` from
  appkit, which posts to `context`'s thread and drops the call if `context`
  was destroyed. This one helper replaces the five ad hoc relay-with-token
  patterns the maps describe (a09, a11, a13, a21, a31) and the crash class they
  were fixing.

The product event bus of a19 (`automation::EventBus`, SSE, webhooks) is a
feature, not plumbing; features publish to it through `automation/events.h`.

### 3.12 Application context and features (`appkit/feature.h`)

```cpp
namespace fv {
class AppContext {
public:
    template <class T> T& service();              // throws naming T if absent
    template <class T> void provide(std::shared_ptr<T>);
    Executor& ui();  Executor& jobs();  Executor& persistence();  Executor& io();
    ScreenRegistry& screens(); MenuRegistry& menus(); SettingsPageRegistry& settingsPages();
    ShutdownPlan& shutdown(); CancelRouter& cancel(); ActivityState& activity();
};
class Feature {
public:
    virtual ~Feature() = default;
    virtual std::string_view id() const = 0;
    virtual std::vector<std::string_view> requires() const { return {}; }
    virtual void provide(AppContext&) {}           // phase 1: construct services
    virtual void install(AppContext&) = 0;         // phase 2: wire, register UI
};
#define FV_FEATURE(Class) /* static registrar; the module is linked WHOLE_ARCHIVE */
}
```

The main window builds tabs from `ScreenRegistry` sorted by fixed order
constants declared in `appkit/order.h` in M0 (Quick Record 100, Upload 200,
Walkthrough 300, Meeting 400; side panels; File/View/Help menu items; tray
menu items; Settings chapters and pages with their a11 destination ids). This
header is written once, from a01/a11/a03, so no later package edits it.

---

## 4. Threading model

| Thread | Name | Owner | Runs |
|---|---|---|---|
| GUI | `main` | Qt | widgets, every "UI actor" core object (controllers, runtimes, state machines) through `ctx.ui()` |
| Speech | `speech` | `SpeechService` | every NeMo call: load, warm-up, decode; one at a time by construction |
| Audio callbacks | miniaudio's | audio backend | `CaptureFn` only: copy into SPSC rings, update atomics |
| Recorder writer | `recorder-journal` | `Recorder` | ring drain, journal append, checkpoints |
| Recorder watcher | `recorder-watch` | `Recorder` (per recording) | post-roll wait, stream close, metrics |
| Hook | `hotkey-hook` | `KeyboardHook` | Win32 message loop for `WH_KEYBOARD_LL`/`WH_MOUSE_LL`; decides suppression inside the callback |
| Dispatcher | `hotkey-dispatch` | `HotkeyRuntime` | ordered delivery of matched hotkeys (OpenWhisper's `OrderedDispatcher`) |
| Focus | `focus-capture` | `FocusCapture` | UIA and foreground reads; abandonable on deadline |
| Pools | `jobs` (2), `persistence` (1, drained on quit), `components` (1), `library` (1), `io` (4) | `ApplicationController` | blocking work: decode orchestration, HTTP, file and DB I/O |
| Network | `net-0`, `net-1` | `net::Reactor` | all servers and WS clients; handlers never block |
| Meeting | `meeting-spool-<ch>`, `meeting-watchdog`, `meeting-asr`, `meeting-diarize`, `meeting-agent` | meeting libs | spool writers, capture health, ASR queue (submits to `speech`), ORT inference, scheduler |
| Metrics | `metrics-writer` | `MetricsCapture` | coalesced metrics file writes |
| Crash | `crash-watch` | `CrashHandler` | pre-created thread that writes the minidump |

Rules for crossing threads (a worker applies them mechanically):

1. **Every class declares its affinity** in a header comment: `// Affinity:
   UI actor`, `thread-safe`, or `owned thread <name>`. A UI actor's public
   methods are called only on `ctx.ui()`; tests drive them on a
   `ManualExecutor`.
2. **Cross by posting, never by calling.** `Signal::emit` posts; results from
   pools come back with `ui.post`. No code waits on a future on the GUI thread.
   The only blocking allowed on the GUI thread is a mutex held for at most a
   few microseconds by design.
3. **No lock is held while calling out**: no callback, signal emit, log call to
   a slow sink, or backend call happens under a mutex that another thread
   needs to make progress.
4. **Lock order** where several are needed is written in `appcore/locks.h`:
   capture, then meeting, then job (a03 backup acquire); acquired together
   with `std::scoped_lock`.
5. **Realtime threads** (audio callbacks, LL hook callback) touch only
   preallocated memory and atomics. A test helper (`testkit::RealtimeGuard`)
   fails a debug test when such a callback allocates.
6. **The speech thread is reached only through `SpeechService::submit`.**
   This replaces CLAUDE.md's "only through `EngineWorker`'s queued signals"
   with an equivalent that Qt-free callers (meeting, remote host, agent API)
   can use; M0.1 updates CLAUDE.md.
7. **Widgets are never touched off the GUI thread and never captured by a
   worker lambda**; workers capture a `std::weak_ptr` to a core object or go
   through `qtConnect`.
8. **Hard exit after updater hand-off uses `std::_Exit`** so no static
   destructor touches the NeMo runtime.

---

## 5. Speech backends

### 5.1 One interface

```cpp
namespace fv::speech {
struct PcmClip {                       // mono float at 16 kHz unless rate says otherwise
    std::shared_ptr<const std::vector<float>> samples;
    std::size_t begin = 0, end = 0;
    int rate = 16000;
    double offset_s = 0.0;             // where begin sits in the recording
};
enum class Lane { Final = 0, Meeting = 1, Window = 2, Preview = 3, Warmup = 4 };
struct DecodeJob {
    std::variant<PcmClip, std::filesystem::path> input;  // path only for cloud uploads
    RecognitionOptions options;        // language, speech contexts
    Lane lane = Lane::Final;
    std::string coalesce_key;          // newer job with the same key replaces a queued one
    CancelToken cancel;
};
struct DecodeResult { Transcript transcript; double decode_s = 0; std::uint64_t generation = 0; };
struct Capabilities { bool windows = true; bool live_preview = true; bool accepts_files = false;
                      double max_window_s = 30.0; std::vector<std::string> languages; };
enum class ReadyState { Ready, Loading, ModelMissing, RuntimeMissing, NeedsKey, Offline, Failed };
struct Readiness { ReadyState state; std::string device_info; std::string message; };

class Backend {                        // all methods run on the speech thread
public:
    virtual ~Backend() = default;
    virtual std::string_view id() const = 0;          // "parakeet", "api", "remote"
    virtual Capabilities capabilities() const = 0;
    virtual Readiness load(const CancelToken&) = 0;
    virtual DecodeResult decode(const DecodeJob&) = 0; // throws EngineError
    virtual void release() = 0;
};

class SpeechService {                  // thread-safe facade; owns the "speech" thread
public:
    SpeechService(BackendFactory, Executor& notify);
    std::future<DecodeResult> submit(DecodeJob);
    void select(BackendConfig);        // debounced reload is appcore's EngineReloadMachine
    EngineLease lease(std::string_view holder);  // blocks reloads while held
    Readiness readiness() const;
    Signal<Readiness> readiness_changed;
    Signal<bool> busy_changed;
};
}
```

Implementations: `ParakeetBackend` (`fv_speech`, over `fv::Recognizer`),
`CloudSpeechBackend` (`fv_speech`, OpenAI/Groq/own server over `http::Client`,
with a05's large-file splitting and a14's chunk upload concurrency of 3),
`RemoteSpeechBackend` (`fv_remote`, over the remote WS client). The backend
table of a03 is a `BackendFactory` keyed by `selected_model` values
`parakeet`, `api`, `remote`.

### 5.2 Scheduling on the speech thread

The queue is ordered by lane, then by submission. `Final` always runs next,
which reproduces OpenWhisper's "the final pass starts at once"; `Preview`
jobs coalesce per recording so only the newest window waits; `Warmup` runs
only when nothing else is queued. A meeting's `Meeting` lane cannot starve
dictation because the a03 gates refuse dictation during a meeting anyway; the
lane order matters for walkthrough section decodes and uploads that run while
a meeting finalizes.

### 5.3 Live preview by window decoding

`dictation::PreviewSession` (UI actor) receives 16 kHz blocks from the
recorder's preview ring (one `fft_resample` implementation shared by the live
and the file path, so incremental windows are bit-identical by construction),
cuts 3.0 s windows with 0.75 s overlap, submits them on the `Preview` lane
with `coalesce_key = "preview:<job id>"`, and joins results with the ported
`append_preview_text`. Stop never decodes the preview tail; the kept tail is
decoded only if the final pass comes back empty (a04's lazy `finalize_preview`).
`IncrementalDictation` submits completed 30 s windows (quietest 0.1 s split
after 24 s) on the `Window` lane and checks the CRC fingerprint before reusing
them in the final transcript. Cloud and remote backends report
`live_preview = false` unless the remote host advertises it, and the preview
reason strings of `streaming.preview_unavailable_reason` say why.

### 5.4 Cancel

The C ABI has no abort, so a decode is never longer than one window: every
input longer than `max_window_s` is decoded window by window, with the cancel
token checked between windows. The longest uncancellable stretch is one
30-second window (well under two seconds on CPU). Cancelled jobs still return
through the future, with `EngineError("Transcription canceled")`.

### 5.5 Lifetime

- **Runtime:** exactly one NeMo runtime library is loaded per process, chosen at
  first load from the settings (`kParakeetRuntimeVariant`: cpu, cuda, vulkan).
  The CPU and GPU builds ship DLLs with the same base names (`ggml*.dll`), so
  loading a second variant in one process would bind it to the first one's
  dependencies. Choosing another variant saves the setting and asks for a
  restart ("Restart OpenWhisper_CPP to switch to the GPU runtime"). The component
  store refuses to update or remove the loaded runtime's directory and stages
  the change for the next start.
- **Model:** `ParakeetBackend` destroys and recreates the `Recognizer` when the
  model or GPU index changes. The `EngineReloadMachine` (appcore) keeps a03's
  400 ms debounce, re-arming while recording or transcribing, and warm-up after
  load.
- **Sharing:** dictation, uploads, rule dictation, walkthrough sections,
  meetings (live preview, chunk ASR, offline re-decode) and the remote host
  all submit to the same `SpeechService`. OpenWhisper's release/restore model
  lease for meetings becomes `EngineLease`: the meeting holds a lease so no
  reload can start under it, and nothing is unloaded. a03's "Released while
  Meeting Mode runs" status disappears; that is a deliberate difference.

---

## 6. Third-party choices

All are in `vcpkg.json` from M0.1 (pinned baseline); workers do not add
dependencies.

| Need | Choice | Runner-up | Why |
|---|---|---|---|
| Audio capture, playback, WASAPI loopback | **miniaudio** 0.11 | PortAudio 19.7 | One single-header library covers microphone, loopback and playback with the same callback model; vcpkg's PortAudio has no WASAPI loopback, so meetings would need a second raw-WASAPI path anyway. The cost (device names differ from PortAudio's MME names) is handled by the a05 matching ladder plus a one-time key rewrite to `{name, "Windows WASAPI"}`. |
| Audio file decoding (uploads) | **Media Foundation** behind `AudioFileDecoder`, miniaudio decoders as fallback | FFmpeg | FFmpeg is not in the pinned port list and is a heavy, LGPL build; Media Foundation already decodes MP3, AAC/M4A, WMA, FLAC and WAV on Windows. |
| HTTP/WS server, WS client | **Boost.Beast + Asio** with OpenSSL | IXWebSocket + cpp-httplib | One library gives HTTP, WS on the same port, SSE, streamed Range responses, a WS client and TLS context control (`SSL_CTX_set_num_tickets(0)` for the remote engine); two libraries would give two threading models. Beast compiles slowly, so it is confined to `fv_net`'s `.cpp` files behind pimpl. |
| TLS and certificates | **OpenSSL** | Windows CNG/Schannel | Needed for self-signed P-256 generation and pinning in a17; Schannel cannot generate certificates portably. |
| HTTP client | **libcurl** (Schannel on Windows) | QNetworkAccessManager | Blocking calls from worker threads with a cancel callback, `CURLOPT_RESOLVE` for the SSRF-safe pinned webhooks (a20), Range resume and streaming bodies; QNAM needs an event-loop thread and cannot pin an address while keeping SNI. |
| JSON | **nlohmann-json** (`ordered_json`) | QJsonDocument | Insertion order and int/float fidelity are needed for byte-compatible settings and exports; `QJsonObject` sorts keys and merges int and double. |
| SQLite | **sqlite3 C API** (vcpkg, `fts5`) | QtSql | Qt-free cores, the online backup API, custom functions, explicit `BEGIN IMMEDIATE` and FTS5 certainty. |
| Zip and .docx | **minizip-ng** (zlib only) + **pugixml** | libzip | Streaming Zip64 read and write for multi-GB backups, also used for DOCX packages and Office text extraction; pugixml writes OOXML parts. |
| tar.gz (Linux runtimes) | own reader over **zlib** | libarchive | Only reading, only for NeMo archives, and the safety filter must be hand-written either way. |
| PDF | **QPdfWriter + QTextDocument** (QtGui) | libharu | a22's pagination is defined in terms of `QTextDocument` layout; matching it in another engine would be a rewrite. |
| Markdown | **`QTextDocument::setMarkdown`** (GitHub dialect) | md4c directly | It is what OpenWhisper renders with, and Qt bundles md4c already. |
| Regex and Unicode | **PCRE2** (UTF + UCP) + generated tables in `fv_pyport` | RE2, ICU | Python patterns use lookbehinds (RE2 has none); casefold and `\w` tables generated from Python 3.12's `unicodedata` match Python exactly, which ICU (a newer Unicode) would not. |
| Resampling and FFT | **kissfft** with a port of `fft_resample` | pffft, libsamplerate | `fft_resample` needs FFTs of arbitrary length (pffft only does 2^a 3^b 5^c sizes); a sinc resampler would change numerics that incremental decoding and diarization parity rely on. |
| Speaker diarization inference | **ONNX Runtime official prebuilt DLL, loaded by path** through the C API (header vendored) | vcpkg `onnxruntime` linked | Same pattern as NeMo: downloaded as a pinned component, never on the build path; the vcpkg port builds ORT from source, which would add tens of minutes to every cold worker build. |
| Credentials | **WinCred** directly (`wincred.h`) | qtkeychain | qtkeychain from vcpkg depends on Qt (would compile Qt), and keyring's target naming must be matched exactly. |
| Crash reporting | **own handler**: `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` from a pre-created thread, plus a text crash log | crashpad | Crashpad needs a handler process and a server; a local minidump plus the log matches OpenWhisper's `faulthandler` file. |
| Global hotkeys, input injection | **Win32**: `WH_KEYBOARD_LL`, `WH_MOUSE_LL`, `SendInput`; UIA via COM with **wil** | `RegisterHotKey` | Suppression, hold tracking and keypad keys need the LL hook; `RegisterHotKey` cannot see key-up or suppress selectively. |
| Screenshots | **`QScreen::grabWindow(0)`** per screen + `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` | Windows.Graphics.Capture (cppwinrt) | It is what OpenWhisper's Snap freezes with; WGC is a later gain for HDR displays. |
| Logging | **own** facade and sinks (~400 lines) | spdlog | The failure journal and metrics need custom sinks either way; avoiding spdlog and fmt keeps the base closure at one port. |
| Tests | **Catch2** for every test target, Qt Test as a helper | Qt Test runner | One runner, one filter syntax and one CTest discovery for all ~60 test executables. |

Not used: spdlog, fmt, simdjson, ICU, libsamplerate, speexdsp, libsndfile,
opus, drogon, crow, uwebsockets, libhv, mdns (discovery uses plain UDP sockets
in `fv_net`).

---

## 7. Data and identity

### 7.1 Folder layout (`%LOCALAPPDATA%\OpenWhisper_CPP`, or `OPENWHISPER_CPP_DATA_DIR`)

```
openwhisper_cpp_settings.json        OpenWhisper key set and JSON shape, indent 2
openwhisper-cpp.db                   schema v18, identical to openwhisper.db
openwhisper-cpp.log(.1..3), openwhisper-cpp.crash.log, crash-*.dmp
diagnostic-failures.json, diagnostic-metrics.json
recorded_audio.wav, .recorded_audio-recovery/<32 hex>/...
recordings/, meetings/, captures/, remote_records/, scratchpad.txt
mcp_event_subscriptions.json, backup-preferences.json
.openwhisper-cpp-data.lock, .openwhisper-cpp-restore/
components/<id>/{manifest.json,.installed,...}, components/.cache, .staging
speech-models/<key>/ + installed.json
models/speaker-id/voxceleb_resnet34_LM.onnx
```

Every file and folder name not containing the product name is OpenWhisper's,
unchanged. Install folder: `%LOCALAPPDATA%\Programs\OpenWhisper_CPP`.

### 7.2 Settings format

One JSON object with OpenWhisper's keys and value shapes. Engine keys that
only served dropped engines (`whisper_*`, `custom_whisper_models`,
`meeting_whisper_model`) are dropped on import; `selected_model` accepts
`parakeet`, `api`, `remote` and maps anything else to `parakeet`. New keys:
`parakeet_runtime_variant`, `parakeet_runtime_dir` (explicit override), and
`openwhisper_import` (`{done_at, source_root}`). The current `QSettings`
storage in `src/app` is dropped.

### 7.3 Database

`fv_db` creates schema v18 exactly as OpenWhisper's `database.py` and
`models.py` do (same tables, AUTOINCREMENT, FTS5 external-content triggers,
`schema_version`), and runs the v6..v18 migrations so an imported older DB
upgrades. A golden test compares `sqlite_master` of a fresh C++ database with
one produced by OpenWhisper. OpenWhisper_CPP-only data never goes in new columns
of shared tables; if needed, it goes in new tables prefixed `fv_`, which
import ignores and backup carries.

### 7.4 Import from OpenWhisper

Import reuses the backup machinery, so there is one tested code path:

1. Guided setup (or Settings, Backup page) detects `%LOCALAPPDATA%\OpenWhisper`
   (or a chosen folder) and checks that OpenWhisper is not running by trying a
   shared lock on its `.openwhisper-data.lock`.
2. `backup::stage_from_data_root(legacy_identity(), source, identity())` builds a
   restore stage exactly as restoring an `.owbackup` would: SQLite online
   backup of `openwhisper.db` into the stage as `openwhisper-cpp.db`, settings
   sanitized and mapped (dropped keys removed, `selected_model` mapped, paths
   rebased), recordings, meetings, captures, scratchpad and recovery journals
   copied (hard links when on the same volume).
3. The app restarts (exit code 73, as OpenWhisper), and the startup restore gate
   applies the stage under the exclusive data lease, with the existing
   journal and rollback.
4. Credentials: each known variable is read from the `OpenWhisper` service and
   written under `OpenWhisper_CPP`, after the user ticks "Copy API keys".
5. Runtime and models: `models::discover_openwhisper_install()` finds NeMo
   runtime components and Parakeet models; they are hard-linked (copied across
   volumes) into OpenWhisper_CPP's trees and verified against the catalog's
   SHA-256, so neither app can delete the other's loaded files.

`.owbackup` archives restore directly into OpenWhisper_CPP through the same name
mapping. OpenWhisper_CPP writes `.fvbackup` archives with the same layout and format
id `openwhisper-backup` v1, so OpenWhisper's inspect code would read them.

### 7.5 Shared with an OpenWhisper install

Nothing is shared at run time: OpenWhisper_CPP never writes to OpenWhisper's data
root, credential entries or components. On the wire, the remote engine stays
byte-compatible (`/openwhisper/v1/engine`, magics, SAS strings, pairing
shapes), so OpenWhisper_CPP and OpenWhisper computers can pair; those constants live in
`remote/wire.h`, the only place "openwhisper" appears in code outside
`legacy_identity()`. The MCP server name, client header, event types and
dashboard storage keys become OpenWhisper_CPP's.

---

## 8. UI architecture

### 8.1 Theme pipeline

`src/ui_theme/resources/theme.qss` is OpenWhisper's file copied verbatim
(recorded commit in `resources/SOURCE`), with the 59 Tabler SVGs and app
icons under the same relative paths, compiled into `:/ui_theme/`. The pipeline
in `fv::ui::ThemeManager` runs exactly OpenWhisper's order: rewrite `url()` to
`:/ui_theme/...` (before tokens, because `@icon-suffix` sits inside URLs),
substitute `@token`s from `Palette` (96 tokens, dark and light), apply the
font-scale rewrite (`std::nearbyint`, 90/100/115/130 %), set the application
stylesheet and `QPalette`. Golden test: the resolved sheet's SHA-1 equals the
reference dump `app-<theme>-s<scale>.qss` for both themes and all four scales.
OpenWhisper_CPP-only rules go in `src/<dir>/resources/<dir>.qss`, appended in
sorted directory order; `theme.qss` is never edited. Unknown tokens are logged
and left unresolved, never thrown from the style filter. Floor: Qt 6.8 (LTS),
raised from 6.5 so `QStyleHints::setColorScheme` needs no guard and workers
write no version branches.

### 8.2 Shared widgets

`fv_ui_widgets` ports a02's widgets with OpenWhisper's class names
(`Button`, `HotkeyHintFilter`, `SettingTile`, `InfoTile`, `SegmentedBar`,
`NavRail`, `TabbedContentWidget`, `SearchableComboBox`, `WrappedLabel`,
`ElidingLabel`, `AnimatedProgressBar`, `FieldHelpCard`, ...). They land in two
packages (M0.9 leaf widgets, M2 layout and composite widgets) before any page
that uses them.

### 8.3 PyQt classes to C++ classes

- A Python class `Foo(QFrame)` in `ui_qt/widgets/foo_bar.py` becomes
  `class fv::Foo : public QFrame` in `src/<dir>/foo_bar.{h,cpp}`: same class
  name, same base Qt class, same file stem. Python signals become Qt signals
  with camelCase names (`record_toggled` to `recordToggled`); public methods
  keep their meaning (`set_status` to `setStatus`).
- Pure helper functions inside widget modules move to the matching Qt-free
  library (`uimath`, or the area's core) so they get golden vectors.
- Qt-free view models are allowed but not required; the rule is that anything
  with a decision table (enable rules, wording choices, layout maths) is a
  pure function with a golden vector, and the widget only applies it.

### 8.4 objectName and dynamic properties

- Object names are the Python string literals, verbatim (`footerBar`,
  `customTitleBar`). `tools/lint/check_qss_names.py` runs as a CTest test:
  every `#name` in `theme.qss` must appear in some C++ source, or be listed in
  `tools/lint/qss_unused.txt` with the reason.
- Dynamic properties go through `fv::ui::setStyleProperty(QWidget*, const char*
  name, QVariant)`, which sets, unpolishes and polishes; setting a property
  directly with `setProperty` is a lint error in `ui_*` dirs.
- Widget-local stylesheets go through `fv::ui::setLocalStyleSheet(QWidget*,
  QString tokenSource)`, which stores the `@token` source and re-resolves it on
  theme and scale changes. Colours never appear as hex in C++; sizes that
  scale use `fv::ui::scaled(px)`.

### 8.5 Wiring controllers and widgets

Widgets emit intent signals and expose setters, as in Python. Each feature's
`install()` connects its widgets to its controller and its controller to core
services through `ctx.service<T>()`. Widgets never hold core objects and
never call `SpeechService`. The a03 wiring tables become per-feature
`install()` code plus one test per feature, `"<feature> connects every signal"`,
which walks the widget's `metaObject()` and fails on an unconnected signal not
listed as intentionally unused. Screens registered in `ScreenRegistry` are
created lazily and receive queued `update_screen` calls on creation, as
`MainWindow.update_screen` does.

### 8.6 Strings

User-visible strings stay at their use site, verbatim from Python, as UTF-8
`QStringLiteral` (typographic quotes and dashes kept), with "OpenWhisper"
replaced by "OpenWhisper_CPP". Strings that core logic decides (statuses, refusals,
notices, error sentences) live in the core library as `constexpr
std::string_view` or builder functions, golden-tested with the replay
driver's rebrand map (`OpenWhisper` to `OpenWhisper_CPP`, `openwhisper` to
`openwhisper-cpp`). No translation layer now; strings are not wrapped in `tr()`.

---

## 9. Testing and parity gates

### 9.1 Test targets

One executable per library: `tests/<dir>/` builds `fv_<dir>_tests` (Catch2),
discovered with `catch_discover_tests` and labelled `<dir>` plus kind labels:
`unit`, `golden`, `widget`, `screenshot`, `decode` (needs
`OPENWHISPER_CPP_TEST_RUNTIME_DIR`, `_MODEL`, `_WAV`), `platform` (touches real
Win32 hooks or devices; skipped in CI). Widget tests link `fv_testkit`'s Qt
main, which creates a `QApplication` on the offscreen platform with the pinned
fonts, DPI and style; CTest sets the environment, as today.

Test names keep OpenWhisper's: `"[recorder] repeat stop keeps the first
deadline"` with `// from tests/test_recorder_postroll.py::test_...`. Each
package writes `tests/<dir>/PARITY.json` mapping every OpenWhisper test file
it covers to `ported`, `golden`, `dropped` or `pending`; `tools/parity/
parity_report.py` aggregates them (no shared ledger file).

### 9.2 Golden vectors

`tools/parity/golden_vectors.py` (curated cases) and `trace_plugin.py`
(inputs harvested from OpenWhisper's own tests) write
`tests/golden/<python.module>.json` with the OpenWhisper commit inside. The
package that ports a module generates and commits its vectors. `fv_golden`
gives `golden::for_each(file, "function", [](const Json& args, const Json&
expected){...})`, typed comparators (exact, rebranded string, float with
tolerance 1e-6 for float32 DSP) and `raises` checks. A changed vector is a
behaviour change to review.

### 9.3 Reference screenshots

`tools/parity/render_reference.py` builds OpenWhisper widgets with fixed fake
data under the a33 pins (offscreen, `QT_QPA_FONTDIR`, DPI 96, DPR, base
style set after `QApplication`, focus pinned, animations at their end
values, clock and seed pinned) and writes `tests/reference/<widget>/
<variant>.png` plus the tree dump. The C++ test builds the same widget with
the same fake data and calls `screenshot::expect_matches(widget, "<widget>",
"<variant>")`: first the tree (class, objectName, properties, geometry, text,
visibility) for readable failures, then pixels, writing a diff PNG into the
build tree. Screenshot tests run only on Windows with the pinned Qt kit
recorded in the sidecar; a different kit version skips them with a message.
Variants per widget: dark and light at 100 %, plus dark at 130 % for anything
with reflow.

### 9.4 Where the harness lives

`out/parity-scratch` moves to `tools/parity/` (scripts only; generated data
goes to `tests/golden` and `tests/reference` when committed, else to
`out/parity`). Environment: `OPENWHISPER_CPP_OW_ROOT` (OpenWhisper checkout) and
`OPENWHISPER_CPP_OW_PYTHON` (its venv). `tools/parity/README.md` gives the commands.

### 9.5 What a work package must pass before merge

1. Its own `fv_<dir>_tests` pass, and the full tree configures, builds and
   passes `ctest --preset windows-debug` from the merge result (the
   orchestrator runs this after rebasing onto the wave's integration branch).
2. `lint.layering`, `lint.ownership` (files changed are inside the package's
   owned directories), `lint.qss_names`, `clang-format --dry-run -Werror`.
3. Every pure function ported from Python has a golden vector or a ported test
   cited in `PARITY.json`; every new widget has screenshot variants.
4. Core libraries it touched build on the `linux` preset (the orchestrator
   runs this per wave in WSL).
5. No new third-party dependency, no edit to a frozen file (`CMakeLists.txt`,
   `cmake/`, `vcpkg.json`, `CMakePresets.json`, `appkit/order.h`,
   `settings/keys.h`, `ui_theme/resources/theme.qss`, `CLAUDE.md`).

---

## 10. Build

### 10.1 Module discovery

`cmake/OpenWhisper_CPPModules.cmake` (frozen after M0.1):

```cmake
# Each src/<dir>/CMakeLists.txt is one call:
fv_module(KIND core
          DEPS fv_base fv_pyport
          THIRD_PARTY PCRE2::8BIT
          WIN32_LIBS user32)
```

- `fv_module` derives the target name from the directory, globs `*.cpp *.h`,
  `detail/`, `testing/`, plus `win/` or `posix/` with `CONFIGURE_DEPENDS`, so
  adding a file never touches CMake. It applies warnings, `/utf-8`, the
  layer's PCH and the include root, records `DEPS` for the layering lint, and
  for `ui`/`feature` kinds runs AUTOMOC and `qt_add_resources` on
  `resources/`.
- The top-level file globs `src/*/CMakeLists.txt` and `tests/*/CMakeLists.txt`
  and adds them in sorted order. `target_link_libraries` resolves names at
  generate time, so order does not matter. A deferred check
  (`cmake_language(DEFER CALL ...)`) fails configure when a `core` module
  depends on a non-core one.
- `src/app` links every `feature` module through
  `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>` from a global property, so adding a
  feature adds no line anywhere else.
- `fv_test(DEPS fv_testkit fv_golden)` in `tests/<dir>/CMakeLists.txt` builds
  `fv_<dir>_tests`, labels it, and sets the offscreen environment for widget
  tests.

### 10.2 Build-time controls

- PCH: `fv_pch_core` (standard headers, `nlohmann/json.hpp`) and `fv_pch_qt`
  (QtCore, QtGui, QtWidgets headers); modules reuse them with `REUSE_FROM`.
  Beast, Asio, OpenSSL, curl, sqlite3, miniaudio and Windows headers never
  appear in public headers (pimpl), so they never enter another library's
  compile.
- Qt deployment runs once: one `fv_qt_runtime` custom target copies the Qt
  DLLs and plugins into `bin/` and every Qt executable depends on it, instead
  of `windeployqt` after each of ~25 executables.
- A worker's inner loop: `cmake --build --preset windows-debug --target
  fv_<dir>_tests` and `ctest --preset windows-debug -L <dir>`.
- vcpkg binary cache: M0.1 sets `VCPKG_DEFAULT_BINARY_CACHE` to a shared folder
  outside the worktrees and pre-builds all ports once, so a fresh worktree's
  vcpkg step restores archives instead of compiling Boost, OpenSSL and curl.

### 10.3 vcpkg additions (M0.1, then frozen)

`nlohmann-json`, `pcre2`, `sqlite3[fts5]`, `curl` (Schannel on Windows),
`boost-beast`, `boost-asio`, `openssl`, `miniaudio`, `kissfft`, `minizip-ng`
(zlib only), `pugixml`, `zlib`, `wil` (Windows only), `catch2`. Catch2 moves
from FetchContent to vcpkg; the `default` preset keeps FetchContent for
Catch2 and builds only `fv_engine`, `fv_cli` and their tests without vcpkg.

### 10.4 Presets

Keep `windows`/`linux`/`linux-asan` and the workflow presets. Add
`windows-ci` (Release, `OPENWHISPER_CPP_WARNINGS_AS_ERRORS=ON`, screenshot tests
on) and a `linux-core` configure preset with `OPENWHISPER_CPP_BUILD_APP=OFF` that
builds every `core` module and its tests, which is how "src/engine and the
cores build on Linux" stays true.

---

## 11. Milestones

Each milestone ends in a runnable app that can be demonstrated, with the
tree green.

| Milestone | Demo | Areas |
|---|---|---|
| **M0 Foundation** | Splash, then a themed frameless main window (title bar, menus, footer, four placeholder tabs), tray, close to tray, single instance, settings and logs in `%LOCALAPPDATA%\OpenWhisper_CPP`; `openwhisper-cpp-cli --version`, `--diagnostics`; title bar, footer and EmptyState pass screenshot parity. | a01 (core, shell skeleton), a02 (pipeline, leaf widgets), a10 (settings, credentials), a33 (harness), a34 (build) |
| **M1 Dictation** | Press `kp *`, speak, the Bars overlay reacts, press again: Parakeet text is pasted into Notepad, appears in Quick Record and the History sidebar; runtime and model found automatically from an OpenWhisper install or picked once. | a03 (core, controller), a04 (dictation, hotkey runtime), a05 (recorder, devices, WAV), a06 (Windows hooks, paste, clipboard, focus), a07 (overlay with Bars and Classic), a08 (Quick Record), a09 (DB, manager, sidebar), a14 (catalog, store, speech service) |
| **M2 Everyday** | Live preview, incremental long dictation, Upload File with batches, full History (entry dialog, playback, export, calendar, Stats), Settings window with General/Recording/Appearance/Hotkeys/API keys, Downloads and Voice model pages, cloud API speech, guided setup. | a05 rest, a08 rest, a09 rest, a11, a14 rest (downloads, consent, GPU), a15, a06 rest (hotkeys page, mouse buttons), a07 rest (all looks, moments) |
| **M3 Data safety and release** | Backup and restore with schedule; import from OpenWhisper; update check and setup-exe update; Inno installer and signed build. | a10 backup, a16, a34 |
| **M4 Personalize and AI** | AI cleanup with profiles and levels, dictionary, snippets, app styles, Command Mode, transforms, Scratchpad transform, learned rules. | a12, a13 |
| **M5 Automation and agents** | Actions with all kinds, Route by voice, hooks, live API with SSE, MCP server, History REST, Claude Code channel. | a19, a20, a21 |
| **M6 Walkthroughs** | Walkthrough tab, live column, Snap with annotations, Auto Mode, PDF/DOCX/Markdown documents, AI write-up, library. | a22, a23 |
| **M7 Meetings** | Meeting Mode with mic and loopback, live transcript, diarization, finalization card, recovery, past meetings, browser dashboard on LAN, then AI insights with the native agent loop and installed agents. | a24, a25, a28, a30, a31, then a26, a27, a29 |
| **M8 Remote engine** | Pair with an OpenWhisper or OpenWhisper_CPP computer, dictate through it, Host Mode dashboard, records on host. | a17, a18 |
| **M9 Parity sweep** | PARITY report at 100 % for kept features; native updater (phase 2) if chosen; Linux core CI green; macOS/Linux platform backends start. | all |

### 11.1 Work packages for M0

| WP | Name | Owned directories | Depends on | Est. lines |
|---|---|---|---|---|
| M0.1 | build-skeleton | `CMakeLists.txt`, `cmake/`, `CMakePresets.json`, `vcpkg.json`, `tools/lint/`, `tests/CMakeLists.txt`, `tests/engine/` (moved), `src/engine/CMakeLists.txt`, `src/cli/CMakeLists.txt`, `CLAUDE.md`, `BUILDING.md` | - (runs alone) | 900 CMake/Python |
| M0.2 | base | `src/base`, `tests/base` | M0.1 | 2,300 |
| M0.3 | pyport | `src/pyport`, `tests/pyport`, `tools/gen/` (Unicode table generator) | M0.1 | 2,700 |
| M0.4 | parity-harness | `tools/parity/`, `tests/support/`, `tests/golden/` (format_utils, fuzzy_match seeds) | M0.1 | 1,300 + scripts moved |
| M0.5 | ui-theme | `src/ui_theme`, `src/uimath`, `tests/ui_theme`, `tests/uimath` | M0.1 | 2,400 |
| M0.6 | log | `src/log`, `tests/log` | M0.2 | 1,500 |
| M0.7 | settings | `src/settings`, `tests/settings` | M0.2, M0.3 | 2,900 |
| M0.8 | credentials | `src/credentials`, `tests/credentials` | M0.2 | 1,000 |
| M0.9 | widgets-leaf | `src/ui_widgets`, `tests/ui_widgets`, `tests/reference/` (its widgets) | M0.4, M0.5 | 2,700 |
| M0.10 | appkit | `src/appkit` (incl. `order.h`), `tests/appkit` | M0.2 | 1,300 |
| M0.11 | shell | `src/ui_shell`, `src/app`, `tests/ui_shell` (retires `src/app` bench) | M0.6..M0.10 | 3,200 |
| M0.12 | cli | `src/cli`, `tests/cli` | M0.6, M0.7 | 500 |

Waves: {M0.1} -> {M0.2, M0.3, M0.4, M0.5} -> {M0.6, M0.7, M0.8, M0.10} ->
{M0.9 (needs only M0.4 and M0.5, so it may join wave 3)} -> {M0.11, M0.12}.

### 11.2 Work packages for M1

| WP | Name | Owned directories | Depends on | Est. lines |
|---|---|---|---|---|
| M1.1 | appcore | `src/appcore` | M0 | 1,400 |
| M1.2 | dsp | `src/dsp` | M0 | 1,000 |
| M1.3 | audio | `src/audio` | M0 | 2,300 |
| M1.4 | sql-db | `src/sql`, `src/db` | M0 | 2,300 |
| M1.5 | hotkeys | `src/hotkeys` | M0 | 2,900 |
| M1.6 | focus-paste | `src/focus`, `src/clipboard` | M0 | 2,500 |
| M1.7 | http-download-archive | `src/http`, `src/download`, `src/archive` | M0 | 2,000 |
| M1.8 | overlay | `src/ui_overlay` (Bars, Classic, state machine, placement, notices) | M0 | 3,000 |
| M1.9 | quick-record | `src/ui_record` (Quick Record tab, engine card, transcript pane, compact controller shell) | M0 | 2,300 |
| M1.10 | recorder | `src/recorder` | M1.2, M1.3 | 2,200 |
| M1.11 | models | `src/models` | M1.7 | 2,500 |
| M1.12 | speech | `src/speech` (Parakeet backend, service, windows, preview core) | M1.1, M1.2 | 2,000 |
| M1.13 | history-core | `src/history` | M1.4 | 1,700 |
| M1.14 | dictation | `src/dictation` (job slot, transcription and hotkey runtimes, pipeline without cleanup, delivery) | M1.1, M1.5, M1.6, M1.10, M1.12, M1.13 | 3,300 |
| M1.15 | history-sidebar | `src/ui_history` (sidebar, card, edge tab, entry dialog) | M1.13 | 2,600 |
| M1.16 | controller | `src/ui_controller` (UiController, OverlayRouter, ApplicationController, readiness, runtime/model setup, shutdown) | all of M1 | 2,800 |

Waves: {M1.1..M1.9} -> {M1.10, M1.11, M1.12, M1.13} -> {M1.14, M1.15} ->
{M1.16}. M1.16 ends with an end-to-end test: fake audio backend plays a WAV,
a scripted hotkey sequence starts and stops, a fake recognizer returns text,
and the test asserts paste, history row and overlay state trace.

---

## 12. Risks, and what the native app should do better

### 12.1 Risks

| Risk | Mitigation |
|---|---|
| Interfaces in M0 turn out wrong and many packages depend on them | Interfaces only carry what three maps already show; additions are allowed, changes go through a wave-boundary "interface fix" package run by the orchestrator. |
| Workers translate Python semantics literally (rounding, code points, casefold, `\w`, `int()`, `fromisoformat`) | `fv_pyport` lands first; golden vectors cover every pure function; a lint flags `std::round`, `std::lround`, `std::regex` and `.size()` on user text in reviews. |
| Device names under miniaudio do not match saved OpenWhisper keys | Matching ladder golden-tested against a dumped OpenWhisper device list; unmatched keys fall back to the default device and are kept, never deleted. |
| One runtime per process surprises users switching CPU/GPU | Clear restart prompt; the switch is a setting applied at start, never a hot swap. |
| In-process native crash kills dictation (no worker isolation) | Crash log and minidump, journal-based recording recovery on next start (a05), and a decode-level try/catch around every `EngineError`. |
| Feature registration hides wiring mistakes | Per-feature "connects every signal" tests and an app-level smoke test that opens every screen and Settings page offscreen. |
| Screenshot parity depends on Qt kit version and Segoe UI | References record the kit; screenshot tests are a Windows-only gate on the pinned kit; tree comparison still runs elsewhere. |
| Remote engine byte compatibility with live OpenWhisper peers | Wire constants in one header; transcripts recorded from a Python host/client pair replayed against C++ in tests. |
| MCP has no C++ SDK | Request/response transcripts recorded from the Python server for both protocol revisions, replayed through the C++ dispatcher. |
| Knowledge-folder PDF text extraction (a29) has no library in the allowed list | Ship DOCX/PPTX/XLSX/plain text extraction first; PDF text needs an owner decision (a runtime-loaded PDFium component is the likely answer). |
| Beast compile cost and complexity | Confined to `fv_net` behind pimpl; only two packages ever edit it. |
| Late areas (meetings, remote) find the speech queue too coarse | Lanes and coalescing are designed from a25's needs now (meeting chunk lane, preview coalescing); the queue is one class to adjust. |

### 12.2 What the native app should do better

- **Latency:** the final pass starts the instant post-roll ends, on an engine
  already loaded in-process: no IPC, temp files or GIL.
- **Capture:** a lock-free callback path should end the input overflows Python
  recorded under GIL contention.
- **Cancel:** window-bounded decodes make cancel work during long uploads and
  meeting re-decodes.
- **Memory and startup:** one shared Parakeet model for dictation and meetings;
  no import costs, and the model loads on the speech thread while the window is
  already usable.
- **Diagnosis:** minidumps with symbols instead of a faulthandler trace.
- **Hooks:** LL hook callbacks finish in microseconds, so Windows should stop
  dropping the hook; the watchdog stays as insurance.
- **Single instance on Windows too**, through `QLocalServer`.
- **Thread safety by construction:** posting signals and `qtConnect` remove the
  relay-with-token crash class instead of patching it per dialog.
- **Search:** a registered Unicode case-folding SQL function can extend
  case-insensitive history search beyond ASCII, behind a test that ASCII
  behaviour is unchanged.
