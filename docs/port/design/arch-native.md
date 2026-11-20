# OpenWhisper_CPP full-parity architecture: the native-first proposal

One of three competing proposals for porting OpenWhisper 2.8.92 (Python 3.12, PyQt6,
`D:\coding\whisper_local`) to OpenWhisper_CPP (C++20, Qt 6 Widgets). Angle: the app a strong
C++/Qt engineer would build knowing every feature in the 34 area maps. Every user-visible
behaviour stays identical; every internal shape that exists because of the GIL, worker
processes, `asyncio`, import cost or lazy-loading tricks is replaced, and each replacement
says what it buys.

Inputs: `docs/port/map-summaries.json` (34 areas, about 205k estimated C++ lines),
the maps under `docs/port/map/`, `areas.json`, the current tree (about 2k lines: `fv_engine`,
`openwhisper-cpp-cli`, a bench `fv_app`), and the owner's decisions and working assumptions in the
task brief. Where a working assumption decides the design, the text says so and section 12
lists the alternatives.

---

## 1. Principles

1. **Copy behaviour, not shape.** Strings, timings, file formats, wire bytes and widget looks
   match OpenWhisper; module boundaries, threads and processes are redesigned. *Why:* parity is
   judged by users, tests and screenshots, not by how Python happened to be organised.
2. **Qt stops at the widget and controller layer.** All domain logic lives in Qt-free C++20
   libraries under `src/core`; only `src/app` links Qt. *Why:* the cores build and test with a
   compiler alone (also on Linux), and the headless modes (`--api`, `--channel`) start without
   loading a single Qt DLL.
3. **Python semantics live in exactly one library.** Half-to-even rounding, code-point lengths,
   full case folding, Unicode `\w`, `json.dumps` spacing and escaping, `repr(float)` and
   `difflib` are implemented once in `fv_pytext`. *Why:* these are the commonest parity bugs (the
   a33 replay already caught `round(62.5)`), and 40 workers must not reinvent them 40 times.
4. **One owner thread per scarce resource, reached by message.** The speech runtime, the SQLite
   writer, each audio device, the hook thread and the GUI each have one owner; everyone else
   posts work. *Why:* it removes the lock webs Python needed (`_engine_lock`, `_capture_lock`,
   `portaudio_lock`) and makes ordering explicit and testable.
5. **Audio is written once and referenced everywhere.** Capture appends to a `PcmTape`; the
   journal, level meter, live preview, incremental decoding, final pass, meeting spool and remote
   upload read spans of it. *Why:* no copies between stages, and windows are bit-identical between
   the live and file paths by construction instead of by CRC luck.
6. **Contracts first, frozen per milestone.** Interfaces, all 186 settings keys, settings
   destinations, tab ids, style-property names and service slots are declared before workers start;
   a work package adds directories and never edits a shared file. *Why:* workers cannot ask
   questions and every branch must merge without conflicts.
7. **Parity is proven mechanically.** Every pure function gets golden vectors, every widget a tree
   dump and an offscreen pixel diff, every ported test keeps its Python name. *Why:* "looks the
   same" and "behaves the same" are otherwise opinions.
8. **Heavy native code is loaded by path, never unloaded, one per process.** NeMo-Speech.cpp,
   onnxruntime, PDFium and NVML are bound at run time like `NemoApi::get`. *Why:* `FreeLibrary`
   crashes NeMo 0.1.0, ggml DLL names collide between CPU and GPU builds, and none of them should
   be compiled through vcpkg or grow the installer.
9. **The GUI thread never waits.** Window first; model load, database open, device listing,
   credential reads and scans run elsewhere and report back. *Why:* target under 400 ms to a
   painted main window, against Python's import-bound startup and its `run_with_ui_pulse` crutch.
10. **OpenWhisper_CPP names, OpenWhisper formats.** All names come from one `Identity` value; settings
    keys, database schema, backup layout and the remote-engine wire stay OpenWhisper's. *Why:* a
    one-time import and peer interoperability without ever writing into OpenWhisper's folder.

---

## 2. Libraries and directory layout

```
CMakeLists.txt  CMakePresets.json  vcpkg.json        (lead-owned; packages never edit them)
cmake/          OpenWhisper_CPPModules.cmake (fv_add_library, fv_module, fv_feature, fv_embed),
                OpenWhisper_CPPLayers.cmake (allowed dependency edges), OpenWhisper_CPPQt.cmake, pch/
src/engine/     fv_engine            existing Qt-free Parakeet wrapper (stays as CLAUDE.md says)
src/core/<lib>/ Qt-free libraries, one sub-directory per module, each with its own CMakeLists.txt
src/app/<lib>/  Qt libraries; src/app/features/<feature>/ for feature UIs
src/gui/        openwhisper-cpp            (WIN32 GUI executable, main.cpp only)
src/cli/        openwhisper-cpp-cli        (console executable)
src/updater/    openwhisper-cpp-updater    (Qt-free, static CRT; milestone M9)
resources/      theme/ (theme.qss verbatim, palette.json, tabler/*.svg, icons), data/ (catalogs,
                prompts, keyboard names), webui/ (React dashboard source + committed dist)
third_party/    nemo_speech/ (existing), onnxruntime/ and pdfium/ (C API headers only)
tests/          core/<lib>/, app/<feature>/, golden/<area>/*.json, parity/, fixtures/
tools/          parity/ (Python harness), gen/ (generators from the Python source), lint/
docs/port/      maps, designs, parity-status.json (per OpenWhisper test file)
```

Include paths are rooted at `src` (`"engine/recognizer.h"`, `"core/audio/pcm_tape.h"`,
`"app/uikit/button.h"`). Namespaces: `fv` everywhere, one nested namespace per core library
(`fv::audio`, `fv::meeting`), Qt classes directly in `fv`. Core code uses the engine's
snake_case; Qt code uses camelCase.

### Targets

Layers are enforced by `OpenWhisper_CPPLayers.cmake`: a target may link only targets in a lower layer
or listed peers, and a core target that links any `Qt6::` target fails configuration.

| Target | Dir | Layer | Qt modules | Third-party | Links (ours) |
|---|---|---|---|---|---|
| `fv_base` | src/core/base | C0 | none | nlohmann-json | none (OS: bcrypt, dbghelp, advapi32) |
| `fv_engine` | src/engine | C0 | none | vendored `asr.h` | none (kept standalone) |
| `fv_pytext` | src/core/pytext | C1 | none | pcre2, utf8proc | fv_base |
| `fv_uicore` | src/core/uicore | C2 | none | — | fv_pytext |
| `fv_settings` | src/core/settings | C2 | none | — | fv_pytext |
| `fv_net` | src/core/net | C2 | none | boost-beast/asio, openssl | fv_base |
| `fv_data` | src/core/data | C3 | none | sqlite3[fts5], miniz | fv_settings |
| `fv_audio` | src/core/audio | C3 | none | miniaudio, ffmpeg, pocketfft | fv_settings |
| `fv_input` | src/core/input | C3 | none | wil | fv_settings |
| `fv_speech` | src/core/speech | C4 | none | — | fv_engine, fv_audio |
| `fv_text` | src/core/text | C4 | none | — | fv_data |
| `fv_update` | src/core/update | C4 | none | miniz | fv_data, fv_engine |
| `fv_appcore` | src/core/appcore | C5 | none | — | fv_speech, fv_text, fv_input |
| `fv_walkthrough` | src/core/walkthrough | C5 | none | — | fv_speech, fv_text |
| `fv_automation` | src/core/automation | C5 | none | — | fv_text, fv_net, fv_speech |
| `fv_remote` | src/core/remote | C5 | none | openssl | fv_speech, fv_data, fv_net |
| `fv_meeting` | src/core/meeting | C6 | none | pugixml (+ ORT, PDFium by path) | fv_automation, fv_audio |
| `fv_qtbridge` | src/app/bridge | Q0 | Core, Network | — | fv_settings |
| `fv_docs` | src/app/docs | Q1 | Core, Gui | miniz | fv_walkthrough, fv_qtbridge |
| `fv_uikit` | src/app/uikit | Q1 | Widgets, Svg, SvgWidgets | — | fv_uicore, fv_qtbridge |
| `fv_overlay` | src/app/overlay | Q2 | Widgets | — | fv_uikit, fv_appcore |
| `fv_runtime` | src/app/runtime | Q2 | Core, Gui | — | every core lib, fv_qtbridge, fv_docs |
| `fv_shell` | src/app/shell | Q3 | Widgets, Network | — | fv_runtime, fv_uikit, fv_overlay |
| `fv_ui_record` | src/app/features/record | Q4 | Widgets | — | fv_shell |
| `fv_ui_history` | src/app/features/history | Q4 | Widgets | — | fv_shell |
| `fv_ui_settings` | src/app/features/settings | Q4 | Widgets | — | fv_shell |
| `fv_ui_personalize` | src/app/features/personalize | Q5 | Widgets | — | fv_ui_settings |
| `fv_ui_downloads` | src/app/features/downloads | Q5 | Widgets | — | fv_ui_settings |
| `fv_ui_update` | src/app/features/update | Q5 | Widgets | — | fv_ui_settings |
| `fv_ui_automation` | src/app/features/automation | Q5 | Widgets | — | fv_ui_settings |
| `fv_ui_remote` | src/app/features/remote | Q5 | Widgets | — | fv_ui_settings |
| `fv_ui_walkthrough` | src/app/features/walkthrough | Q5 | Widgets | — | fv_ui_history, fv_ui_settings |
| `fv_ui_meeting` | src/app/features/meeting | Q5 | Widgets | — | fv_ui_history, fv_ui_settings |
| `openwhisper-cpp` | src/gui | exe | all of the above | — | fv_shell + feature libs via the generated registry |
| `openwhisper-cpp-cli` | src/cli | exe | none | — | fv_automation, fv_data, fv_update, fv_speech |
| `openwhisper-cpp-updater` | src/updater | exe | none | miniz | fv_update (native updater part only) |
| `fv_<lib>_tests` | tests/core/<lib> | test | none | catch2 | one per core library |
| `fv_app_tests` | tests/app | test | Test + above | — | every Qt library |
| `fv_parity_tests` | tests/parity | test | Test + above | — | Windows only, label `parity` |

Why these cuts: a library per bounded context keeps link edges honest and incremental builds
small; a separate `fv_appcore` holds the Qt-free half of the controllers (job slot, hold state
machine, reload state machine, gates, string builders, dictation pipeline) so the most test-pinned
logic in a03/a04 replays golden traces without Qt. Rejected: one big `fv_app` (today's shape),
because 120k lines of UI in one library means every header change rebuilds everything and nothing
stops a widget from calling SQLite.

### Where each code area lands

| Area | Qt-free | Qt |
|---|---|---|
| a01 app shell | fv_base (paths, identity, log, crash, diagnostics, process env, profiler) | fv_shell (Application, Bootstrap, LoadingScreen, MainWindow, TitleBar, SystemTray, SingleInstance); openwhisper-cpp, openwhisper-cpp-cli |
| a02 UI foundation | fv_uicore (palette tokens, QSS text pipeline, font scale, fuzzy match, overlay geometry, loudness, release notes HTML, list reconcile) | fv_uikit (ThemeEngine, ~40 shared widgets, icons, tooltip style) |
| a03 UI controller | fv_appcore (EngineReloadMachine, readiness and refusal gates, overlay routing table, PreviewThrottle, OverlayState) | fv_runtime (ApplicationController), fv_shell (UiController and its presenters) |
| a04 runtime services | fv_appcore (JobSlot, HoldStateMachine, status builders, URL helpers) | fv_runtime (Transcription, Command, Hotkey, Streaming, Model, Component, Updater, Walkthrough, Meeting, Remote runtimes) |
| a05 recording/audio | fv_audio (devices, capture, PcmTape, recorder, journal, WAV, player, file decode, split, DSP), fv_speech (preview, incremental, dictation language), fv_appcore (dictation pipeline), fv_data (dictation stats) | fv_ui_settings (MicrophoneCheck), fv_ui_walkthrough (CaptureDelay) |
| a06 hotkeys/paste/focus | fv_input (grammar, matcher, LL hooks, synthetic keys, foreground identity, UIA reader, catalog, join) | fv_runtime (TemporaryClipboard), fv_ui_settings (capture fields, Hotkeys rows) |
| a07 overlays | fv_uicore (VoiceRibbon, easing, moments, particles, placement, tidy lines) | fv_overlay (WaveformOverlay, 11 looks, preview widget, language menu) |
| a08 record/upload tabs | fv_appcore (batch upload types, scratchpad text, progress model) | fv_ui_record |
| a09 history/stats | fv_data (db, schema v18, history manager, export, calendar index, titles, format utils, stats) | fv_ui_history |
| a10 settings services | fv_settings (store, keys, kinds, resolvers, credentials), fv_data (backup, restore, data lease, OpenWhisper import) | fv_runtime (BackupCoordinator) |
| a11 settings dialog | fv_appcore (search ranking, mic priority rules, key status copy, retention text, backup dates) | fv_ui_settings |
| a12 text AI services | fv_text | — |
| a13 personalize UI | fv_text (copy builders) | fv_ui_personalize |
| a14 models/components | fv_update (catalogs, verified download, components, model cache, GPU probe, runtime resolver, consent policy), fv_speech (local and cloud backends, windows) | fv_runtime (ModelRuntime, ComponentRuntime) |
| a15 downloads UI | fv_update (queue state machine, consent text) | fv_ui_downloads |
| a16 updater/setup | fv_update (feed, schedule, trust rules, native transaction), openwhisper-cpp-updater | fv_ui_update (update dialog, notice, guided setup) |
| a17 remote services | fv_remote (+ fv_net transport) | — |
| a18 remote UI | fv_remote (view models) | fv_ui_remote |
| a19 actions/automation | fv_automation (actions, routing, event bus, webhooks, programs, filters, launch, channel bridge) | fv_runtime (ActionRuntime) |
| a20 agent API/MCP | fv_automation (History API, MCP server, events, controls, installed agents), fv_base (process and job objects) | fv_runtime (McpRuntime) |
| a21 actions UI | fv_automation (action copy, setup text) | fv_ui_automation |
| a22 walkthrough services/snap | fv_walkthrough (session, sections, manifest, library, write-up text, styles) | fv_docs (PDF, DOCX, Markdown), fv_ui_walkthrough (Snap overlay, toolbar, live annotations) |
| a23 walkthrough UI | fv_walkthrough (column model, layout math, formatters) | fv_ui_walkthrough |
| a24 meeting engine | fv_meeting (engine, EndRun, refinalize, recovery, clock) | fv_runtime (MeetingRuntime) |
| a25 capture/ASR/diarize | fv_meeting (capture session, spool, ASR worker, diarizer, playback), fv_audio (sources, fft_resample) | — |
| a26 meeting agent | fv_meeting/agent (prompts, scheduler, policy, evidence, CLI drivers, loopback MCP) | — |
| a27 sidecars | fv_text/agent (native loop over four streaming protocols) | — |
| a28 state/persist/export | fv_meeting (state, ops, store, repository, corrections, exports) | — |
| a29 intelligence | fv_meeting/intel | — |
| a30 web server | fv_meeting/web (DashboardApi, WsHub) on fv_net; resources/webui | — |
| a31 meeting Qt UI | fv_meeting (finalization view model, formatters, setup checks) | fv_ui_meeting |

---

## 3. Cross-cutting services and their interfaces

The sketches below are the M0 contracts: package M0.0 writes these headers, the owning package
implements them, everyone else codes against them from day one.

### Identity and paths (`core/base/identity.h`, `paths.h`)

```cpp
namespace fv {
// Every product-specific name. OpenWhisper_CPP's own values, plus OpenWhisper's for the importer
// and the backup reader. Nothing else in the tree spells either product name.
struct Identity {
    std::string_view product;            // "OpenWhisper_CPP"
    std::string_view data_dir_name;      // "OpenWhisper_CPP"  (%LOCALAPPDATA%\OpenWhisper_CPP)
    std::string_view env_prefix;         // "OPENWHISPER_CPP_"  (OPENWHISPER_CPP_DATA_DIR, ..._LOG_LEVEL)
    std::string_view settings_file;      // "openwhisper_cpp_settings.json"
    std::string_view database_file;      // "openwhisper-cpp.db"
    std::string_view log_file;           // "openwhisper-cpp.log"
    std::string_view lease_file;         // ".openwhisper-cpp-data.lock"
    std::string_view restore_dir;        // ".openwhisper-cpp-restore"
    std::string_view credential_service; // "OpenWhisper_CPP"
    std::string_view backup_format;      // "openwhisper-cpp-backup" (reader also accepts openwhisper-backup)
    std::string_view backup_extension;   // ".fvbackup"
    static const Identity& openwhisper-cpp();
    static const Identity& openwhisper();
};

class Paths {
public:
    // OPENWHISPER_CPP_DATA_DIR wins, else the per-user dir; a source checkout never becomes the data
    // root (a deliberate difference: OpenWhisper's checkout rule exists for `python main.py`).
    static Paths resolve(const Identity&, const Environment&);
    const std::filesystem::path& data_root() const;
    std::filesystem::path user_file(std::string_view name) const;   // data_root()/name
    std::filesystem::path components_root() const;                  // <local app dir>/components
    std::filesystem::path speech_models_root() const;
    std::vector<ExternalSource> openwhisper_sources() const;        // read-only reuse, section 7
};
}
```

### Settings store (`core/settings/store.h`, `setting.h`, `keys.h`)

```cpp
namespace fv::settings {
using Json = nlohmann::ordered_json;           // keeps key order and int/float/bool distinction
using Snapshot = std::shared_ptr<const Json>;  // copy-on-write; readers never copy

template <class T> class Setting {             // one per key, generated from Python (186 keys)
public:
    constexpr std::string_view key() const;
    T read(const Json& s) const;               // exact Python fallback rules (int() semantics...)
    std::optional<T> stored(const Json& s) const;
    T read(const Store&) const;
};
namespace keys { extern const Setting<bool> auto_paste; /* ... generated ... */ }

class Store {
public:
    explicit Store(std::filesystem::path file, Strictness);
    Snapshot snapshot() const;                                      // lock-free after first load
    Snapshot update(const Json& changes, std::span<const std::string> remove = {});
    template <class Fn> auto mutate(Fn&& fn);                       // atomic read-modify-write
    Subscription watch(std::vector<std::string> keys,
                       std::function<void(const Snapshot&, const std::vector<std::string>&)>);
    bool is_new_install() const;
};
}
```

Watchers run on the writing thread and must only enqueue; `fv_qtbridge::SettingsRelay` turns
them into a Qt signal on the GUI thread. Native gain: OpenWhisper pickles 45 KB per read; a
snapshot here is one atomic `shared_ptr` load.

### Logging and diagnostics (`core/base/log.h`, `diagnostics.h`, `crash.h`)

```cpp
namespace fv::log {
enum class Level { debug, info, warning, error, critical };
class Logger {                                 // name = the Python logger name ("services.recorder")
public:
    explicit constexpr Logger(std::string_view name);
    template <class... A> void info(std::format_string<A...>, A&&...) const;   // and the others
    void error_with(const std::exception&, std::string_view message) const;    // category from type
};
void install(const Paths&, Level, bool console);   // file "%(asctime)s - LEVEL - name - message"
}
```

One writer thread owns the rotating file (5 MiB x 3), the failure journal (`{utc, component,
category, line}`, never message text, max 50) and the metrics file (12 allowlisted names). Callers
push into a bounded lock-free queue and never block on stalled storage (a01 test pins it). Qt
messages arrive through `qInstallMessageHandler` as logger `qt`. Crash handling: section 6.

### Clock, cancellation and executors (`core/base/clock.h`, `cancel.h`, `exec.h`)

```cpp
namespace fv {
class Clock {                                   // injected everywhere; tests use FakeClock
public:
    virtual std::chrono::steady_clock::time_point steady() const = 0;
    virtual std::chrono::system_clock::time_point utc() const = 0;
    virtual const std::chrono::time_zone& zone() const = 0;
};
class CancelToken {                             // cheap copy; from a CancelSource
public:
    bool canceled() const;
    Registration on_cancel(std::function<void()>) const;  // aborts replies, wakes waits
    bool wait_for(std::chrono::milliseconds) const;       // true when canceled
};
class Executor { public: virtual void post(std::function<void()>) = 0; };
class ThreadPool final : public Executor { /* fixed size, named threads */ };
class Strand final : public Executor {          // serial queue on a pool: no thread of its own
public:
    enum class OnShutdown { cancel_queued, drain };
    Strand(ThreadPool&, std::string name, OnShutdown);
    void shutdown(std::chrono::milliseconds budget);
};
void assert_not_gui_thread();                   // every blocking API calls this in debug builds
}
```

Python's five `ThreadPoolExecutor`s become strands (`jobs`, `persistence` drain, `components`,
`walkthrough`, `backup`, `meeting`, `agent`, `db-write`) on one pool: fewer threads, and the
"cancel queued versus drain without cancel" shutdown that QThreadPool cannot express is built in.

### Credentials (`core/settings/credentials.h`)

```cpp
namespace fv {
class CredentialBackend {                       // WinCred now; Keychain and libsecret later
public:
    virtual std::optional<std::string> get(std::string_view name) = 0;
    virtual void set(std::string_view name, std::string_view secret) = 0;
    virtual bool remove(std::string_view name) = 0;
    virtual CredentialStoreStatus status() = 0;
};
class CredentialStore {                          // cache, timeouts, never logs values
public:
    CredentialStore(std::unique_ptr<CredentialBackend>, const Identity&);
    std::optional<std::string> resolve(std::string_view name) const;  // store, env, then .env
    CredentialSource source(std::string_view name) const;
};
}
```

The Windows backend calls `CredReadW`/`CredWriteW` with keyring's WinVault target convention
(`<name>@<service>`, legacy fallback), so the importer can read OpenWhisper's entries with the
same code by passing `Identity::openwhisper()`.

### HTTP client (`core/base/http.h`)

```cpp
namespace fv::http {
struct Request {
    std::string method = "GET", url;
    Headers headers;
    Body body;                                  // bytes, multipart (with file parts), or file
    std::chrono::milliseconds connect_timeout{10'000}, idle_timeout{120'000};
    std::optional<std::chrono::steady_clock::time_point> deadline;   // wall-clock cap
    Redirects redirects = Redirects::none;      // manual: caller re-validates each hop
    std::optional<ByteRange> range;             // resumable downloads
};
struct Response { int status = 0; Headers headers; std::string body; NetError error; };
class StreamSink {                              // SSE, NDJSON and downloads
public:
    virtual void on_headers(const Response&) = 0;
    virtual bool on_data(std::string_view chunk) = 0;   // false aborts
};
class Client {
public:
    virtual Response send(const Request&, const CancelToken&) = 0;           // blocking
    virtual Response stream(const Request&, StreamSink&, const CancelToken&) = 0;
};
}
```

Two implementations. `fv_qtbridge::QtHttpClient` runs one `QNetworkAccessManager` on its own
thread ("fv-qtnet"); callers block on a promise, never on the GUI thread; it brings the system
proxy, the OS trust store, HTTP/2 and keep-alive pools for free. `fv_net::PinnedHttpsClient`
(Beast) connects to an address that was resolved and checked once, while still sending SNI and
verifying the certificate for the host name: the SSRF guard webhooks and MCP Events need, which
QNAM cannot express. Rejected: libcurl, a third client stack for one feature.

### HTTP and WebSocket server (`core/net/server.h`)

```cpp
namespace fv::net {
class IoContext;                                 // 1-2 threads ("fv-net-N"), shared by all servers
struct HttpRequest { std::string method, path; Query query; Headers headers;
                     std::string body; Peer peer; bool tls; };
class Responder {                                // movable; may complete on any thread
public:
    void send(int status, Headers, std::string body);
    void send_file(const std::filesystem::path&, std::optional<ByteRange>);   // 206/416 handled
    SseStream start_sse(Headers);
    void upgrade_websocket(std::function<void(std::shared_ptr<WsSession>)>);
};
class Router { public: void add(std::string_view method, std::string_view pattern, Handler); };
class HttpServer {
public:
    HttpServer(IoContext&, Router);
    Endpoint listen(std::string_view address, uint16_t port, const TlsIdentity* = nullptr);
    void stop(std::chrono::milliseconds grace);   // aborts streaming responses after grace
};
class WsSession {
public:
    void send_text(std::shared_ptr<const std::string>);   // one buffer fanned out to N sockets
    void close(uint16_t code, std::string_view reason);
};
class WsClient;                                    // remote-engine client: TLS with SHA-256 pinning
}
```

Handlers in the cores are pure functions `(HttpRequest) -> response` (DashboardApi, HistoryApi,
McpServer, LiveApi) so tests call them without sockets; only `fv_net` includes Boost.

### Database (`core/data/db.h`)

```cpp
namespace fv::db {
class Statement;                                  // RAII prepared statement, typed bind/get
class Connection {                                // one thread at a time
public:
    static Connection open(const std::filesystem::path&, Mode);   // rw, ro (URI), memory
    Statement prepare(std::string_view sql);
    void exec(std::string_view sql);
    Transaction begin_immediate();
    sqlite3* raw();                               // backup API, FTS5 helpers
};
class Database {                                   // WAL; one writer strand, per-thread readers
public:
    template <class Fn> auto write(Fn&& fn) -> Future<std::invoke_result_t<Fn, Connection&>>;
    template <class Fn> auto read(Fn&& fn) -> std::invoke_result_t<Fn, Connection&>;
    int schema_version() const;                    // 18 at import time
};
}
```

### Process launcher (`core/base/process.h`)

```cpp
namespace fv {
struct ProcessSpec {
    std::filesystem::path exe; std::vector<std::string> args;
    std::filesystem::path cwd; EnvMap env;          // from ProcessEnv::external_env()
    bool own_job = true;                            // suspended start, assign job, resume
    bool hidden = true;                             // CREATE_NO_WINDOW
    Stdio stdio = Stdio::pipes;
};
class Process {
public:
    static Process start(const ProcessSpec&, net::IoContext&);  // pipes are overlapped I/O
    void write_stdin(std::string);
    void on_line(std::function<void(std::string_view, Stream)>);
    std::optional<int> wait_for(std::chrono::milliseconds);
    void kill_tree();                               // TerminateJobObject
    void disown();                                  // clean exit leaves children running
};
}
```

Pipes ride the asio loop, so ten agents and hook programs need no reader threads. Rejected:
`QProcess`, which has no job-tree kill or disown (a19, a20 pin both).

### Audio input and output (`core/audio/devices.h`, `capture.h`, `pcm_tape.h`)

```cpp
namespace fv::audio {
struct DeviceKey { std::string name, hostapi; std::string id; };   // {name, hostapi} = saved format
struct InputDevice { DeviceKey key; std::string label; Role role; int channels, rate; };
class DeviceCatalog {
public:
    virtual std::vector<InputDevice> inputs() = 0;          // never called on the GUI thread
    virtual std::vector<InputDevice> loopbacks() = 0;       // render endpoints, for meetings
    virtual std::optional<InputDevice> match(const DeviceKey&) = 0;   // the a05 ladder
};
class PcmTape {                                   // append-only int16 store in 1 s chunks
public:
    void append(std::span<const int16_t>);        // single writer: the audio-io thread
    TapeView view(int64_t first, int64_t last) const;  // zero-copy, shares chunk ownership
    int64_t frames() const;                       // atomic
    int rate() const;
};
class CaptureSource {                             // mic, loopback, test fake
public:
    virtual void start(const InputDevice&, Format, Sink&) = 0;    // Sink fed on audio-io thread
    virtual void stop() = 0;
    virtual Health health() const = 0;            // first block, stall, overflow counters
};
class Player { public: bool play(const std::filesystem::path&, std::function<void()> done); };
}
```

The device callback does three things only: copy into a preallocated SPSC ring, update an atomic
peak and RMS, return. The "fv-audio-io" thread drains every ring into tapes, journals and spools.

### Events between layers (`core/base/signal.h`, `app/bridge/mailbox.h`, `core/automation/event_bus.h`)

```cpp
namespace fv {
template <class... Args> class Signal {           // Qt-free observer; emits on the caller's thread
public:
    Connection connect(std::function<void(Args...)>);
    void emit(const Args&...) const;
};
class Mailbox : public QObject {                  // the only way a worker reaches a widget
public:
    template <class Fn> void post(QPointer<QObject> receiver, Token token, Fn&& fn);
    // runs fn on the GUI thread only if receiver is alive and token is still current
};
class EventSink {                                 // automation bus seen from lower layers
public:
    virtual void publish(std::string_view type, nlohmann::ordered_json payload) = 0;
};
}
```

Three channels, each with one job: `Signal` for core-to-runtime notifications, `Mailbox` plus
queued Qt signals for anything that lands on a widget (one implementation replacing OpenWhisper's
seven relay classes), and the automation `EventBus` (sequence numbers, replay ring, gap
reporting) for events that leave the process. Lower layers depend on `EventSink`, not on
`fv_automation`.

### Composition: AppContext and the feature registry (`app/runtime/app_context.h`, `registry.h`)

```cpp
namespace fv {
struct AppContext {                               // built once in Bootstrap, outlives every window
    const Identity& identity; const Paths& paths; Clock& clock;
    settings::Store& settings; CredentialStore& credentials; db::Database& db;
    ThreadPool& pool; net::IoContext& net; http::Client& http;
    speech::SpeechScheduler& speech; audio::DeviceCatalog& devices;
    EventSink& events; Mailbox& mailbox; /* runtimes, filled by ApplicationController */
};
class FeatureRegistry {                           // filled by generated code, read by the shell
public:
    void add_tab(TabId, std::function<QWidget*(AppContext&, QWidget*)>);
    void add_settings_page(DestinationId, PageFactory);
    void add_startup_step(int order, std::function<void(AppContext&)>);
    void add_shutdown_step(int order, std::function<void(AppContext&)>);
    void add_cli_mode(std::string flag, CliHandler);   // also used by openwhisper-cpp-cli
    void add_http_routes(ServerId, std::function<void(net::Router&, AppContext&)>);
    void add_mcp_tool(McpToolSpec);
};
}
```

Every feature library defines `void fv_register_<feature>(FeatureRegistry&)` and declares it in
its own CMakeLists with `fv_feature(...)`; CMake writes `generated/registry.cpp` calling them in
a stable order. Tab ids, settings destinations, chapters and their order are declared up front in
`app/runtime/destinations.h`, so a feature only supplies factories.

---

## 4. Threading model

| Thread | Count | What runs there |
|---|---|---|
| `main` (GUI) | 1 | Widgets, controllers, QTimers, Mailbox deliveries. Never blocks. |
| `fv-speech` | 1 | `SpeechScheduler`: owns every `Recognizer`; loads, warms, decodes. The only caller of the NeMo C ABI. |
| device callbacks | 1 per open device | miniaudio's WASAPI threads (mic, loopback, playback). Ring write and atomics only. |
| `fv-audio-io` | 1 | Drains rings into PcmTapes; journal and spool writes with 1 s checkpoints; post-roll gate; device health watchdog (100 ms); meeting chunk cutting. |
| `fv-input-hook` | 1 | `WH_KEYBOARD_LL` and `WH_MOUSE_LL` message loop; the matcher decides suppression inside the callback in microseconds; events go to the dispatcher queue. |
| `fv-hotkey-dispatch` | 1 | Ordered press/release handling with deadlines (the `OrderedDispatcher`), push-and-hold state machine. |
| `fv-focus` | 1 (replaceable) | Foreground identity and UIA caret/selection reads under a deadline; a hung UIA call is abandoned and a fresh thread replaces it (bounded to 2). |
| `fv-net-N` | 1-2 | asio `io_context`: dashboard, MCP, History API, live API/SSE, remote host and client, pinned webhooks, child-process pipes. |
| `fv-qtnet` | 1 | `QNetworkAccessManager` for outbound HTTPS (LLM, downloads, updates, GitHub). |
| `fv-pool-N` | 2-6 | Strands: `jobs` (cleanup, filters, delivery), `persistence` (history writes; drains at quit), `db-write`, `components`, `walkthrough`, `backup`, `meeting`, `agent`, `diarize`, scans and previews. |
| `fv-log` | 1 | Log file, failure journal, metrics. |
| watchdog | 0-1 | Only during an update hand-off: the 10 s hard-exit timer. |

At idle that is about ten threads, against OpenWhisper's interpreter threads, executors and a
worker process per engine.

Rules for crossing threads:

1. **GUI posts, never waits.** Blocking APIs (`http::Client::send`, `Database::read`,
   `future.get`) assert they are not on the GUI thread.
2. **Results reach widgets only through the Mailbox or queued signals of GUI-thread QObjects,**
   always with a token; a worker never holds a widget pointer. This is the C++ form of the relay
   crash fixes the Python suite pins.
3. **Audio callbacks never lock, allocate, log or make a syscall.** Overflow of the SPSC ring maps
   to OpenWhisper's 512-block queue semantics ("Recording storage cannot keep up").
4. **Only `fv-speech` touches a Recognizer,** and reads `last_error` inside the same job.
5. **Writes to SQLite go through the `db-write` strand;** reads use per-thread read connections
   (WAL). `BEGIN IMMEDIATE` exactly where Python uses it.
6. **The hook callback consults only state owned by the hook thread** (matcher, held keys,
   suppression set). Configuration arrives as an immutable snapshot swapped atomically.
7. **Prefer strands to locks.** Where locks remain (meeting store, credential cache, job slot) the
   global order is: job slot, capture, meeting runtime, meeting store, db. `std::scoped_lock` for
   multi-lock acquisitions such as the backup gate.
8. **Subscribers run on the publishing thread and only enqueue** (settings watchers, meeting store
   fan-out, event bus).
9. **Every wait takes a CancelToken;** shutdown cancels the root source first.
10. **Exceptions never cross threads raw:** they travel in futures and results; Qt slots, event
    filters and callbacks from C code catch everything and log the type.

Shutdown keeps OpenWhisper's order (a03): stop accepting work, cancel the batch download and
Auto Mode, stop hooks and capture, stop servers (1 s grace for streaming responses), cancel queued
speech jobs and let the current window finish, drain `persistence`, close the database, flush the
log, return from `main`. Recognizers are destroyed; runtime libraries are deliberately leaked.

---

## 5. Speech backends

### One interface, three implementations

```cpp
namespace fv::speech {
struct AudioRef {                                // zero-copy: a tape view, or owned float samples
    std::variant<audio::TapeView, std::shared_ptr<const std::vector<float>>> samples;
    int sample_rate;
};
struct Capabilities { bool offline_only; bool word_times; std::vector<std::string> languages; };
class Backend {
public:
    virtual std::string identity() const = 0;    // "parakeet:parakeet-v3:cuda0", "api:openai:...", "remote:<fp>"
    virtual Capabilities capabilities() const = 0;
    virtual Lane lane() const = 0;               // local -> fv-speech, cloud -> pool, remote -> net
    virtual Transcript transcribe(const AudioRef&, const Options&, const CancelToken&) = 0;
    virtual void warm_up(const CancelToken&) {}
};
enum class Priority { final, remote_client, meeting_live, incremental, preview, background, warmup };
struct Job {
    Slot slot;                                   // dictation or meeting (selects the backend)
    Priority priority; AudioRef audio; Options options; CancelToken cancel;
    std::string coalesce_key;                    // a newer job with the same key replaces a queued one
    uint64_t generation;                         // engine generation captured at submit
};
class SpeechScheduler {
public:
    JobHandle submit(Job, Executor& reply_on, std::function<void(Result<Transcript>)>);
    void configure(Slot, BackendSpec);           // debounced reload happens in fv_appcore
    EngineState state(Slot) const;               // unloaded, loading, warming, ready, failed
    Signal<Slot, EngineState> state_changed;
    void release_local(Slot); void restore_local(Slot);   // meeting lease, see below
};
}
```

- **`LocalParakeetBackend`** wraps the existing `Recognizer`. Lane `fv-speech`. Audio goes in at
  the capture rate: the runtime resamples 8-96 kHz itself, so the dictation path has no 16 kHz
  re-decode at stop. The int16-to-float conversion into a reused scratch buffer is the only copy.
  Long audio is cut by the ported `WindowSplitter` (30 s maximum, quietest 0.1 s block after
  24 s) and decoded window by window, which is also how cancel works (below).
- **`CloudBackend`** (OpenAI, Groq, own OpenAI-compatible server). Lane: a pool strand with
  concurrency 3, matching `CHUNK_UPLOAD_CONCURRENCY`. Multipart upload through `http::Client`
  from the saved WAV or from tape-encoded WAV bytes; files over 23 MB use the ported
  silence-aware splitter. Cancel aborts the reply at once; no automatic retries, as Python.
  Behaviour of the isolated upload worker is kept, the process is not.
- **`RemoteBackend`** (fv_remote client). Lane `fv-net`. Resamples to 16 kHz with the ported
  `fft_resample` because the wire carries 16 kHz float32, frames per the OpenWhisper protocol,
  keeps the busy-retry and reconnect rules. Cancel abandons the request but keeps the connection.

On the host side, the remote-engine server submits client requests to the same scheduler with
`Priority::remote_client`, so a host can dictate locally while serving, and engine identity changes
come from `state_changed`.

### Live preview by window decoding

Parakeet TDT rejects `streaming_recognize`, so `fv_speech::WindowPreview` (port of
`StreamingTranscriber`) watches the active recording's PcmTape: every 3.0 s of new audio it
submits a window of 0.75 s overlap plus the new audio as `Priority::preview` with coalesce key
`preview:<job>`. If the engine is still busy with the previous window, the queued one is replaced,
so preview latency never grows. Text joins with the ported `append_preview_text` (bounded seam
de-duplication). Stop never decodes; `finalize_preview` decodes the leftover only when the final
transcript comes back empty, exactly as OpenWhisper's lazy tail. The overlay header gets text
through the `Mailbox`.

### Incremental dictation

Once 30 s are captured, `IncrementalSession` cuts windows from the tape with the same
`WindowSplitter` the file path uses and submits each as `Priority::incremental`. At stop only the
remaining windows go in as `Priority::final`. Live and file windows are the same samples from the
same tape, so they match by construction; the CRC of the tape against the saved WAV's data chunk
stays as a one-line safety check. Invalidation rules (engine reload, mic switch, language or
vocabulary change, cancel, meeting start) are ported from a05.

### Dictation, uploads and meetings share one Recognizer

There is one `Recognizer` per (runtime, model, device) key, owned by the scheduler, cached with a
maximum of two resident models. The priority order is the policy:
`final > remote_client > meeting_live > incremental > preview > background > warmup`.

- Dictation's final pass jumps every queued preview, which is OpenWhisper's "final pass starts at
  once" rule without a second engine.
- Uploads are `final` jobs over `TapeView`s of the decoded file, window by window, so a long
  upload never starves a dictation for more than one window.
- Meeting chunk decodes are `meeting_live`; the offline re-decode at End is `background` windows.
  Dictation is refused during a live meeting anyway (a03 gates), and unlocks once capture ends,
  when re-decode windows simply yield to it.
- OpenWhisper's model *lease* existed to keep one Whisper resident. When the meeting uses the
  dictation model it becomes a no-op; when `meeting_speech_separate` picks another model, the
  scheduler loads it and releases the dictation model for the meeting's duration, which keeps
  OpenWhisper's memory profile and its `release_local_engine`/`restore_local_engine` messages.

### Cancel

The C ABI has no abort. Every job is a sequence of windows of at most 30 s; the scheduler checks
the job's token between windows and drops queued windows at once. Worst-case cancel latency is
one window decode, well under a second on the GPU runtime. Results carry the generation captured
at submit, and anything from an older generation is dropped (a14's generation protocol, without
the worker process).

### Runtime and model lifetime

- The first successful load fixes the process runtime (CPU or one GPU build). Its DLLs, and every
  ggml dependency, stay loaded until exit. Loading a second NeMo build in the same process is
  refused because the ggml DLL names collide and Windows would bind the second runtime to the
  first one's modules.
- Changing the runtime (CPU to CUDA, or an update or uninstall of the loaded runtime) is staged
  and applied at the next start, with a "Restart OpenWhisper_CPP to use the GPU runtime" prompt. This is
  a deliberate difference: OpenWhisper killed its worker process instead.
- Model or device changes within a runtime destroy and recreate the Recognizer on `fv-speech`
  (destroy is safe; only `FreeLibrary` is not). Debounce (400 ms), warm-up (one short non-silent
  decode, never queued behind another job) and the reload state machine live in
  `fv_appcore::EngineReloadMachine` and are golden-trace tested against `test_engine_warmup.py`.
- Crash containment replaces process isolation: the unhandled-exception filter records the
  faulting module; if it lies in the NeMo or ggml DLLs, the next start shows "The speech engine
  crashed" with an offer to switch to the CPU runtime, and the minidump goes to the diagnostics
  export.

---

## 6. Third-party choices

| Need | Choice | Runner-up | Why |
|---|---|---|---|
| Audio capture and playback, WASAPI loopback | **miniaudio** 0.11.26 (WASAPI backend; loopback device type; one implementation TU) | PortAudio 19.7 | One header covers mic, loopback and playback with low-latency WASAPI and later Pulse/CoreAudio; vcpkg PortAudio has no loopback, so it would still need hand-written `IAudioClient` code. Saved `{name, hostapi}` keys still match through the ported ladder (WASAPI names equal PortAudio's WASAPI and DirectSound names; the MME 31-character rung covers MME entries). |
| HTTP server, WebSocket server and client | **Boost.Beast + Asio** 1.92 (with asio::ssl/OpenSSL) | libhv 1.3.4 | One stack for every inbound need: Range/206, SSE, WS upgrade on the dashboard port, a TLS WebSocket host with pinned clients for the remote engine, the pinned-address webhook client. Beast is mature and permissive; its compile cost is fenced inside `fv_net`. Rejected: cpp-httplib plus IXWebSocket (two stacks, no WS on the HTTP port), Drogon (framework-sized). |
| Outbound HTTPS | **QtNetwork** (QNAM on `fv-qtnet`) | Beast client | System proxy, OS trust store, HTTP/2 and connection pooling for free; the core sees only `http::Client`. |
| JSON | **nlohmann-json** 3.12 (`ordered_json`) | QJsonDocument | Keeps insertion order and int/float/bool apart; `QJsonObject` sorts keys and merges int and double, which breaks settings round trips, prompts and meeting state. |
| SQLite access | **sqlite3** 3.54.1 C API (`[fts5]`) behind `fv::db` | QtSql QSQLITE | Online backup API, FTS5, URI read-only mode, custom functions, WAL control, and no Qt in the core. |
| Zip and .docx writing | **miniz** 3.1.2 (Zip64, deflate, gzip inflate) + hand-written OOXML templates | libzip 1.11.4 | One small library covers `.owbackup`/`.fvbackup`, DOCX, knowledge-folder OOXML reading, component zips and (with a 150-line tar reader) the Linux runtime `.tar.gz`. |
| XML reading (DOCX, PPTX, XLSX text) | **pugixml** 1.16 | QXmlStreamReader | Qt-free, tiny, used only by `fv_meeting` and DOCX tests. |
| PDF writing | **QPdfWriter + QTextDocument** (QtGui) | libharu | The same layout engine OpenWhisper's pagination code drives, so its workarounds port line by line. |
| PDF text extraction (knowledge folder) | **PDFium** (pinned pdfium-binaries DLL, loaded by path, headers vendored) | own minimal extractor | pypdf-level fidelity without vcpkg; shipped in the installer, loaded lazily on first use. |
| Markdown | **QTextDocument::setMarkdown** (md4c inside Qt) for display; ported regex code for snippets and release notes | md4c 0.6 | Same reader PyQt used, so the Python styling passes port directly. |
| Resampling and DSP | **pocketfft** (vendored header if the baseline lacks the port) for the ported `fft_resample`; the runtime's own resampler for ASR input; swresample only inside file decode | libsamplerate | `fft_resample` is numpy's pocketfft algorithm, so spool chunks and golden vectors match bit-near; nothing else needs a resampler. |
| Audio file decoding | **FFmpeg** (vcpkg, `avcodec avformat swresample`, LGPL, shared) | Media Foundation | PyAV decodes whatever FFmpeg decodes; Media Foundation misses Ogg/Opus/WebM. |
| Speaker embeddings | **onnxruntime** C API, official release DLL pinned in the components catalog, loaded by path | vcpkg onnxruntime | Downloaded with the WeSpeaker model on first meeting, as OpenWhisper fetches the model; no 40-minute vcpkg build, no installer growth. CPU EP, 1 thread. |
| Credentials | **Windows Credential Manager** directly (`wincred.h`) | qtkeychain | qtkeychain depends on Qt (vcpkg would build Qt) and lacks lookup-only reads and timeouts. |
| Crash reporting | **SetUnhandledExceptionFilter + MiniDumpWriteDump** plus terminate, purecall and invalid-parameter handlers | crashpad | No handler process to ship; minidumps land in `<data>/crashes/` and the crash log keeps OpenWhisper's file name pattern. |
| Global hotkeys and input injection | **Win32 `WH_KEYBOARD_LL`/`WH_MOUSE_LL` + `SendInput`**, key-name table generated from the Python `keyboard` library | `RegisterHotKey` | Push-and-hold, suppression and physical-key repeat need the hook; `RegisterHotKey` cannot see releases. |
| Screenshots | **QScreen::grabWindow** per screen, `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` for our overlays, Win32 window-under-pointer rects | Windows.Graphics.Capture (cppwinrt) | What the Python Snap does today; WGC kept in reserve for HDR monitors. |
| Unicode semantics | **PCRE2** 10.49 (UTF + UCP) and **utf8proc** (full case folding, categories) | ICU | Python's `re` and `casefold` behaviour at a fraction of ICU's 30 MB. |
| Logging | **own `fv::log`** (std::format, one writer thread) | spdlog 1.17 | The exact Python line format, a failure-journal sink and never-block semantics are 400 lines; spdlog would still need all three customised. |
| Tests | **Catch2** 3.16 (cores), **Qt Test** (widgets) | — | Already in the tree. |
| UIA / COM helpers | **wil** | raw COM | RAII for COM, handles and errors in `fv_input`. |

Not used: the Qt modules missing from the kit (Multimedia, WebSockets, HttpServer, Pdf,
WebEngine), libcurl, crow, uWebSockets, simdjson.

---

## 7. Data and identity

### Folder layout (OpenWhisper_CPP's own, `%LOCALAPPDATA%\OpenWhisper_CPP` unless `OPENWHISPER_CPP_DATA_DIR`)

```
openwhisper_cpp_settings.json          OpenWhisper's key set and JSON shape (indent 2, insertion order)
openwhisper-cpp.db (-wal, -shm)        OpenWhisper schema v18, unchanged
recorded_audio.wav  .recorded_audio-recovery\<32hex>\{active.lock,session.json,audio.pcm,discarded}
recordings\  meetings\<id>\ (spools)  captures\ (walkthroughs)  remote_records\
scratchpad.txt  mcp_event_subscriptions.json  backup-preferences.json  .env
openwhisper-cpp.log(.1-.3)  openwhisper-cpp.crash.log  crashes\*.dmp  diagnostic-failures.json  diagnostic-metrics.json
.openwhisper-cpp-data.lock  .openwhisper-cpp-restore\{pending.json, journal.json, stage-*, previous-*}
components\<id>\{manifest.json, .installed, ...}  speech-models\<key>\{installed.json, *.gguf}
models\speaker-id\  updates\  webui-cache\
```

Formats inside are OpenWhisper's byte for byte where tests or the importer care: journal
`session.json`, WAV `LIST`/`bext` tags (with `ISFT`/originator now "OpenWhisper_CPP"), component
manifests and sentinels, `installed.json`, walkthrough manifests (Python float `repr` and ISO
offsets), meeting spool sidecars.

### Settings

One JSON object, OpenWhisper's 186 keys with their exact kinds and fallbacks, generated by
`tools/gen/settings_keys.py` from the Python declarations (key, kind, default, choices,
aliases, clamp, strictness) into `src/core/settings/keys.inc`. Unknown keys are preserved on
rewrite. Dropped-engine keys stay declared with a `dropped` flag so imports round-trip and the UI
ignores them. OpenWhisper_CPP-only keys carry an `fv_` prefix (for example `fv_speech_runtime`) so they
can never collide with a future OpenWhisper key. The current QSettings keys in
`src/app/main_window.cpp` disappear.

### Database

Schema version 18 exactly as OpenWhisper's `services/models.py` and `services/database.py`
define it: tables, `AUTOINCREMENT`, FTS5 external-content tables and triggers, the
`meeting_segment_delivery` trigger, timestamp string formats, booleans as 0/1. Migrations v6-v18
are ported so any OpenWhisper database opens. OpenWhisper_CPP-only additions (an FTS5 index over
history text, see section 12) arrive as additive migration 19: new tables and triggers only, never
a changed column, so a downgrade to v18 is a `DROP`.

### Import from OpenWhisper

The importer is the backup restore pipeline pointed at a live folder, not a new mechanism:

1. **Detect** (guided setup welcome page, and Settings > Backup > "Import from OpenWhisper"):
   `OPENWHISPER_DATA_DIR`, then `%LOCALAPPDATA%\OpenWhisper`, or a folder the user picks.
2. **Exclusive check:** take OpenWhisper's data lease exclusively for the duration; failure means
   OpenWhisper is running and the user is asked to close it.
3. **Stage:** `fv_data::create_backup(Identity::openwhisper(), source_root)` writes a stage
   directly into `.openwhisper-cpp-restore/stage-*` (no zip): SQLite online backup of
   `openwhisper.db`, settings transformed (Whisper, Moonshine, Qwen, MLX, Unified and Nemotron
   values of `selected_model` become `parakeet`; dropped keys kept but inert), recordings, meetings
   and captures **hard-linked** when on the same volume, copied otherwise. Meeting `spool_dir` and
   absolute history paths are rebased with the existing restore rebasing code.
4. **Credentials:** every name OpenWhisper's code reads (`OPENAI_API_KEY`, `OPENROUTER_API_KEY`,
   `GROQ_API_KEY`, `OPENCODE_*`, `TYPESAFE_API_KEY`, MCP token, event and webhook secrets, the
   remote-engine token) is read with OpenWhisper's WinVault target convention and written under
   `OpenWhisper_CPP`. `OPENWHISPER_*` names map to `OPENWHISPER_CPP_*`.
5. **Apply** at the next start under the exclusive OpenWhisper_CPP lease with the existing journal and
   rollback. A `.owbackup` file is imported the same way through the normal restore page.

### What is shared with a live OpenWhisper install

Only downloaded speech runtimes and models, read-only. `Paths::openwhisper_sources()` lists
OpenWhisper's `components\` and `speech-models\`; the model and runtime resolvers accept a tree
there if its sentinel, manifest and `installed.json` validate. On first use OpenWhisper_CPP **adopts**
it: hard links into its own folders (same volume; copy otherwise), then loads from its own path.
OpenWhisper_CPP therefore never holds a file open in OpenWhisper's tree, so OpenWhisper's swap and
rollback logic keeps working, and OpenWhisper deleting its copy never affects OpenWhisper_CPP. OpenWhisper_CPP
never writes into OpenWhisper's folder. The remote engine shares nothing on disk; it talks the
OpenWhisper wire (`wss://host:47821/openwhisper/v1/engine`, `OWHISPER?1` discovery, SAS strings)
from a `remote::wire` constants file that is deliberately independent of `Identity`.

---

## 8. UI architecture

### Theme pipeline

`resources/theme/theme.qss` is OpenWhisper's 6,119-line sheet copied verbatim (636 object names,
79 tokens in use), with `palette.json` (96 tokens, dark and light) and the 59 Tabler SVGs, all in
a Qt resource. `fv_uicore::QssPipeline` is the pure part: rewrite `url(ui_qt/assets/...)` to
`url(:/assets/...)` and `@icon-suffix` first, substitute `@tokens`, then `scale_qss_fonts`
(half-to-even through `fv_pytext::round`). It is golden-tested against the
`app-<theme>-s<scale>.qss` dumps from `render_reference.py`. `fv_uikit::ThemeEngine` caches the
resolved sheet per (theme, scale), applies it with `qApp->setStyleSheet`, builds the `QPalette`,
sets the app font (Segoe UI, 14 px at 100 %; 9/10/12/13 pt at 90/100/115/130 %) and follows the
OS scheme for "Match system" (`QStyleHints::colorScheme`).

Widget-local sheets go through `fv::setThemedStyleSheet(widget, source)`, which stores the
unresolved source in a dynamic property; a theme or scale change walks only widgets that carry it.
Native gain: OpenWhisper re-resolves through an app-wide event filter on every StyleChange; here a
theme switch is one pass and a typed `Theme` struct of `QColor`s replaces per-paint token string
parsing in painted widgets (progress bars, switches, overlays, charts).

### Shared widgets (fv_uikit, ported in M0 and M1)

Button family (`Button`, `PrimaryButton`, `SuccessButton`, `DangerButton`, `WarningButton`,
`neutralButton()`, `compactPrimaryButton()`) with `HotkeyHintFilter`; `Card`, `HeaderCard`,
`ControlPanel`, collapsible card with the 300 ms InOutCubic animation; `WrappedLabel`,
`ElidingLabel`, `DecodeLabel`; `SettingsSwitch`; `SegmentedBar`; `TileBase`/`SettingTile`/
`FieldTile`/`InfoTile`; `NavRail`; `FieldHelp`; `FeatureBadge`; `EmptyState`;
`ElidingComboBox`, `NoWheelComboBox`, `NoWheelSpinBox`, `SearchableComboBox`;
`AnimatedProgressBar`; `TabbedContentWidget`; `ButtonRow`, `Reflow`, `FlowLayout`,
`StudioSplit`/`Stage`, `ScrollingBody`; `UpdateNoticeButton`; context menu helper;
`revealInFileManager`; `DragToMove`. Hotkey capture fields live in `fv_ui_settings`.

### PyQt to C++ mapping rules

- One Python module maps to one `.h/.cpp` pair in the feature library:
  `ui_qt/widgets/history_sidebar.py` becomes `src/app/features/history/history_sidebar.{h,cpp}`.
  Class names stay; private `_Foo` helpers go into the `.cpp` in an anonymous namespace.
- **The Qt base class stays identical** (`QFrame`, `QPushButton`...), because `theme.qss` selects
  on Qt class names, `#objectName` and `[property=value]` only (a33 confirmed no selector names an
  OpenWhisper class).
- Signals and methods go snake_case to camelCase (`record_toggled` to `recordToggled`);
  `pyqtSignal(object)` payloads become registered value structs; the ~75 assignable `on_*`
  callbacks of the controllers become signals.
- Python properties become `isX()`/`x()` getters; module constants become `static constexpr`.
- Layout numbers, timer intervals, easing curves and paint constants are copied literally with
  the Python line cited in a comment when the reason is not obvious.

### objectName and dynamic-property discipline

- Object names are set with literals at construction, verbatim from the Python.
- Dynamic properties are set only through `fv::setStyleProperty(widget, prop::tone, value)`
  (port of `set_style_property`: no-op when unchanged, unpolish and polish otherwise). Property
  names are constants in `app/uikit/style_props.h`, generated from the theme census (`tone`,
  `state`, `kind`, `buttonId`, `tileId`, `selected`, `muted`, `nearCap`, `overlay`, `stage`,
  `finalizationTone`, `meetingLink`, `meetingResultAction`, `scratchpadAction`, `starred`,
  `walkthroughAction` and the rest).
- `tools/lint/qss_names.py` fails the build when a `#name` or `[prop]` in `theme.qss` is used by no
  C++ file and is not on the allowlist of features not yet ported, or when C++ sets a property
  name that is not in `style_props.h`.
- The top-level-window property `openwhisperHotkeyWindow` becomes `openwhisperCppHotkeyWindow`
  (internal only).

### Controllers and widgets

Widgets never call services. They emit signals and expose setters. Each feature library has a
presenter QObject (for example `UploadPresenter`) that the feature's registration function
creates; it connects widget signals to runtime calls and runtime signals to widget setters. The
runtimes in `fv_runtime` call Qt-free cores and get results back through the Mailbox.
`MainWindow::updateScreen(key, call)` keeps OpenWhisper's replay of the latest call per method
for screens not built yet (lazy tabs stay, because they also keep the Settings and History scans
off the startup path). `UiController` in `fv_shell` is split into `OverlayRouter`,
`SettingsOpener`, `DownloadProgressPresenter`, `MeetingUiGlue`, `UpdateHandoff`,
`WalkthroughUiGlue` and `ClipboardService`, each its own file and package. An app test asserts
that every signal of MainWindow, tray and overlay is connected (the "missed connection" risk in
a03). The Settings window's staged idle build is dropped: in C++ the shell and the first page
build in milliseconds, and pages stay lazily built on first visit.

### Where strings live

Inline, verbatim, at the point of use, as in the Python, so a reviewer can diff a C++ file against
its Python source. UTF-8 source (`/utf-8` is already on), `QStringLiteral(u"...")` in Qt code,
`std::string` literals in cores. The product name comes from `brand.h`
(`kProduct = "OpenWhisper_CPP"`) and only from there; `tools/lint/strings.py` rejects the literal
"OpenWhisper" outside `core/data/import`, `core/remote/wire` and tests. Copy that cores must
produce for golden tests (status lines, readiness messages, batch lines, consent text) lives in
the core's copy builders. Large text data (cleanup prompts, catalogs, model and component rows,
meeting prompts, keyboard names) is generated from the Python source into `resources/data/*.json`
and embedded with `fv_embed()`. No `tr()`: OpenWhisper is English-only and adding i18n is out of
scope.

---

## 9. Testing and parity gates

### Test targets and layout

- `tests/core/<lib>/<module>/` with Catch2 sources, auto-discovered into `fv_<lib>_tests`. Test
  names keep the Python test name: `"[recorder_postroll] quiet window ends post-roll"` with a
  `// from tests/test_recorder_postroll.py::test_...` comment.
- `tests/app/<feature>/` Qt Test classes, registered into one `fv_app_tests` with a custom main
  that runs one class per `ctest` entry, so failures are isolated and CTest still sets
  `QT_QPA_PLATFORM=offscreen`.
- `tests/parity/<feature>/` builders for screenshot cases; `fv_parity_tests`, Windows only,
  label `parity`, pinned environment (`QT_QPA_FONTDIR=%WINDIR%\Fonts`, `QT_SCALE_FACTOR`,
  `QT_FONT_DPI=96`, base style set after `QApplication` exists).
- `tests/fixtures/`: fake settings stores, a Python-made `openwhisper.db` at v5 and v17 for
  migrations, sample WAVs, Python-made `.owbackup`, recorded MCP and dashboard transcripts.
- C++ seams mirror the Python fakes: `FakeClock`, in-memory `Store`, `MemoryCredentialBackend`,
  `FakeHttpClient` (scripted responses), `FakeCaptureSource`, `FakeBackend` for speech,
  `FakeProcessLauncher`, null focus service.

### Golden vectors

`tools/parity/golden_vectors.py` (curated edge cases, one `cases/<area>.py` per area) and
`trace_plugin.py` (inputs harvested from OpenWhisper's own tests) write
`tests/golden/<area>/<module>.json`, each with the OpenWhisper source commit inside. A generic
driver in `tests/golden_replay/` (`load_vectors`, typed `DYNAMIC_SECTION` per case, tagged
float/NaN/`$repr` values) replays them; a numeric tolerance is stated per file for DSP (1e-6
absolute for float32 unless the Python test says otherwise). Vectors are regenerated only when the
reference commit moves, and a diff in a vector file is reviewed as a behaviour change.

### Reference screenshots

`tools/parity/render_reference.py` gains a builder per widget and dialog (`builders/<area>.py`)
with fixed fake data, pinned clock, time zone, seeds, focus and size; it writes
`tests/parity/reference/<widget>/<theme>-<style>-dpr<n>-s<scale>.{png,json}` with the tree dump.
The C++ case builds its own widget from the same fake data and checks in two steps: tree against
dump (geometry, texts, object names, properties: readable failures), then pixels against the PNG
with a magenta diff image on failure. Default variants: dark and light at 100 %, plus 130 % for
anything with reflow or fit logic. Animated and painted widgets (overlay looks, LiveDot, agent
tiles) are captured at fixed animation times. References are committed. The build kit should move
to Qt 6.11.x, the version PyQt6 6.11.0 bundles, to remove the minor-version risk.

### Where the harness lives

`out/parity-scratch` moves into `tools/parity/` (Python: `ow_isolation.py`, `classify_tests.py`,
`golden_vectors.py`, `trace_plugin.py`, `render_reference.py`, `coverage_table.py`, new
`status.py`) and `tests/golden_replay/` plus `tests/parity/` (C++). `tools/parity/README.md`
documents the commands. `docs/port/parity-status.json` holds, for each of the 548 OpenWhisper
test files, `ported`, `golden`, `not applicable (dropped)` or `pending`, keyed by
`classification.json`; `status.py` prints the per-area table.

### What a package must pass before merge

1. `cmake --workflow --preset windows-ci` green: configure, build with `/WX` on our targets, all
   tests including the package's new ones.
2. `cmake --workflow --preset linux-core` compiles the Qt-free libraries it touched (WSL).
3. `clang-format --dry-run -Werror` clean; lints green: layering, Qt-free, `qss_names`, `strings`,
   no edits outside the package's owned directories (a script compares the diff with the package
   manifest).
4. Every golden-vector candidate the map lists for the package's modules has a vector file and a
   passing replay.
5. Every OpenWhisper test file assigned to the package is `ported` or `golden` in
   `parity-status.json`, or `pending` with a reason and the package that will finish it.
6. Every new widget or dialog has a parity case that passes (0 differing pixels, or a documented
   per-case tolerance the lead approved).
7. No new third-party dependency, settings key or style property: those come from the lead.

---

## 10. Build

### Conflict-free structure

- Each library directory holds a short `CMakeLists.txt` calling
  `fv_add_library(fv_audio LAYER C3 DEPS fv_settings THIRD_PARTY miniaudio ffmpeg)` followed by
  `fv_discover_modules()`, which globs `*/CMakeLists.txt` one level down (`CONFIGURE_DEPENDS`) and
  adds each. A module directory's file is a few lines:
  `fv_module(SOURCES ... WIN32_SOURCES ... TESTS ... GOLDEN ...)`, plus `fv_feature(...)`,
  `fv_embed(...)`, `fv_qt_test(...)` and `fv_parity_case(...)` where needed. A package adds
  directories only; it never edits a parent file, the registry or the test runner.
- Generated, never committed: `generated/registry.cpp` (feature registrations sorted by order,
  then name), the Qt test class list, the embedded resource tables.
- Lead-owned shared files, edited only by milestone-start packages: root `CMakeLists.txt`,
  `CMakePresets.json`, `vcpkg.json`, `cmake/*`, `resources/theme/*`, `keys.inc`,
  `destinations.h`, `style_props.h`, `CLAUDE.md`.

### Build-time controls

- Precompiled headers per layer: `cmake/pch/core.h` (std, `<format>`, nlohmann), `qt_widgets.h`.
- Expensive headers fenced: Beast and Asio only in `fv_net` `.cpp` files, compiled once with
  `BOOST_ASIO_SEPARATE_COMPILATION`/`BOOST_BEAST_SEPARATE_COMPILATION`; miniaudio's implementation
  in one TU; FFmpeg headers only in `core/audio/file/`; Windows SDK headers only in `win/`
  directories behind interfaces.
- `OPENWHISPER_CPP_UNITY=ON` (unity builds) for CI; off for development.
- Budget: one package's incremental build under 60 s; a cold worktree build of the finished app
  under 10 minutes on 16 cores.

### vcpkg additions (pinned baseline unchanged)

`nlohmann-json`, `pcre2`, `utf8proc`, `sqlite3` with `fts5`, `miniz`, `pugixml`, `miniaudio`,
`ffmpeg` with `avcodec`, `avformat`, `swresample` (no GPL features), `boost-beast` (pulls
`boost-asio`), `openssl`, `wil`, `catch2` (existing), and `pocketfft` if the baseline has it
(else a vendored header). Not added: onnxruntime, PDFium (runtime-loaded), Qt anything.
Workers share one binary cache (`VCPKG_DEFAULT_BINARY_CACHE=D:\vcpkg-cache`) so a fresh worktree
restores FFmpeg, Boost and OpenSSL as archives instead of building them; the first build of
FFmpeg (about 15 minutes) happens once, in M0.0.

### Presets

Keep the existing ones; add `windows-ci` (RelWithDebInfo, warnings as errors, unity build),
`windows-core` (`OPENWHISPER_CPP_BUILD_APP=OFF`, cores and tests only, fastest loop for core packages),
`linux-core`, a `parity` test preset (label `parity`, pinned environment), and matching workflow
presets. The decode tests keep `OPENWHISPER_CPP_TEST_RUNTIME_DIR`, `OPENWHISPER_CPP_TEST_MODEL` and
`OPENWHISPER_CPP_TEST_WAV`.

### Qt floor

Raise the floor from 6.5 to **6.8 LTS**. The 6.5 floor buys nothing on Linux (Ubuntu 24.04
ships 6.4), while 6.8 brings `QStyleHints::setColorScheme` (title bar and "Match system") and the
Markdown fixes the walkthrough PDF code relies on. Develop and take parity references on 6.11.x.

---

## 11. Milestones

Each milestone ends in an app that starts, runs and can be demonstrated.

1. **M0 Foundation** (a01 core and shell frame, a02 theme and leaf widgets, a03 skeleton, a09 DB
   core, a10 store/keys/credentials, a14 scheduler, a33 harness). Demo: splash, then the themed
   frameless window with title bar, menus, footer, tray and four lazy tabs; a WAV dropped on the
   Upload placeholder is transcribed through `SpeechScheduler`; `openwhisper-cpp-cli --version`,
   `--diagnostics out.json`.
2. **M1 Dictation** (a04 transcription/hotkey/streaming runtimes, a05, a06 Windows, a07 Bars and
   Classic, a08 Quick Record and single upload, a09 history core and sidebar, a03 overlay routing).
   Demo: press `kp *` in Notepad, speak with live preview in the overlay, text is pasted, the entry
   appears in History and plays back; an MP3 upload transcribes.
3. **M2 Settings, models and personalization** (a10 resolvers and backup, a11, a12, a13, a14
   components and model cache, a15, a06 capture fields). Demo: full Settings window; download the
   CUDA runtime and Parakeet model from Downloads; AI cleanup through OpenRouter; dictionary,
   snippets, styles, Command Mode and transforms; backup and restore.
4. **M3 History, uploads, cloud speech, import and installer** (a09 calendar, export, stats; a08
   batch, scratchpad, viewer; a05 splitting; a14 cloud backend; a07 remaining looks; a10 import;
   a16 update check and setup-exe update; a34 Inno installer). Demo: install with the Inno
   setup, import an OpenWhisper data folder, browse the calendar and Stats, dictate through Groq,
   update to a newer build.
5. **M4 Walkthroughs** (a22, a23, a04 walkthrough runtime, Auto Mode). Demo: record a walkthrough
   with Snap and live drawing, review the storyboard, export PDF, Word and Markdown.
6. **M5 Meetings** (a24, a25, a28, a30, a31, a04 meeting runtime; transcript-only intelligence).
   Demo: a meeting with mic and system audio, live captions in the browser dashboard and on a
   guest's phone, local speaker labels, exports, kill and recover.
7. **M6 Agents and meeting intelligence** (a26, a27 native loop, a29, a20 installed agents and
   process ownership, a12 agent cleanup runners, a22 write-up). Demo: live notes and the final
   report through OpenRouter, a Claude Code report, a walkthrough write-up by Codex.
8. **M7 Automation and MCP** (a19, a20, a21). Demo: "Route by voice" to a webhook, Claude Code
   using the MCP server and channel, `openwhisper-cpp-cli --api` serving history.
9. **M8 Remote engine** (a17, a18, a01 Host Mode). Demo: OpenWhisper_CPP dictates through an
   OpenWhisper host and serves an OpenWhisper client.
10. **M9 Native updater and release** (a16 transactional updater, a34 CI, signing, health gate,
    performance budgets). Demo: a signed release updates itself, rolls back after a forced
    failure, and passes the health gate.

### M0 work packages

| ID | Package | Owned directories | Depends on | C++ lines |
|---|---|---|---|---|
| M0.0 | Skeleton and contracts | root CMake, `cmake/`, presets, `vcpkg.json`, every library's root `CMakeLists.txt`, contract headers of section 3, `destinations.h`, `style_props.h`, `tools/lint/`, CLAUDE.md update | — | 1,800 |
| M0.1 | Base | `src/core/base/*` (identity, paths, env, log + journal + metrics, crash, clock, cancel, pool/strand, atomic files, data lease, process + job objects, process env, profiler, diagnostics report) | M0.0 | 3,400 |
| M0.2 | Python text semantics | `src/core/pytext/*` (code points, half-even round, float repr and `%g`, casefold, PCRE2 regex, split, `json.dumps`, difflib) | M0.0 | 2,600 |
| M0.3 | Parity harness home | `tools/parity/`, `tests/golden_replay/`, `tests/parity/framework/`, first vectors (format_utils, fuzzy, loudness, titles) | M0.0 | 1,200 |
| M0.4 | Theme pipeline | `src/core/uicore/qss/`, `src/core/uicore/palette/`, `src/app/uikit/theme/`, `resources/theme/` | M0.0 | 1,900 |
| M0.5 | Settings store and keys | `src/core/settings/{store,kinds,keys,resolvers}/`, `tools/gen/settings_keys.py` | M0.1, M0.2 | 3,500 |
| M0.6 | Credentials | `src/core/settings/credentials/` (WinCred, memory, dotenv, resolution, OpenWhisper reader) | M0.1 | 1,300 |
| M0.7 | Database core | `src/core/data/db/`, `src/core/data/schema/` (v18, migrations v6-v18) | M0.1, M0.2 | 2,400 |
| M0.8 | Speech scheduler | `src/core/speech/{scheduler,local,windows}/`, `src/engine` (window splitter, GPU probe via NVML by path) | M0.1 | 2,300 |
| M0.9 | Leaf widgets | `src/app/uikit/widgets/` (buttons and hint, labels, cards, switch, segmented bar, tiles, combos, empty state, progress, tabbed content, button row, reflow, flow layout) | M0.4 | 3,600 |
| M0.10 | Qt bridge | `src/app/bridge/` (Mailbox, QtHttpClient, SettingsRelay, string and path conversions, executor bridges) | M0.1, M0.5 | 1,400 |
| M0.11 | Shell frame | `src/app/shell/` (Application, Bootstrap, LoadingScreen, MainWindow frame, TitleBar, footer, SystemTray, SingleInstance, lazy tabs, geometry basics), `src/gui/` | M0.9, M0.10 | 3,800 |
| M0.12 | Runtime skeleton | `src/core/appcore/{engine,gates,overlay_state,throttle}/`, `src/app/runtime/{app_context,controller}/`, `src/cli` modes `--version --diagnostics` | M0.8, M0.11 | 2,700 |

Waves: M0.0; then M0.1, M0.2, M0.3, M0.4; then M0.5, M0.6, M0.7, M0.8, M0.9; then M0.10; then
M0.11; then M0.12. About 32k lines.

### M1 work packages

| ID | Package | Owned directories | Depends on | C++ lines |
|---|---|---|---|---|
| M1.1 | Audio devices, capture, tape, player | `src/core/audio/{devices,capture,tape,player,dsp}/` (miniaudio, identity ladder, health, SPSC rings, level meter) | M0 | 2,800 |
| M1.2 | Journal and WAV | `src/core/audio/{journal,wav}/` (journal, recovery, atomic WAV, `LIST`/`bext` tags) | M0 | 1,500 |
| M1.3 | Hotkey core and Windows hook | `src/core/input/{hotkeys,dispatch,hold}/`, `src/core/input/win/hook/`, `resources/data/keyboard_names.json` | M0 | 3,000 |
| M1.4 | Paste, clipboard and focus | `src/core/input/{focus,catalog,join}/`, `src/core/input/win/{synthetic,foreground,uia}/`, `src/app/runtime/clipboard/` | M0 | 3,000 |
| M1.5 | History core | `src/core/data/{history,format,titles,stats}/` | M0 | 2,600 |
| M1.6 | Overlay | `src/core/uicore/overlay/`, `src/app/overlay/` (window, state machine, Bars, Classic particles, moments, chip, badge) | M0 | 3,300 |
| M1.7 | File decoding and batch types | `src/core/audio/file/` (FFmpeg reader, preview, decode to tape), `src/core/appcore/batch/` | M0 | 1,800 |
| M1.8 | Recorder | `src/core/audio/recorder/` (state machine, post-roll gate, pause, cancel, fallback and mid-recording switch) | M1.1, M1.2 | 2,200 |
| M1.9 | History UI | `src/app/features/history/{sidebar,item,edge_tab,entry_dialog,playback}/` | M1.1, M1.5 | 2,800 |
| M1.10 | Quick Record tab | `src/app/features/record/{tab_base,transcript_pane,quick_record,compact}/` | M0 | 2,200 |
| M1.11 | Upload tab | `src/app/features/record/upload/` (drop zone, queue, file card, progress panel) | M1.7 | 2,500 |
| M1.12 | Dictation runtime | `src/core/appcore/{job,pipeline}/`, `src/app/runtime/transcription/` (job slot, start/stop/cancel, decode via scheduler, delivery, history persist, failure retention, upload jobs) | M1.4, M1.5, M1.7, M1.8 | 3,500 |
| M1.13 | Live preview and incremental | `src/core/speech/{preview,incremental,language}/`, `src/app/runtime/streaming/` | M1.8 | 1,700 |
| M1.14 | Hotkey runtime and overlay routing | `src/app/runtime/hotkeys/`, `src/app/shell/overlay_router/`, tray and menu wiring | M1.3, M1.6, M1.12 | 1,900 |

Waves: M1.1-M1.7 and M1.10 in parallel; then M1.8, M1.9, M1.11; then M1.12, M1.13; then M1.14.
About 35k lines.

---

## 12. Risks, and what the native app should do better

### Risks

1. **Python semantics drift** (rounding, code points, casefold, `\w`, `json.dumps`, `repr`,
   `difflib`, local-time conversion). Mitigation: `fv_pytext` plus golden vectors for every pure
   function before its port starts.
2. **In-process engine crashes take the app down.** Mitigation: faulting-module detection,
   minidumps, the CPU-runtime offer on the next start; nightly decode tests on both runtimes.
3. **Runtime switching needs a restart** (DLL name collisions, no unload). Mitigation: explicit
   prompt and staged switch; documented as a deliberate difference.
4. **Device identity after the move to miniaudio.** The Microphones list shows one WASAPI row per
   device instead of one row per PortAudio host API. Mitigation: the ported matching ladder maps
   every saved `{name, hostapi}` key; a fixture of real PortAudio device lists from OpenWhisper
   users replays through it. If the owner requires identical rows, swap the capture backend to
   PortAudio behind `DeviceCatalog` and keep miniaudio only for loopback.
5. **Worker drift on shared contracts.** Mitigation: contracts frozen per milestone, lead-owned
   shared files, the ownership lint, a contract-change log that milestone-start packages apply.
6. **Screenshot parity** depends on Qt 6.11 vs 6.10, Segoe UI (Windows-only gate) and focus/DPR
   pins. Mitigation: upgrade the kit to 6.11.x, pin as a33 documents.
7. **Protocol parity without SDKs**: MCP's two revisions, FastAPI's 422/404/405 bodies, the
   dashboard contract. Mitigation: record transcripts from the running Python servers and replay
   them against the pure handler functions.
8. **Windows hook removal and UIA hangs.** Mitigation: hook callback does no I/O and no locking;
   watchdog rehook kept; abandonable focus thread.
9. **Document fidelity** (PDF pagination quirks, DOCX that Word accepts). Mitigation: line-by-line
   port of the pagination workarounds; XML golden files taken from python-docx output, and every
   release's DOCX opened by hand in Word and LibreOffice.
10. **Build cost of FFmpeg and Boost** in many parallel worktrees. Mitigation: shared binary cache,
    fenced headers, PCH; FFmpeg built once in M0.0.
11. **Licensing**: FFmpeg LGPL (shared, decoders only), Beast/Boost BSL, PDFium BSD, onnxruntime
    MIT, miniaudio public domain, Tabler MIT. Notices generated from vcpkg copyright files.
12. **The dashboard rebrand needs Node at development time** (title, storage keys). Mitigation:
    vendored `resources/webui` source with committed `dist` and a freshness check.

### What the native app should do better (each keeps the visible behaviour)

- **Startup:** painted window in under 400 ms; model load, DB open and scans after first paint;
  no lazy-import machinery, no staged Settings build, no 8 s import-avoidance delays (the update
  check delay stays because it keeps network off the startup path).
- **Stop-to-paste latency:** no worker-process hop, no 16 kHz re-decode at stop, the final pass
  pre-empts previews, incremental windows already decoded; target p95 under 300 ms after the
  post-roll for a 10 s dictation on the GPU runtime.
- **Capture robustness:** lock-free callbacks remove the GIL-induced input overflows; the
  journal and level meter allocate nothing per block.
- **Idle footprint:** about ten threads and no interpreter; target idle RSS under 150 MB without a
  model loaded.
- **Cancel:** within one 30 s window instead of killing a worker process; uploads and HTTP calls
  abort their replies instead of leaving daemon threads running.
- **Processes:** job objects own every child (agents, hook programs, terminals), with
  kill-the-tree and disown semantics and no reader threads.
- **Model sharing:** meetings and remote clients reuse the loaded Parakeet model instead of the
  release-and-reload lease, so End-to-ready drops by a model load.
- **Downloads:** resumable with speed and ETA, hard-link adoption of OpenWhisper's 700 MB model,
  runtime and component state from a few `stat` calls (no "Checking downloaded models...").
- **Search:** an additive FTS5 index makes History search Unicode case-insensitive and fast
  (OpenWhisper's ASCII `LIKE` scans every keystroke); flagged as a deliberate improvement.
- **Diagnostics:** minidumps with the faulting module, crash-to-CPU-runtime recovery, and a GUI
  hang detector that logs the stuck thread, all without message text.
- **Real GPU choice:** the runtime's device list replaces Auto/GPU/CPU guessing on the Voice
  model page (presented within the same control so the look is unchanged).
