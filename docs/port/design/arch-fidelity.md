# OpenWhisper_CPP parity port: architecture, fidelity-first proposal

Lens: mirror OpenWhisper's module structure as closely as C++ allows. Every Python
module gets one obvious C++ home at the same relative path, every pytest file ports to
one C++ test file of the same name, and a reviewer can put a `.py` and a `.cpp` side by
side and read them line for line. The design departs from the Python structure only where
C++ or the hard constraints force it, and each departure is named where it happens.

Reference: OpenWhisper at `D:\coding\whisper_local`, commit `768b559edd93` (2.8.92), the
34 area maps in `docs/port/map`, and OpenWhisper_CPP at `4fbe53d`.

---

## 1. Principles

1. **One Python module, one C++ module, same path.** `services/recorder.py` becomes
   `src/services/recorder.{h,cpp}`, and `tests/test_recorder.py` becomes
   `tests/unit/test_recorder.cpp`. Why: workers cannot ask where something goes, and
   reviewers check parity file by file.
2. **Python identifiers are kept verbatim.** Functions, methods, signals, constants,
   `_private` members and class names keep their Python spelling. Each package becomes a
   namespace (`fv::services`, `fv::meeting::state`, `fv::ui_qt::widgets`). Why: `grep` for
   one name finds both sides, and the a03 wiring tables port as written.
3. **Qt appears only where Python imported PyQt6.** A C++ file may include Qt only if its
   Python counterpart imports PyQt6 or lives under `ui_qt/`: 21 non-UI modules plus the UI.
   Why: OpenWhisper already proved that split, and it keeps about 80% of the code testable
   with a compiler and Catch2 alone.
4. **Python semantics come from a library.** `fv::py` supplies `round`, code-point `len`,
   `casefold`, `re`, `json.dumps`, `datetime`, `difflib`, `%`-formatting, threading,
   executors, `subprocess` and `logging`. Ported code calls these and never hand-rolls a
   `std::` substitute. Why: the a33 replay already caught `std::lround` against
   half-to-even rounding, and there are hundreds of such traps.
5. **Each third-party Python package is replaced by a shim with the same shape.**
   `sounddevice` maps to PortAudio, `httpx` to libcurl, `fastapi`/`uvicorn`/`websockets` to
   Boost.Beast, `keyring` to WinCred, `sqlite3` to SQLite, and `av` to FFmpeg, each under
   `src/pylib`. Why: call sites then port line by line instead of being redesigned.
6. **Bytes that users or files see are copied verbatim.** That covers strings, object names,
   dynamic properties, settings keys, the database schema, JSON layouts and backup format.
   Only the brand is swapped, through one identity block. Why: the look must match, and an
   OpenWhisper install must import.
7. **Every departure is written down.** Each departure carries a `// departs: <reason>`
   comment at the site and a line in the package ledger. Why: parity stays auditable after
   70 parallel merges.
8. **The engine is one thread with OpenWhisper's worker protocol.** `SpeechProcess` keeps
   its Python API but runs a thread instead of a child process, and every NeMo call happens
   on that thread. Why: it meets the CLAUDE.md runtime rules, and Python's blocking call
   sites port unchanged.
9. **Parity is shown with evidence.** Every package ships golden vectors, a ledger of its
   ported pytest files, and reference screenshots for every widget it adds. Why: a merge
   should prove parity, not just compile.
10. **Shared build files are frozen after M0.** Modules are added by dropping files into
    mirrored folders, which CMake globs. Registries are one file per package. Why: parallel
    branches must merge without conflicts.

---

## 2. Libraries and directory layout

### 2.1 Tree

```
src/
  CMakeLists.txt        all targets, written once in M0 (globs, see section 10)
  config.{h,cpp}        <- config.py (AppConfig, paths, identity block)
  version.h.in          <- _version.py
  main.cpp              <- main.py (CLI dispatch, data lease, restart)
  engine/               existing Parakeet/NeMo engine (OpenWhisper_CPP-native, unchanged rules)
  py/                   Python stdlib semantics (OpenWhisper_CPP-native, no Python counterpart)
  pylib/                shims for third-party Python packages (OpenWhisper_CPP-native)
  services/             <- services/   (local_asr/ focus_context/ actions/ automation/
                           agent_api/ agent_mcp/ remote_asr/ remote_history/
                           remote_records/ runtime/ walkthrough/)
  transcriber/          <- transcriber/
  meeting/              <- meeting/    (agent/ agent/installed/ agent/native/ asr/ capture/
                           diarize/ export/ persist/ state/ web/)
  ui_qt/                <- ui_qt/      (dialogs/ overlays/ snap/ styles/ utils/
                           waveform_styles/ widgets/ assets/)
  cli/                  existing openwhisper-cpp-cli (engine smoke tool), unchanged
webui/                  vendored OpenWhisper webui/ (React source + committed dist/)
third_party/nemo_speech vendored C ABI header (unchanged)
tools/parity/           golden-vector and screenshot harness (moved from out/parity-scratch)
tools/lint/             Qt-include, QSS-name, brand-literal and ownership checks
tests/                  see section 9
```

**File naming.** Each module is named after its Python file. Platform variants use the
suffixes Python already uses (`_win`, `_mac`, `_linux`, `_x11`): `focus_context/_win.py`
becomes `focus_context/_win.cpp`. Two forced departures:

- `__init__.py` with code becomes `<package>/<package>.h` (`focus_context/focus_context.h`).
- A Python module that branches on `sys.platform` inside one file gets a portable header
  plus `<module>_win.cpp`, because C++ cannot hold the other platforms' bodies behind
  `if`s without their headers.

Dropped modules (Whisper, Moonshine, Qwen, MLX, Unified, isolated workers) get no file.

**The 21 Qt modules outside `ui_qt`.** These Python modules import PyQt6 directly:
`services/application_controller`, `backup_manager`, `omarchy_controls`, `release_checks`,
`runtime/*` (9), `actions/runtime`, `automation/app_control`, and
`walkthrough/{document,finish,library,session,word}`. Each keeps its mirrored path but
compiles into `fv_qt`. The list is fixed once in `cmake/OpenWhisper_CPPQtModules.cmake` (from an
AST scan of the Python, re-run in M0). `runtime/transcription.py` and the `runtime`
package are added to `fv_qt` too: neither imports PyQt6 itself, but both import Qt-side
runtime modules, and making them Qt-free would mean inverting most of `runtime/`.

**Inverted imports.** An import scan found exactly seven edges where a Qt-free Python
module imports a PyQt module. Two of them are the two `runtime/` modules just added to
`fv_qt`, and `package_checks` is dropped. The remaining four are inverted, not mirrored:

- `actions/agents` imports `actions/runtime`.
- `automation/filters` imports `actions/runtime`.
- `automation/service` imports `automation/app_control`.
- `walkthrough/write_up` imports the walkthrough document modules.

The names the Qt-free side uses move into a Qt-free `<module>_types.h` next to the Qt
module (`actions/runtime_types.h`). The Qt module implements an interface declared there.
That keeps `fv_core → fv_qt` one-directional, which static libraries need and Python never
did.

### 2.2 Targets

| Target | Kind | Directory | May link (Qt) | May link (third party) |
|---|---|---|---|---|
| `fv_engine` | static | `src/engine` | none | none; NeMo loaded by path. Builds on Linux. |
| `fv_py` | static | `src/py` | none | nlohmann-json, pcre2, pocketfft, zlib, OpenSSL libcrypto (hashes, CSPRNG), wil (Windows). Builds on Linux. |
| `fv_pylib` | static | `src/pylib` | none | curl, boost-beast/asio, openssl, sqlite3, minizip-ng, pugixml, portaudio, ffmpeg (avformat, avcodec, swresample), onnxruntime (feature `meeting`). Beast and asio stay inside `.cpp` files behind pimpl. |
| `fv_core` | static | `src/config.*`, `services/**`, `transcriber/**`, `meeting/**` minus the Qt list | none | via `fv_py`, `fv_pylib`, `fv_engine` only |
| `fv_qt` | static | `src/ui_qt/**` + the Qt list | Core, Gui, Widgets, Svg, SvgWidgets, Concurrent | via `fv_core` |
| `openwhisper-cpp` | exe (WIN32) | `src/main.cpp` | via `fv_qt` | — |
| `openwhisper-cpp-cli` | exe | `src/cli` | none | `fv_engine` (unchanged) |
| `openwhisper-cpp-updater` | exe, M9 | `src/services/app_update_apply_helper*` | none | `fv_core` subset |
| `fv_test_support` | static | `tests/support` | Core, Gui, Widgets, Test (Qt part only in `qt/`) | catch2, nlohmann-json |
| `fv_tests` | exe | `tests/engine` | none | existing engine tests |
| `fv_unit_tests` | exe | `tests/unit` | none | `fv_core` |
| `fv_qt_tests` | exe | `tests/qt` | Test | `fv_qt` |

Notes:

- Qt Network, Sql and Qml are not used. The HTTP client, servers and database sit in
  Qt-free code (principle 3); section 6 explains why.
- `fv_core` is one library, not one per Python package, because Python's import graph is
  cyclic (`settings ↔ app_styles ↔ focus_context`, `services ↔ meeting`). One static
  library absorbs the cycles the way Python's lazy imports did.
- Layering is enforced by `tools/lint/qt_includes.py`, not by target count.

### 2.3 Areas to targets

| Area | `fv_core` (Qt-free) | `fv_qt` | Other |
|---|---|---|---|
| a01 app shell | `config.*`, `services/{diagnostics,process_env,desktop_session,app_styles,ui_instance(_win)}` | `ui_qt/{app,bootstrap,loading_screen,main_window,startup_profiler,system_tray,whats_new}` | `main.cpp` → `openwhisper-cpp` |
| a02 UI foundation | — | `ui_qt/utils/*`, `ui_qt/widgets/*` (shared), `ui_qt/styles/theme.qss`, `ui_qt/assets/**` | Qt resources |
| a03 UI controller | — | `services/application_controller`, `ui_qt/{ui_controller,overlay_state}` | — |
| a04 runtime services | — | `services/runtime/*` | — |
| a05 recording/audio | `services/{recorder,audio_devices,audio_player,audio_processor,recording_journal,wav_metadata,streaming_transcriber,incremental_dictation,dictation_language,dictation_pipeline,dictation_stats}` | `ui_qt/widgets/{microphone_check,capture_delay}` | `pylib/{sounddevice,av}` |
| a06 hotkeys/paste/focus | `services/{hotkey_manager,_hotkey_common,_hotkey_keyboard,_mouse_hook_win,synthetic_keys,hotkey_conflicts}`, `services/focus_context/*` | `ui_qt/clipboard`, hotkey capture widgets, Hotkeys rows | — |
| a07 overlays | — | `ui_qt/overlays/*`, `ui_qt/waveform_styles/*` | — |
| a08 record/upload tabs | `services/batch_upload` | tabs, scratchpad, viewer, relation dialog | — |
| a09 history/stats | `services/{database,models,history_manager,history_export,history_calendar,format_utils,titles}` | history sidebar, calendar, export dialog, stats window | — |
| a10 settings services | `services/{settings,setting_types,credentials,backup,backup_startup}` | `services/backup_manager` | `pylib/keyring` |
| a11 Settings dialog | — | `ui_qt/dialogs/settings_*` | — |
| a12 text AI/personalize | `services/{text_llm,text_generation,text_model_catalog,thinking_levels,transcript_cleanup,cleanup_prompts,cleanup_profiles,text_transforms,text_rewrite,dictionary,vocabulary,snippets,recognition_context,typesafe,text_judge,local_text_servers,speech_endpoints,cleanup_agent_offer}` | — | `pylib/httpx` |
| a13 personalize UI | — | `ui_qt/dialogs/settings_{dictionary,snippets,styles,...}`, picker widgets | — |
| a14 models/components | `services/{components,component_catalog,component_leases,component_runtime,gpu_info,hf_access,model_catalog,verified_download,verified_files}`, `services/local_asr/{catalog,cache,audio,languages,nvidia,process,worker}`, `transcriber/{base,optional_backend,openai_backend,remote_backend}` | — | — |
| a15 downloads UI | — | `ui_qt/dialogs/{downloads,consent,...}`, row widgets | — |
| a16 updater/setup | `services/{app_update,update_contract,update_data,code_identity,setup_update,guided_setup,app_update_apply(_win)}` | `ui_qt/dialogs/{app_update_dialog,guided_setup}` | `openwhisper-cpp-updater` (M9) |
| a17 remote engine services | `services/{remote_asr,remote_history,remote_records}/*`, `services/lan_address` | — | `pylib/websockets` |
| a18 remote engine UI | — | settings page, pair dialog, host dashboard | — |
| a19 actions/automation | `services/actions/*`, `services/automation/*` (minus 2) | `services/actions/runtime`, `services/automation/app_control` | `pylib/fastapi` |
| a20 agent API/MCP | `services/{agent_api,agent_mcp}/*`, `services/{installed_agents,agent_process(_win)}` | — | `--api` mode in `main.cpp` |
| a21 actions UI | — | `ui_qt/dialogs/settings_{actions,hooks,mcp}`, agent picker widgets | — |
| a22 walkthrough services/Snap | `services/walkthrough/{storage,sections,styles,write_up,...}` | `services/walkthrough/{document,finish,library,session,word}`, `ui_qt/snap/*` | — |
| a23 walkthrough UI | — | walkthrough tab, panel, review, library dialog | — |
| a24 meeting engine | `meeting/{engine,end_run,finalization,refinalize,recovery,clock,time_utils,interfaces,platform,dev_fixture}` | — | — |
| a25 capture/ASR/diarize | `meeting/{capture/*,capture_session,asr/*,diarize/*,audio_playback}` | — | onnxruntime |
| a26 meeting agent | `meeting/agent/**` (without the sidecar supervisors) | — | — |
| a27 sidecars (TS) | `meeting/agent/native/*` (new: the native loop that replaces `sidecar/` and `sidecar-opencode/`) | — | TypeScript not ported |
| a28 state/persist/export | `meeting/{state,persist,export}/*`, `meeting/{content,stored,corrections}` | — | — |
| a29 intelligence | `meeting/{custom_report,insight_review,citation_verifier,live_signals,highlights,recall,context_folder,semantic_search,voice_commands,voice_actions,voice_help,fast_state,reinsight}` | — | — |
| a30 web server + dashboard | `meeting/web/{server,api,ws,auth,archive}` | — | `webui/dist` served |
| a31 meeting Qt UI | `services/{meeting_setup,meeting_rerun,meeting_agent_route}` | meeting tab, past meetings panel, 11 dialogs | — |

---

## 3. Cross-cutting services and their interfaces

The following are written in M0, before any area work. Sketches show the public surface.
Bodies follow the Python modules named in each header comment.

### 3.1 Identity and paths (`src/config.h`, from `config.py`)

```cpp
namespace fv::config {
// Identity block: the only place the brand lives. A different owner answer edits this
// block and nothing else.
inline constexpr std::string_view APP_NAME = "OpenWhisper_CPP";            // was "OpenWhisper"
inline constexpr std::string_view APP_SLUG = "openwhisper-cpp";            // file/folder stems
inline constexpr std::string_view ENV_PREFIX = "OPENWHISPER_CPP_";
inline constexpr std::string_view DATA_DIR_ENV = "OPENWHISPER_CPP_DATA_DIR";
inline constexpr std::string_view SETTINGS_FILENAME = "openwhisper_cpp_settings.json";
inline constexpr std::string_view DATABASE_FILENAME = "openwhisper-cpp.db";
inline constexpr std::string_view CREDENTIAL_SERVICE = "OpenWhisper_CPP";
// Protocol constants are wire, not brand (remote engine stays byte-compatible).
inline constexpr std::string_view REMOTE_ENGINE_PATH = "/openwhisper/v1/engine";

std::filesystem::path local_app_dir();        // %LOCALAPPDATA%\OpenWhisper_CPP
std::filesystem::path data_root();            // env wins, created, re-evaluated per call
std::filesystem::path components_root();      // <local_app_dir>/components
std::filesystem::path user_data_path(std::string_view name);
std::filesystem::path env_file_path();
std::optional<std::filesystem::path> openwhisper_root();   // departs: import source

struct AppConfig {                            // field names verbatim from config.py
    int SAMPLE_RATE = 44100;  int CHUNK_SIZE = 1024;  int HOTKEY_DEBOUNCE_MS = 300;
    std::filesystem::path SETTINGS_FILE, DATABASE_FILE, RECORDINGS_FOLDER, LOG_FILE;
    // ... about 200 constants, grouped as in config.py, incl. cleanup prompt sentences
};
const AppConfig& config();                    // Python: `from config import config`
}
```

### 3.2 Settings store (`services/setting_types.h`, `services/settings.h`)

```cpp
namespace fv::services {
using py::Json;                                 // nlohmann::ordered_json

template <class T> class Setting {              // setting_types.Setting
public:
    std::string_view key() const;  bool in_table() const;  T default_value() const;
    T parse(const Json& raw) const;                         // exact fallback rules
    T read(const Json* settings = nullptr) const;           // nullptr: load the file
    std::optional<T> stored(const Json* settings = nullptr) const;
};
// BoolSetting, IntSetting (Python int() semantics via py::int_), FloatSetting,
// StrSetting, ChoiceSetting, IntChoiceSetting, ListSetting, DictSetting,
// OptionalListSetting, OptionalDictSetting, RawSetting.

struct SettingsKey {                            // all 186 keys, in declaration order
    static inline const BoolSetting MINIMIZE_TRAY{"minimize_tray", true};
    static inline const ChoiceSetting SELECTED_MODEL{"selected_model", "parakeet", {...}};
    // ...
};

class SettingsManager {                         // settings.SettingsManager
public:
    explicit SettingsManager(std::filesystem::path settings_file = {});
    bool is_new_install() const;
    std::shared_ptr<const Json> load_all_settings() const;  // immutable snapshot
    Json load_settings(std::span<const std::string_view> keys) const;
    Json get(std::string_view key, Json default_value = nullptr) const;
    // Callback runs on the saving thread, as in Python. Returns the stop function.
    std::function<void()> watch(std::vector<std::string> keys,
                                std::function<void(const Json& changed)> callback);
    void save_all_settings(const Json& settings);
    Json update_settings(const Json& updates, std::span<const std::string> remove = {});
    Json mutate_settings(const std::function<void(Json&)>& mutator);  // throws: nothing written
    void save_setting(std::string_view key, Json value);
};
SettingsManager& settings_manager();
// resolve_* free functions keep their Python names and signatures.
}
```

Usage reads the same as Python: `SettingsKey::MINIMIZE_TRAY.read()`. The snapshot is a
`shared_ptr<const Json>`, not a pickle copy. Writes use `py::os::atomic_write` (temp file,
`FlushFileBuffers`, `MoveFileExW` with `REPLACE_EXISTING | WRITE_THROUGH`).

### 3.3 Python semantics (`src/py`)

`str.h` provides:
- `len` (code points), `casefold`, `lower`, `upper`, `isalpha`, `isspace`, `split`,
  `strip` and slicing, all on UTF-8 `std::string`;
- tables generated from Python 3.12's own `unicodedata` (Unicode 15.0) by
  `tools/parity/gen_unicode_tables.py`, so `ß` folds to `ss` exactly as in Python.

The other modules:

| Module | Python equivalent | Notes |
|---|---|---|
| `re.h` | `re` | PCRE2 with UTF and UCP; `compile`, `search`, `match`, `fullmatch`, `sub`, `split`, `findall`, `finditer`; `IGNORECASE` with Python's case rules |
| `format.h` | `round`, `repr(float)`, `%`, format specs, `json.dumps` | `round` is half-to-even (`std::nearbyint`); `repr(float)` is shortest round trip; `percent_format` handles `"%s %d %.1f"`; `format_spec` handles `{:.1f}` / `{:g}`; `dumps(value, indent, ensure_ascii, separators, sort_keys)` is byte-identical |
| `difflib.h` | `SequenceMatcher`, `get_close_matches` | `ratio`, `get_opcodes`, autojunk |
| `datetime.h` | `datetime`, `timezone`, `date` | `fromisoformat` (3.12 rules), `isoformat`, `astimezone`, `strftime` in the C locale; zone injectable for tests |
| `time.h` | `monotonic`, `time`, `perf_counter`, `sleep` | Behind a replaceable `Clock` for tests |
| `threading.h` | `Event`, `Lock`, `RLock`, `Condition`, `Thread`, `Timer`, `local` | — |
| `concurrent_futures.h` | `ThreadPoolExecutor`, `Future` | `shutdown(wait, cancel_futures)` matches Python, which QThreadPool cannot (a03) |
| `queue.h`, `os.h` (incl. `replace`, `fsync`, `_exit`), `tempfile.h`, `hashlib.h`, `secrets.h`, `base64.h`, `uuid.h` | same names | — |
| `subprocess.h` | `Popen`, `run`, `PIPE`, `CREATE_NO_WINDOW` default | Win32 `CreateProcessW` with pipes; `_posix` later |
| `logging.h` | `logging` | See 3.4 |
| `callbacks.h` | listener lists and callback attributes | `Listeners<Args...>` with `add() -> Subscription`, and `Callback<Sig>` for `on_*` |

### 3.4 Logging and diagnostics (`py/logging.h`, `services/diagnostics.h`)

```cpp
namespace fv::py::logging {
enum Level { DEBUG = 10, INFO = 20, WARNING = 30, ERROR = 40, CRITICAL = 50 };
class Logger {
public:
    template <class... A> void debug(std::string_view fmt, const A&... args);  // %-style
    template <class... A> void info(std::string_view fmt, const A&... args);
    template <class... A> void warning(std::string_view fmt, const A&... args);
    template <class... A> void error(std::string_view fmt, const A&... args);
    void exception(std::string_view fmt, const std::exception& e);  // type name + what()
    bool isEnabledFor(Level level) const;
};
Logger& getLogger(std::string_view name);      // name = Python module path
class Handler { public: virtual ~Handler(); virtual void emit(const LogRecord& r) = 0; };
void addHandler(std::shared_ptr<Handler> h);
}
```

- Each ported module declares `static auto& logger = py::logging::getLogger("services.recorder");`
  with its Python dotted name. That way `diagnostics.FailureCapture` maps components exactly
  as Python does.
- Log calls keep Python's `%s` literals verbatim.
- Formatter: `%(asctime)s - %(levelname)s - %(name)s - %(message)s`, rotating 5 MiB × 3.
- `services/diagnostics.cpp` ports `FailureCapture`, `MetricsCapture` (one writer thread)
  and `collect_report`.
- The crash handler (`ui_qt/bootstrap` via `services/diagnostics_win.cpp`, a departure
  because Python used `faulthandler`) is described in section 6.

### 3.5 Clock (`py/time.h`)

```cpp
namespace fv::py::time {
double monotonic();  double time();  double perf_counter();  void sleep(double seconds);
class Clock { public: virtual ~Clock(); virtual double monotonic() = 0; virtual double time() = 0; };
class ScopedClockOverride { public: explicit ScopedClockOverride(Clock& fake); ~ScopedClockOverride(); };
}
```

Python tests patch module-level `_now`/`_today`. Here one override covers `time`,
`datetime.now` and `date.today`. Where a Python test patches only one module's clock, the
port uses the same global override; the test ledger notes it.

### 3.6 Credentials (`pylib/keyring.h`, `services/credentials.h`)

```cpp
namespace fv::pylib::keyring {                  // keyring package, WinVaultKeyring rules
std::optional<std::string> get_password(std::string_view service, std::string_view username);
void set_password(std::string_view service, std::string_view username, std::string_view secret);
bool delete_password(std::string_view service, std::string_view username);
std::string backend_name();                     // "Windows Credential Manager"
}
namespace fv::services {                        // credentials.py, names kept
class CredentialStore { public: CredentialStoreStatus status(); std::optional<std::string>
    get(std::string_view name); void set(std::string_view name, std::string_view value);
    bool remove(std::string_view name); void refresh(); };
CredentialStore& store();  void set_store(std::unique_ptr<CredentialStore>);   // tests
std::optional<std::string> resolve_credential(std::string_view name);         // store, env, .env
}
```

The target name follows keyring's `<name>@<service>` with the legacy fallback. Reading
OpenWhisper's keys uses service `OpenWhisper` (import only).

### 3.7 HTTP client (`pylib/httpx.h`)

```cpp
namespace fv::pylib::httpx {
struct Timeout { double connect = 5, read = 5, write = 5, pool = 5; };
struct ClientOptions { std::string base_url; Headers headers; Timeout timeout;
    bool follow_redirects = false; bool trust_env = true;
    std::vector<std::string> resolve;           // "host:port:ip" (curl RESOLVE) for pinning
    const py::threading::Event* cancel = nullptr; };
class Response { public: int status_code; Headers headers; std::string url; double elapsed;
    std::string text() const; Json json() const; const std::string& content() const;
    void raise_for_status() const; };
class Stream { public: const Response& response(); std::optional<std::string> iter_bytes();
    std::optional<std::string> iter_lines(); };                 // SSE, Range downloads
class Client { public: explicit Client(ClientOptions options = {});
    Response request(std::string_view method, std::string_view url, const RequestArgs& args = {});
    Response get(std::string_view url, const RequestArgs& args = {});
    Response post(std::string_view url, const RequestArgs& args = {});
    Stream stream(std::string_view method, std::string_view url, const RequestArgs& args = {});
    void close(); };
// Exceptions mirror httpx: HTTPError > TransportError > {ConnectError, ConnectTimeout,
// ReadTimeout, RemoteProtocolError}; HTTPStatusError.
}
```

Calls block on the calling worker thread, as httpx sync calls did. `cancel` aborts through
curl's progress callback. `urllib.request` call sites (`verified_download`, `app_update`)
use the same client plus `py::urllib::getproxies()`, which reads the Windows registry the
way urllib does.

### 3.8 HTTP and WebSocket server (`pylib/fastapi.h`, `pylib/websockets.h`)

```cpp
namespace fv::pylib::fastapi {
struct Request { std::string method, path; Params path_params; Query query; Headers headers;
    std::string body; std::string client_host; Json json() const; };
struct Response { int status = 200; Headers headers; std::string body; };
Response JSONResponse(const Json& content, int status = 200);
Response FileResponse(std::filesystem::path, const Request&);   // Range / 206 / 416
class StreamingResponse;                                        // SSE, chunked
class HTTPException : public std::exception { public: HTTPException(int status, Json detail); };
class WebSocket { public: void accept(); std::optional<std::string> receive_text(double timeout);
    void send_text(std::string_view); void close(int code, std::string_view reason = {}); };
class App { public:
    void get(std::string pattern, std::function<Response(Request&)> handler);   // also post/put/delete
    void websocket(std::string pattern, std::function<void(WebSocket&, Request&)> handler);
    void mount(std::string prefix, std::filesystem::path static_dir); };
class Server { public: Server(App& app, ServerConfig config);   // uvicorn: host, port, sockets
    void start(); void stop(double grace_s); int port() const; };
}
```

- Handlers run on the server's worker threads.
- FastAPI's incidental behaviour lives in the router: `{"detail": ...}` envelopes, the 422
  validation body, and the 404 and 405 bodies. Recorded Python transcripts pin it.
- `pylib/websockets.h` mirrors the sync `websockets` API used by `remote_asr`:
  `serve(handler, host, port, ssl)`, `connect(uri, ssl, max_size)`, `send`, `recv`, `ping`,
  `close`. The TLS context takes a verify callback for certificate pinning.

### 3.9 Database (`pylib/sqlite3.h`, `services/database.h`, `services/models.h`)

`pylib/sqlite3` mirrors Python's `sqlite3` module: `connect(path, uri, timeout)`,
`Connection::execute(sql, params)`, `executemany`, `commit`, `rollback`, `backup(target)`,
`Row` with name access, and `Cursor` iteration. SQLAlchemy queries become their emitted SQL
text inside `services/database.cpp` and the repositories. `services/models.h` holds one row
struct per table, with columns named as in `services/models.py`. `DatabaseManager` keeps the
schema-v18 creation, the v6→v18 migrations, WAL, FTS5 triggers and `BEGIN IMMEDIATE` where
Python uses them. One writer connection per process is guarded by a mutex; readers open
their own connections.

### 3.10 Process launcher (`py/subprocess.h`, `services/agent_process.h`)

```cpp
namespace fv::py::subprocess {
struct PopenOptions { std::optional<std::filesystem::path> cwd; std::optional<Env> env;
    Redirect stdin_ = Redirect::None, stdout_ = Redirect::None, stderr_ = Redirect::None;
    unsigned creationflags = CREATE_NO_WINDOW;   // main.py patched Popen to always add this
    bool start_new_session = false; };
class Popen { public: Popen(std::vector<std::string> args, PopenOptions options = {});
    std::optional<int> poll(); int wait(std::optional<double> timeout = {});
    std::pair<std::string, std::string> communicate(std::string_view input = {},
                                                    std::optional<double> timeout = {});
    void terminate(); void kill(); int pid() const; PipeReader& stdout_pipe(); };
}
```

`services/agent_process` keeps Python's job-object ownership, suspended start, tree kill
and disown-on-clean-exit. QProcess is rejected: the agent and program runners are Qt-free
in Python and run on worker threads that have no event loop.

### 3.11 Audio input and output (`pylib/sounddevice.h`, `pylib/soundcard.h`, `pylib/av.h`)

```cpp
namespace fv::pylib::sounddevice {
struct DeviceInfo { int index; std::string name; int hostapi; int max_input_channels;
    int max_output_channels; double default_samplerate; };
std::vector<DeviceInfo> query_devices();  std::vector<HostApiInfo> query_hostapis();
std::pair<int, int> default_device();
void check_input_settings(int device, int channels, std::string_view dtype, double samplerate,
                          const ExtraSettings* extra = nullptr);
using InputCallback = void (*)(const int16_t* frames, unsigned long count,
                               const StreamTime& time, CallbackFlags status, void* user);
class InputStream { public: InputStream(StreamParams params, InputCallback callback, void* user);
    void start(); void stop(); void abort(); void close(); bool active() const; };
class OutputStream;                               // history playback, sample voice
void _terminate();  void _initialize();           // refresh_portaudio, under portaudio_lock
}
```

- `pylib/soundcard` covers WASAPI loopback (`default_speaker().recorder(...)`).
- `pylib/av` covers `open(path)`, iteration over audio frames, and `AudioResampler(format,
  layout, rate)` on FFmpeg.
- Callbacks run on the audio thread and must not allocate, lock a contended mutex or log
  (section 4).

### 3.12 Events between layers

- **Qt-free code** keeps Python's mechanisms. `on_*` callback attributes become
  `py::Callback<Sig>`, which is read at call time just as Python reads the attribute, and
  listener registrations become `py::Listeners<Args...>`. They are called on whatever
  thread Python called them on.
- **Qt code** keeps `pyqtSignal`s as Qt signals with the same snake_case names. `object`
  payloads become `py::Json` (registered with `Q_DECLARE_METATYPE`) or a registered
  struct; `str` becomes `QString`.
- **The bridge** is `ui_qt/utils/relay.h`:

```cpp
namespace fv::ui_qt {
void call_on_gui(std::function<void()> fn);       // QMetaObject::invokeMethod(qApp, ..., Queued)
template <class Owner> class Relay {              // Python's session-lived relay + token pattern
public:
    explicit Relay(Owner* owner);                 // QPointer inside
    int next_token();
    template <class F> void post(int token, F&& deliver);   // runs on GUI only if the owner is
};                                                          // alive and the token is current
}
```

Every Python relay class (`HistoryDelivery`, the `*Relay` QObjects in a13, a18, a21, a23
and a31) ports onto this one template.

---

## 4. Threading model

| Thread | Runs | Python equivalent |
|---|---|---|
| GUI (main) | QApplication, all widgets, `UIController`, `ApplicationController`, runtime QObject slots, QTimers, clipboard, tray | Qt main thread |
| Engine (`SpeechProcess`) | Every `nemo_speech_asr_*` call: runtime load, model load, warm-up, `transcribe` | the `local_asr/worker.py` child process |
| Executors (`py::ThreadPoolExecutor`, same names and sizes as a03) | `executor` (2: jobs, reloads), `database-startup` (1), `history-save` (1, drained, never cancelled), `component` (1), `walkthrough-library` (1) | ThreadPoolExecutors |
| PortAudio callback threads | Recorder input, history player output, meeting microphone | sounddevice callbacks |
| Recorder watcher, journal writer | Post-roll wait, stream close, PCM journal | same daemon threads |
| Preview worker | `StreamingTranscriber` window loop | same |
| Incremental poll | `IncrementalDictation` early windows | same |
| Hook thread (Windows) | `WH_KEYBOARD_LL` and `WH_MOUSE_LL` message loop; decide and return in microseconds | `keyboard` listener |
| Hotkey dispatcher | `OrderedDispatcher` callbacks into runtimes | same |
| Focus capture | UIA reads, abandonable (detached; owns its COM apartment) | same |
| Meeting capture | Microphone stream, WASAPI loopback thread, spool writers, watchdog, ASR worker, diarizer, heartbeat | `meeting/capture`, `asr`, `diarize` threads |
| Agent | Scheduler worker, process pipe readers, loopback MCP server | `meeting/agent` threads |
| Servers | One Beast `io_context` thread each for the meeting web server, MCP, agent API/live API, plus a handler pool; remote host acceptor plus per-connection threads | uvicorn loops, websockets threads |
| Automation | Event bus, webhook lanes, action pool (4), program runners | same |
| Diagnostics | Metrics writer | same |

**Rules for crossing threads.**

1. Widgets and `QObject`s with a GUI affinity are touched only on the GUI thread. A worker
   reaches them through a queued signal, `call_on_gui`, or a `Relay` token. A worker never
   holds a widget pointer (a08, a13 and a21 record real crashes from this in Python).
2. Qt-free modules keep Python's lock objects one for one: same names, same scopes, same
   order. A `threading.RLock` becomes `std::recursive_mutex`. Where Python takes several
   together (`_acquire_backup`: capture, meeting, then job lock), C++ uses
   `std::scoped_lock` in that order.
3. Only the engine thread calls NeMo. Callers block on a future from a worker thread, never
   from the GUI thread. A debug assertion in `SpeechProcess::request` rejects calls from
   the GUI thread.
4. Audio callbacks write into preallocated single-producer, single-consumer rings and signal
   with atomics. The 512-block overflow behaviour of `recording_journal` is kept.
5. Settings watchers and store subscribers fire on the writing thread, as in Python. Qt
   consumers re-post them, and subscribers only enqueue (a28).
6. A `QTimer` is always started on the GUI thread with a context object
   (`QTimer::singleShot(ms, this, fn)`). Python's "a QTimer started off its thread never
   fires" bug class therefore cannot occur.
7. Shutdown follows a03's exact order. It ends with `py::os::_exit(code)` after flushing
   logs, as `main.py` does. That also skips static destructors that could touch a loaded
   runtime, which is never unloaded.

---

## 5. Speech backends

### 5.1 One interface, three implementations (`transcriber/`)

```cpp
namespace fv::transcriber {
class TranscriptionBackend {                    // transcriber/base.py
public:
    virtual ~TranscriptionBackend();
    virtual std::string recognition_support() const { return "after"; }
    virtual std::string transcribe(const std::filesystem::path& audio_path,
                                   const RecognitionContext* recognition = nullptr) = 0;
    virtual bool is_available() = 0;
    virtual void cancel_transcription();  void reset_cancel_flag();
    virtual void prepare_for_transcription() {}
    virtual std::optional<double> large_file_size_mb(const std::filesystem::path&) { return {}; }
    virtual void cleanup() {}
    virtual std::string name() const;
    py::Callback<void(std::string message, bool transcribing)> progress_callback;
};

class LocalSpeechBackend : public TranscriptionBackend {   // transcriber/optional_backend.py
public:
    LocalSpeechBackend(std::string backend, std::optional<std::string> model_name = {},
                       std::optional<std::string> device = {});
    bool is_model_missing() const;  std::string device_info() const;  int generation() const;
    void reload_model(std::optional<std::string> model_name = {}, const py::threading::Event* cancel = nullptr);
    bool warmup();
    std::string preview_audio(std::span<const float> audio16k, std::optional<std::string> language,
                              std::function<bool()> busy);        // non-blocking, may return ""
    std::vector<std::string> transcribe_windows(const Windows&, std::optional<std::string> language,
                                                std::span<const std::string> phrases = {});
    std::string decode_window(std::span<const float> audio16k, std::optional<std::string> language,
                              std::span<const std::string> phrases = {});
    static std::string join_texts(std::span<const std::string> texts);
};
class OpenAITranscriptionBackend;               // transcriber/openai_backend.py, via httpx
class RemoteSpeechBackend;                      // transcriber/remote_backend.py: subclass of
}                                               // LocalSpeechBackend whose "process" is a
                                                // remote_asr RemoteConnection, as in Python
```

The controller's backend table holds exactly these keys:

- `parakeet`
- `api`
- `remote`
- `nemotron`, only if the owner keeps it; it rides the same runtime.

### 5.2 `SpeechProcess`: the engine thread (`services/local_asr/process.h`)

OpenWhisper's `LocalSpeechBackend` talks to a worker process through
`SpeechProcess.request(op, timeout, cancel, progress, **payload) -> dict`. OpenWhisper_CPP keeps
the class name, the request method and the op vocabulary, but the "process" is a thread in
this process. Departs from: the child process (dropped by the owner) and the temporary
`.f32` files (audio travels as shared spans).

```cpp
namespace fv::services::local_asr {
struct RequestOptions { double timeout = 180.0; const py::threading::Event* cancel = nullptr;
    std::function<void(double done, double total)> progress; Lane lane = Lane::interactive; };
enum class Lane { interactive, meeting, preview };   // preview: refused when not idle
class SpeechProcess {                                // process.py
public:
    static SpeechProcess& shared();                  // one per process, started lazily
    py::Json request(std::string_view op, py::Json payload, const RequestOptions& options = {},
                     std::shared_ptr<const std::vector<float>> audio = {});
    std::optional<py::Json> try_request(std::string_view op, py::Json payload,
                     std::shared_ptr<const std::vector<float>> audio);   // preview path
    std::string recent_errors(int lines = 12) const;
    void close();                                    // joins the thread; runtime stays loaded
};
}
```

- **Ops**, handled on the thread in `local_asr/worker.cpp`, mirroring `worker.py`:
  - `load{runtime, model_path, device}` returns `{device, gpu}`;
  - `transcribe{language, phrases}` takes audio and returns
    `{text, segments:[{text,start,end}]}`;
  - `warmup`, `unload`, `version`.
- **Errors** come back as `{error, code}` with the user sentence from `fv::EngineError`.
  `code: "unsupported_gpu"` keeps driving the Auto-device CPU fallback.
- **Lanes.** Final dictation and upload decodes are `interactive`. Meeting chunk and offline
  re-decodes are `meeting`. Previews use `try_request`, which returns empty when the thread
  is busy, as Python's non-blocking `_decode_lock` acquire did. The queue is FIFO within a
  lane and `interactive` first across lanes, so a stop never waits behind previews.
- **Cancel.** The C ABI has no abort. `LocalSpeechBackend` already decodes long audio in
  windows of at most 30 s (split at the quietest 0.1 s after 24 s, from
  `local_asr/audio.py`), so cancel is checked between windows and before each queued
  request starts. The worst case is one window, about 1 s on CPU for 30 s of audio. Queued
  requests are cancelled at once.

### 5.3 Live preview by window decoding

`services/streaming_transcriber.cpp` keeps `StreamingTranscriber`:
- 3.0 s windows with 0.75 s overlap;
- the worker thread fed from the recorder callback through the ring;
- `append_preview_text` seam joining;
- lazy `finalize_preview`, so a stop never decodes the tail before the final pass.

Each window is `fft_resample`d to 16 kHz by the same code the file path uses, so
`IncrementalDictation` stays bit-identical by construction. Then it goes through
`LocalSpeechBackend::preview_audio`, which uses `try_request` on the preview lane. The
native-stream branch (`NativeStreamingTranscriber`) is ported only if Nemotron stays;
Parakeet TDT rejects streaming.

### 5.4 Sharing one Recognizer

Dictation, uploads and meetings each hold their own `LocalSpeechBackend` object, as Python
did, but they all use `SpeechProcess::shared()`.

- **One model at a time.** The thread holds at most one `fv::Recognizer`, keyed by
  (runtime library, model path, device). A `load` for the same key is a no-op that bumps a
  reference count. A different key destroys the old Recognizer and creates the new one.
- **Contention is already prevented** by the a03 refusal gates: dictation, uploads and
  retranscribe are refused while a meeting runs ("Meeting Mode is active — end the meeting
  to use dictation"). What remains is meeting chunks against meeting preview against the
  post-meeting re-decode, all on the `meeting` lane.
- **The model lease** (`release_local_engine` / `restore_local_engine`) keeps its API and
  its state machine (a03 A03-13, test_engine_warmup traces). Releasing becomes "mark
  released, refuse dictation". It no longer frees weights when meeting and dictation use
  the same model. The device-info line "Released while Meeting Mode runs" is shown only
  when a different meeting model really forced an unload. This is a deliberate behaviour
  change: no reload after every meeting.

### 5.5 Model and runtime lifetime

- **Runtimes.** `NemoApi::get(path)` loads a runtime library once and never frees it.
  Switching CPU to GPU loads the second library alongside the first; Windows DLLs keep
  separate ggml copies. A Device change applies at the next model load, with no restart.
- **Updating a loaded runtime.** A component install or removal that targets the loaded
  runtime is staged as `<id>.pending`. The UI says "Restart OpenWhisper_CPP to finish updating
  the speech runtime." `components` settles pending swaps at startup, before any load, using
  the a14 startup-recovery path that already exists.
- **Models** (GGUF) can be swapped freely: `nemo_speech_asr_destroy` is safe. Warm-up is one
  0.5 s decode after each load, outside the reload lock, as a03 pins.
- **Exit.** The engine thread is joined during shutdown. The process then leaves through
  `_exit`, so nothing unloads the runtime.

---

## 6. Third-party choices

| Need | Choice | Runner-up | Why |
|---|---|---|---|
| Microphone capture and playback | **PortAudio 19.7** (`pylib/sounddevice`) | miniaudio | Same host-API and device names as sounddevice, so imported `audio_input_priority` `{name, hostapi}` keys keep matching. miniaudio renames devices and would need a lossy key migration. Risk: the vcpkg build must expose the same host APIs (MME, DirectSound, WASAPI, WDM-KS); verify in M1. |
| WASAPI loopback (meetings) | **Native WASAPI** `IAudioClient` + `AUDCLNT_STREAMFLAGS_LOOPBACK` through wil (`pylib/soundcard_win.cpp`) | miniaudio loopback device | Stock vcpkg PortAudio has no loopback devices. Python's own fallback (`soundcard`) is exactly this API, about 300 lines. |
| HTTP/WebSocket server and WebSocket client | **Boost.Beast 1.92 + Asio + OpenSSL** behind `pylib/fastapi` and `pylib/websockets` | cpp-httplib + ixwebsocket | One library covers HTTP/1.1 routes, WS upgrade on the same port (the dashboard's `/ws` next to `/m/{token}`), SSE and Range streaming, a TLS server with a self-signed cert, and a WS client with a pinning verify callback. cpp-httplib has no WS; Qt HttpServer is not installed and is GPLv3. |
| HTTP client | **libcurl 8.23** (Schannel on Windows) behind `pylib/httpx` | QtNetwork | Python's callers are synchronous Qt-free worker code. curl keeps them synchronous, uses the OS trust store, and supports `CURLOPT_RESOLVE` for the webhook SSRF guard (pinned address with correct SNI), which QNetworkAccessManager cannot do. |
| JSON | **nlohmann-json 3.12 `ordered_json`** + `py::json::dumps` | QJsonDocument | Insertion order and the int/float distinction are load-bearing (a10, a26, a28). QJsonObject sorts keys and merges int with double. |
| SQLite | **sqlite3 3.54 C API** (`fts5`, `json1`) behind `pylib/sqlite3` | QtSql QSQLITE | Must be Qt-free; needs `sqlite3_backup`, `BEGIN IMMEDIATE` and certain FTS5. |
| Zip | **minizip-ng 4.1** (zlib only) | libzip | Streaming Zip64 read/write with progress and cancel, for multi-GB `.owbackup`s, docx/pptx/xlsx read and write, and NeMo zip extraction. libzip writes at close, which fits cancel poorly. |
| .docx writing | **minizip-ng + pugixml 1.16**, OOXML templates captured from python-docx output | QXmlStreamWriter | The same XML library also reads docx/pptx/xlsx for the Qt-free knowledge-folder search. |
| PDF writing | **QPdfWriter + QTextDocument** (QtGui) | libharu | Python composes walkthrough PDFs with exactly these classes, so pagination quirks carry over. |
| PDF text extraction (knowledge folder) | **Own extractor** (zlib FlateDecode, Tj/TJ, ToUnicode CMaps; about 600 lines) | Poppler or PDFium | None is available or permissively licensed in the constraints. Only search hits depend on it. |
| Markdown | **QTextDocument::setMarkdown** (Qt's md4c), as Python uses | md4c, cmark | Python renders through Qt; another parser would change the output. |
| File decoding (uploads) | **FFmpeg** (avformat, avcodec, swresample; LGPL DLLs) behind `pylib/av` | Media Foundation | PyAV is FFmpeg, so decoders and s16 mono downmix match PyAV (up to version drift). One slow source build, absorbed by the binary cache. |
| Resampling and DSP | **pocketfft** (vcpkg, header-only) | kissfft, pffft | numpy 2.x uses this C++ pocketfft, so `fft_resample` and fbank stay bit-near Python. libsamplerate/speexdsp would change the samples. |
| Diarization inference | **onnxruntime 1.31.1** CPU with the same WeSpeaker ONNX file | none | Python runs the same runtime and model. The cosine ≥ 0.999 gate from a25 applies. |
| Credentials | **WinCred** (`CredReadW`/`CredWriteW`/`CredDeleteW`) behind `pylib/keyring` | qtkeychain | qtkeychain-qt6 depends on Qt (vcpkg would build Qt from source) and loses keyring's target naming and lookup-only reads. |
| Crash reporting | **In-process handler**: `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` + dbghelp stack walk to `openwhisper-cpp.crash.log`, plus terminate, invalid-parameter and SIGABRT hooks | crashpad | Python's `faulthandler` writes a local text log and that is the contract. crashpad needs a handler process and an upload server. |
| Global hotkeys and input injection | **`WH_KEYBOARD_LL` / `WH_MOUSE_LL` + `SendInput`** on the hook thread; key names from a table generated from Python's `keyboard` library | `RegisterHotKey` | Push-and-hold needs key-up events and per-key suppression, which RegisterHotKey lacks. Saved hotkey strings must parse with `keyboard`'s names. |
| Screenshots (Snap) | **`QScreen::grabWindow`** + Win32 (`SetWindowDisplayAffinity`, DWM bounds, foreground) | Windows.Graphics.Capture (cppwinrt) | Same pixels path as Python's `ui_qt/snap/capture.py`. |
| Regex | **PCRE2 10.49** (UTF+UCP) behind `py::re` | std::regex | std::regex lacks lookbehind and Unicode `\w`. |
| Tests | **Catch2 3.16** for all tests, Qt Test library for `QTest::mouseClick`/`QSignalSpy` only | QtTest runner | One runner; pytest function names become TEST_CASE names verbatim (section 9). |

Not used: Qt Network, Qt Sql, spdlog (py logging covers it), fmt (`std::format`), cmark,
md4c, libarchive (Linux tar.gz runtimes later use zlib plus a 200-line tar reader with
the a14 safety filter).

---

## 7. Data and identity

### 7.1 Folder layout (`data_root()`; `OPENWHISPER_CPP_DATA_DIR` overrides)

```
%LOCALAPPDATA%\OpenWhisper_CPP\
  openwhisper_cpp_settings.json        same keys, kinds and JSON layout as openwhisper_settings.json
  openwhisper-cpp.db (-wal, -shm)      schema v18, identical tables, triggers and FTS5
  recorded_audio.wav, .recorded_audio-recovery\
  recordings\  meetings\  captures\  remote_records\
  scratchpad.txt  mcp_event_subscriptions.json  backup-preferences.json
  .openwhisper-cpp-data.lock   .openwhisper-cpp-restore\        (lease and staged restore, a10)
  openwhisper-cpp.log(.1..3)  openwhisper-cpp.crash.log  crash-*.dmp
  diagnostic-failures.json  diagnostic-metrics.json
  components\<id>\  speech-models\<key>\  models\speaker-id\  updates\  tls\
  .env
```

Every file name comes from the identity block (`APP_SLUG`). The names inside are Python's
names with the slug substituted.

### 7.2 Settings format

JSON object, `indent=2`, trailing newline, insertion order kept, the 186 OpenWhisper keys
with the same kinds and fallback rules. Dropped-engine keys are kept on disk untouched
(Python ignores unknown keys too) and are never written by OpenWhisper_CPP. OpenWhisper_CPP-only keys
take the prefix `openwhisper_cpp_`, for example `openwhisper_cpp_runtime_dir`, which replaces today's
QSettings `engine/*` entries, and `openwhisper_cpp_import`. `selected_model` choices shrink to
`parakeet`, `api`, `remote` (plus `nemotron` if kept). Any other stored value reads as the
default `parakeet`, as `ChoiceSetting` already does.

### 7.3 Database schema

OpenWhisper's v18 schema is copied verbatim: `transcription_history`, `dictation_stats`,
`meeting_*` with FTS5 external-content triggers, `record_sync` and `schema_version`.
OpenWhisper_CPP never alters these tables. Any OpenWhisper_CPP-only data goes in `fv_*` tables with
their own `fv_schema_version`. That keeps both import and `.owbackup` restore valid, and
lets OpenWhisper's migration tests (v5…v17 files) replay unchanged.

### 7.4 Import from OpenWhisper (`services/openwhisper_import.{h,cpp}`)

This is a OpenWhisper_CPP-native module, offered by guided setup and by Settings → Backup.

1. Locate the source: `OPENWHISPER_DATA_DIR`, then `%LOCALAPPDATA%\OpenWhisper`, then a
   folder the user picks.
2. Take OpenWhisper's shared data lease (`.openwhisper-data.lock`, `LockFileEx`). Refuse
   with a sentence if OpenWhisper holds it exclusively or is running.
3. **Settings:** parse through `SettingsKey` (unknown keys kept), map dropped
   `selected_model` values to `parakeet`, write `openwhisper_cpp_settings.json`.
4. **Database:** `sqlite3_backup` into `openwhisper-cpp.db`, then rewrite absolute paths that
   point into the OpenWhisper root (`meeting_sessions.spool_dir`, legacy absolute
   `audio_file`).
5. **Files:** copy `recordings/`, `meetings/`, `captures/`, `remote_records/`,
   `scratchpad.txt` and `mcp_event_subscriptions.json`, using hard links when on the same
   volume.
6. **Credentials:** read service `OpenWhisper` entries and write service `OpenWhisper_CPP`,
   renaming `OPENWHISPER_*` names (MCP token, event secrets, remote token) to
   `OPENWHISPER_CPP_*`. Provider keys (`OPENAI_API_KEY`, …) keep their names.
7. Record `openwhisper_cpp_import = {source, at, version}`. Import is one-way and idempotent.

The restore of an `.owbackup` runs steps 3–6 on the staged contents. OpenWhisper_CPP writes
`.fvbackup` with the same manifest schema and accepts both on restore.

### 7.5 Shared with an OpenWhisper install (read-only)

- **Speech models.** The resolver looks in `speech-models\<key>` under OpenWhisper_CPP first,
  then under OpenWhisper. An OpenWhisper model is used in place after the same
  `installed.json` and size check. OpenWhisper_CPP never writes into OpenWhisper's folders.
- **NeMo runtime.** On first use it is copied from OpenWhisper's `components\<id>` into
  OpenWhisper_CPP's: 4.7 MB for CPU, 106 MB for CUDA. The embedded Python files are skipped.
  The copy exists because a loaded DLL cannot be replaced, and sharing would block
  OpenWhisper's own updater.
- **Nothing else is shared** at run time: settings, database, credentials, logs, lock and
  mutex names all differ.

---

## 8. UI architecture

**Theme pipeline** (`ui_qt/utils`, a02, ported first):

- `ui_qt/styles/theme.qss` is copied byte for byte (6,119 lines, 636 object names) and
  embedded with `qt_add_resources` under `:/ui_qt/`, together with the 59 Tabler SVGs,
  icons and `check.svg` at their Python paths. Python's `url(ui_qt/assets/...)` becomes
  `url(:/ui_qt/assets/...)` before token substitution, because `@icon-suffix` sits inside
  the URL.
- `palette.{h,cpp}` holds the 96 tokens for dark and light and `resolve`, with
  unknown-token errors caught and logged inside the event filter. Omarchy comes later.
- `font_scale.{h,cpp}`: `scale_qss_fonts` uses `py::round`, the app font is Segoe UI at
  14 px × scale (90/100/115/130 %), and `WidgetStyleFilter` re-resolves widget-local
  sheets on StyleChange.
- `theme_manager`: `QPalette` plus following the system colour scheme through
  `QStyleHints::setColorScheme` (6.8).
- `restyle`: `set_style_property` and `repolish`.
- The base style is `windows11` (set after QApplication exists), wrapped by
  `SnappyTooltipStyle`.

**Qt floor goes to 6.8 LTS.** `windows11` is the base style OpenWhisper runs on Windows
(6.7+), and the parity references pin it. "Match system" needs `setColorScheme` (6.8).
Distro Qt on Ubuntu 24.04 is 6.4 anyway, so 6.5 bought nothing.

**Mapping PyQt classes to C++.**

- `class X(QWidget)` in `ui_qt/widgets/m.py` becomes `class X : public QWidget` in
  `ui_qt/widgets/m.h`, namespace `fv::ui_qt::widgets`. A module with several classes keeps
  them in one header, as in Python.
- `pyqtSignal(str, object)` becomes a `Q_SIGNAL` with snake_case name and
  `(const QString&, const py::Json&)`. Dataclass payloads are registered structs, and
  `Optional` maps to `std::optional`.
- Python methods keep snake_case; Qt overrides keep Qt's names (`paintEvent`). `@property`
  becomes a const method of the same name.
- `self.on_x = None` becomes `py::Callback<...> on_x;`.
- `QTimer.singleShot(ms, f)` becomes `QTimer::singleShot(ms, this, f)`. Connected lambdas
  always take a context object.
- Python dynamic attributes on widgets become members. Ownership follows Qt parenting
  exactly as the Python constructs it.

**objectName and dynamic-property discipline.**

- Every `setObjectName` and property literal is copied from Python.
  `set_style_property(w, "tone", "neutral")` is the only way a QSS-keyed property changes,
  and it repolishes.
- `tools/lint/check_qss_names.py` checks that every `#name` and `[prop=...]` in `theme.qss`
  is either referenced by a literal in `src/ui_qt` or the Qt module list, or appears on an
  allowlist of names not yet ported, which shrinks with each milestone.
- The widget-tree dump in tests (section 9) compares object names and properties against
  Python's dumps.

**Wiring controllers and widgets.**

- `UIController` and `ApplicationController` are ported as written. `_setup_ui_callbacks`
  assigns `ui_controller->on_record_start = ...` in the same order (about 75 entries), and
  `_setup_connections` connects the same signals.
- Python's suggested split of `UIController` into helpers is not adopted. The file stays
  one class, as large as Python's (2,930 lines), so a03's tables can be checked line by
  line.
- Runtimes keep receiving the controller. Python's reaches into privates (`_job_lock`,
  `_capture_lock`) become `friend` declarations listed in the header. Departs: C++ access
  control.
- A wiring test walks the `QMetaObject` of `MainWindow`, `SystemTrayManager` and
  `WaveformOverlay` and asserts `isSignalConnected` for every signal a03 lists.

**Where strings live.**

- Strings stay inline, next to the code, exactly where Python has them: UTF-8 source
  (`/utf-8`), `u8"…"` in Qt-free code, `QStringLiteral(u"…")` in Qt code, typographic
  characters (… — → · ✓) copied, never retyped.
- There is no `tr()`, because OpenWhisper is not localized.
- A Python string containing "OpenWhisper" becomes a `std::format` or `percent_format`
  with `config::APP_NAME`. `tools/lint/check_brand.py` fails on any "OpenWhisper" literal
  outside the allowlist (remote protocol constants, import module, tests).
- The webui bundle is rebranded in source (title, missing-bundle notice,
  `openwhisper.meeting.*` storage keys become `openwhisper-cpp.meeting.*`) and rebuilt with Node
  at development time. `dist/` is committed, as in OpenWhisper.

---

## 9. Testing and parity gates

**Layout.**

```
tests/
  CMakeLists.txt         globs; written once in M0
  support/               fv_test_support: catch_main.cpp (unit), qt_main.cpp (QApplication,
                         offscreen pins, theme pipeline), isolation.cpp (<- conftest.py:
                         temp OPENWHISPER_CPP_DATA_DIR per test, in-memory keyring, null focus
                         capture, no network discovery, no agent probes), fakes/ (<- tests/
                         fakes/*.py: InMemorySettings, http transport, remote host,
                         meeting_web, fake agent CLI), helpers.cpp (<- helpers.py),
                         golden.h (replay driver), reference.h (tree + pixel compare)
  engine/                existing fv_tests (renamed folder)
  unit/test_<name>.cpp   ports of pytest files whose subject is in fv_core
  qt/test_<name>.cpp     ports of pytest files that need QApplication
  golden/<python.module>.json        committed vectors, source commit inside
  reference/<widget>-<theme>-<style>-dpr<n>-s<scale>.{png,json}, app-<theme>-s<scale>.qss,
            palette-<theme>.json
  ledger/<package-id>.json           which Python tests this package ported, and how
```

**One to one.**

- `tests/test_title_bar.py::test_menu_items_fit_at_every_font_scale` becomes
  `TEST_CASE("test_menu_items_fit_at_every_font_scale", "[test_title_bar]")` in
  `tests/qt/test_title_bar.cpp`.
- Parametrized tests use `GENERATE` with the same parameter tables.
- Monkeypatch targets become the seams listed in section 3: clock, settings store,
  credential store, httpx transport (`ClientOptions::transport` for tests), sounddevice
  backend, subprocess launcher, focus service.

**Golden vectors.**

- `tools/parity/golden_vectors.py` takes curated cases from `tools/parity/cases/<module>.py`
  (one file per module, so packages never collide). `trace_plugin.py` adds the inputs
  OpenWhisper's own tests use.
- Output goes to `tests/golden/`.
- `golden.h` gives a typed replay:

```cpp
for (const auto& c : fv::test::golden("services.format_utils", "format_audio_duration"))
    CHECK(format_utils::format_audio_duration(c.arg<double>(0)) == c.result<std::string>());
```

- Exceptions are recorded as `raises`. Numeric DSP stores arrays with a stated tolerance:
  absolute 1e-6 for float32 pipelines, 1 LSB for FFmpeg-decoded PCM.

**Reference screenshots.**

- `tools/parity/render_reference.py` gets one builder file per area in
  `tools/parity/builders/aNN.py`, each with fixed fake data, pinned clock and seeds.
- Pins: offscreen, `QT_QPA_FONTDIR=%WINDIR%\Fonts`, `QT_SCALE_FACTOR=1` (and 2 for the
  HiDPI set), `QT_FONT_DPI=96`, `windows11` base style, focus and default button pinned,
  animations driven to their end.
- In C++, `fv::test::expect_reference(widget, "meeting_delete_dialog", {...})` first diffs
  the widget tree against the JSON (class base, objectName, properties, geometry, text,
  visibility; readable failures), then the pixels.
- The default tolerance is 0 differing pixels. An exception needs
  `reference/<name>.tolerance.json` with a reason.
- Python references come from PyQt 6.11.0. The build machine should move to Qt 6.11.x;
  until then 6.10.3 is accepted because the a33 trial matched with 0 differing pixels.
  Every sidecar records the Qt version.

**Harness home.**

- `tools/parity/` takes over `out/parity-scratch/`'s scripts: `ow_isolation.py`,
  `classify_tests.py`, `coverage_table.py`, `golden_vectors.py`, `trace_plugin.py`,
  `render_reference.py`, `gen_unicode_tables.py`, `gen_keyboard_names.py`,
  `record_http_transcripts.py`, plus `reference.json`, which pins the OpenWhisper commit
  and Python path.
- `classification.json` is committed there.
- `tools/parity/report.py` merges `tests/ledger/*.json` into `docs/port/parity-report.md`:
  for each of the 548 Python test files, ported / golden / not applicable (dropped engine)
  / pending.
- Scripts never write into the OpenWhisper checkout, following a33's isolation recipe.

**Gate a work package must pass before merge.**

1. Fresh worktree: `cmake --workflow --preset windows-debug` succeeds with warnings as
   errors on `fv_py`, `fv_core` and `fv_qt`. The Linux job builds `fv_engine` and `fv_py`.
2. `ctest` is fully green, including every earlier package's tests. Decode tests run with
   the `OPENWHISPER_CPP_TEST_*` variables when `src/engine` or `services/local_asr` changed.
3. Every Python test file the map assigns to the package's modules appears in its ledger.
   Each test function is either ported (same name) or marked golden / not applicable /
   pending with a reason. No more than 10% may be pending, and pending tests stay listed
   for the follow-up package.
4. Every golden candidate the map lists for those modules has vectors committed and
   replayed.
5. Every widget, dialog or page added has a reference render (dark at 100 %, plus light
   and 130 % for top-level windows and Settings pages) passing at 0 differing pixels.
6. Lints pass:
   - clang-format;
   - `qt_includes.py` (no Qt in `fv_core`/`fv_py`; no `fv_qt` header from `fv_core`);
   - `check_qss_names.py`;
   - `check_brand.py`;
   - `check_ownership.py` (files touched lie inside the package's owned folders, plus its
     own ledger, golden, reference, cases and builder files).
7. Every `// departs:` comment has a matching ledger entry.

---

## 10. Build

**CMake.** M0 writes `src/CMakeLists.txt` and `tests/CMakeLists.txt` once. After that
nobody edits them.

```cmake
# src/CMakeLists.txt (M0)
include(OpenWhisper_CPPModules)        # fv_glob_module(), platform filters, Qt module list
fv_glob_module(fv_py    ROOT py)
fv_glob_module(fv_pylib ROOT pylib)
fv_glob_module(fv_core  ROOT . DIRS services transcriber meeting FILES config.cpp
                        EXCLUDE ${FV_QT_MODULES})
fv_glob_module(fv_qt    ROOT . DIRS ui_qt INCLUDE ${FV_QT_MODULES} QT)
```

- `fv_glob_module` uses `file(GLOB_RECURSE … CONFIGURE_DEPENDS *.h *.cpp)` and drops files
  by platform suffix: `_win` only on WIN32, `_mac.mm` only on APPLE, `_linux`/`_x11` only
  on Linux.
- It applies `openwhisper_cpp_target_defaults`, plus `/WX` for these targets and
  `QT_NO_KEYWORDS` on `fv_qt` (signals are `Q_SIGNAL`, because Python names like `emit`
  and `signals` would collide).
- Adding a module means adding files. Ninja re-globs at build time.
- Qt resources glob `ui_qt/styles/*` and `ui_qt/assets/**`.
- `webui/dist` is copied at install, and at build into `bin/webui`.

**Build-time controls.**

- Precompiled headers per target:
  - `fv_py`: `<string> <vector> <map> <mutex> <format> <filesystem>`;
  - `fv_core`: those plus `nlohmann/json.hpp` and `py/*.h`;
  - `fv_qt`: `<QtWidgets>` subset plus the `fv_core` set.
- Beast and asio are included only in `pylib/*_impl.cpp`, behind pimpl, so they never
  reach the PCH or other files.
- No unity builds: file-static helpers mirrored from Python collide.
- `sccache` is used as `CMAKE_CXX_COMPILER_LAUNCHER` when it is on `PATH` (optional).
- Two test executables (unit, qt) instead of one per file, which avoids hundreds of link
  steps and windeployqt runs.

**vcpkg.**

- M0 adds every dependency at once, so `vcpkg.json` is never edited again: nlohmann-json,
  pcre2, pocketfft, zlib, openssl, curl, boost-beast, sqlite3[fts5,json1],
  minizip-ng[zlib], pugixml, portaudio, ffmpeg[avcodec,avformat,swresample], wil, and
  onnxruntime under a manifest feature `meeting` (on by default from M6; the presets
  already pass it, so turning it on is a preset-only change owned by M6-01).
- The lead pre-warms a shared binary cache before wave 1
  (`VCPKG_DEFAULT_BINARY_CACHE=D:\vcpkg-cache`, set by `scripts/setup-windows.ps1`). The
  first FFmpeg and onnxruntime builds are long; every later worktree restores in seconds.
- Optional `OPENWHISPER_CPP_VCPKG_INSTALLED` points `VCPKG_INSTALLED_DIR` at one shared tree,
  which is safe because the manifest is frozen.

**Presets.** These are added to the existing ones:

- `windows-ci` (Release with debug info, `/WX`, lints as tests);
- `windows-parity` (test preset running only `[reference]` and `[golden]` with the pins);
- `linux-engine` (`fv_engine` and `fv_py` only).

CTest sets the offscreen pins and `OPENWHISPER_CPP_DATA_DIR` per test.

**Files frozen after M0** (edits need a lead-owned follow-up, not a worker):
- `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/*`;
- `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/support/*`;
- `src/py` interfaces (implementations may grow);
- `config.h`'s identity block.

---

## 11. Milestones

Every milestone ends in an app a person can run and demonstrate.

| M | Name | Areas | Demo |
|---|---|---|---|
| M0 | Foundation | build, py/pylib, a01 config/logging/diagnostics, a10 settings and credentials core, a02 theme pipeline, engine thread | The existing transcribe-a-file window, themed with `theme.qss` and settings in JSON, decodes through `SpeechProcess` |
| M1 | Dictation | a01 main window, tray, splash, single instance; a02 core widgets; a03 skeleton; a04 transcription, hotkeys, streaming; a05 recorder chain; a06 Windows hotkeys, paste, focus; a07 overlay (Bars, Classic); a08 Quick Record and compact; a09 store; a14 Parakeet backend and shared-component lookup | Press `kp *`, speak, see the overlay and preview, text is pasted and saved to history |
| M2 | Settings, models, personalize | a10 backup and restore plus OpenWhisper import; a11 Settings shell and pages; a12 text LLM, cleanup, dictionary, snippets, transforms, Command Mode; a13; a14 downloads and components; a15; a16 guided setup | Import an OpenWhisper install, download a model, dictate with AI cleanup and a snippet |
| M3 | Uploads, history, stats | a08 Upload, batch, scratchpad, viewer; a09 sidebar, calendar, export, stats; a04 upload/batch/retranscribe; cloud API backend (a14 openai_backend) | Batch-transcribe files locally or through OpenAI/Groq; browse the calendar; export; Stats |
| M4 | Walkthroughs | a22 (storage, sections, documents, Snap, live drawing), a23, a04 walkthrough runtime, a20 installed_agents and agent_process, a26 installed drivers (write-up) | Record a walkthrough with screenshots, review, export PDF/DOCX, AI write-up through Claude Code |
| M5 | Meetings | a24, a25 (Windows mic + loopback, ASR, diarization), a28, a29 without the LLM parts, a30 server + webui, a31, a04 meeting runtime | Run a meeting with live transcript in the browser dashboard; finalize with speakers; past meetings |
| M6 | Meeting intelligence | a26 scheduler, prompts, MCP loopback; a27 native agent loop; a29 reports, review, live signals, voice commands; a12 typesafe/judge | Live insights and a report from an OpenAI-compatible or installed agent |
| M7 | Automation | a19, a20 (history API, MCP server, events), a21, `--api`, `--channel` | Saved actions by hotkey and voice; webhooks; Claude Code reads history over MCP |
| M8 | Remote engine | a17, a18, Host Mode in a01, remote backend | Pair with an OpenWhisper or OpenWhisper_CPP host and dictate through it |
| M9 | Updates and release | a16 updater (setup-exe phase 1, then the native helper), a34 Inno installer, signing, CI, self-test, parity sign-off | Install, update from a previous build, uninstall |

### M0 work packages (waves; packages in one wave are independent)

| ID | Name | Owned directories | Depends on | Est. lines |
|---|---|---|---|---|
| M0-01 | build-skeleton | root `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/support/`, `tools/parity/` (moved and generalized), `tools/lint/`, `scripts/setup-windows.ps1` cache vars; migrates `tests/*.cpp` to `tests/engine/` | — | 2,500 (C++ 1,500; CMake and Python 1,000) |
| M0-02 | py-text | `src/py/{str,unicode_data,re,difflib,format,int_}*` | M0-01 | 2,600 |
| M0-03 | py-runtime | `src/py/{threading,concurrent_futures,queue,time,datetime,json,logging,os,tempfile,hashlib,secrets,base64,uuid,subprocess,callbacks}*` | M0-01 | 3,000 |
| M0-04 | config-identity | `src/config.*`, `src/version.h.in`, `src/main.cpp` (`--help`, `--version`), `services/{desktop_session,process_env}` | M0-01 | 1,200 |
| M0-05 | theme-pipeline | `src/ui_qt/styles/`, `src/ui_qt/assets/`, `ui_qt/utils/{palette,font_scale,restyle,theme_manager,tooltip_filter,app_icon,icons,relay}` | M0-01 | 2,200 |
| M0-06 | speech-process | `services/local_asr/{process,worker}` | M0-01 | 1,000 |
| M0-07 | pylib-io | `src/pylib/{sqlite3,keyring(_win),zipfile,httpx,urllib}` | M0-03 | 2,800 |
| M0-08 | diagnostics | `services/diagnostics(_win)`, logging setup and crash handler, `ui_qt/startup_profiler` | M0-03, M0-04 | 1,300 |
| M0-09 | settings-core | `services/{setting_types,settings}` (186 keys, enums, resolvers) | M0-02, M0-03, M0-04 | 3,000 |
| M0-10 | credentials | `services/credentials` (store, `.env`, resolution, mask and validate) | M0-07, M0-09 | 800 |
| M0-11 | shell-bridge | `ui_qt/app` (QtApplication), temporary `ui_qt/dev_window` hosting today's window on `SpeechProcess` + settings; removes `src/app`, `tests/app` | M0-05, M0-06, M0-09 | 1,500 |

Waves: [M0-01] → [02, 03, 04, 05, 06] → [07, 08, 09] → [10, 11].

### M1 work packages

| ID | Name | Owned directories | Depends on | Est. lines |
|---|---|---|---|---|
| M1-01 | widgets-core | `ui_qt/widgets/{buttons,button_row,cards,collapsible_header,eliding_label,wrapped_label,empty_state,settings_switch,setting_tile,segmented_bar,no_wheel,context_menu,decode_label,animated_progress_bar,tabbed_content,scrolling_body,reflow,flow_layout,studio,feature_badge,field_help}`, `ui_qt/utils/{collapse_animation,drag_move,fuzzy_match,list_reconcile,file_reveal,desktop}` | M0 | 4,000 |
| M1-02 | audio-capture | `src/pylib/{sounddevice,soundcard}`, `services/{audio_devices,recorder,recording_journal,wav_metadata,audio_player}` | M0 | 3,800 |
| M1-03 | hotkeys-win | `services/{hotkey_manager,_hotkey_common,_hotkey_keyboard,_mouse_hook_win,synthetic_keys,hotkey_conflicts}` + generated key-name table | M0 | 3,500 |
| M1-04 | focus-context | `services/focus_context/*` (catalog, `_capture`, `_win`, `_win_uia`), `services/app_styles` | M0 | 2,500 |
| M1-05 | history-store | `services/{database,models,history_manager,format_utils,titles}` | M0 | 3,000 |
| M1-06 | local-asr-backend | `services/local_asr/{catalog,cache,audio,languages,nvidia}`, `services/{components (read/describe/settle),component_catalog,gpu_info,model_catalog}`, `transcriber/{base,optional_backend}`, shared OpenWhisper lookup | M0 | 3,500 |
| M1-07 | overlay | `ui_qt/overlays/*` (shell, state machine, moments, live preview layout), `ui_qt/waveform_styles/{bars,classic}`, `ui_qt/utils/{loudness,overlay_position}` | M0 | 3,500 |
| M1-08 | main-window | `ui_qt/{main_window,loading_screen,system_tray,whats_new}`, `services/ui_instance(_win)` | M1-01 | 4,000 |
| M1-09 | dictation-pipeline | `services/{streaming_transcriber,incremental_dictation,dictation_language,dictation_pipeline,recognition_context}` (cleanup, snippets and dictionary stages as pass-through stubs until M2) | M1-02, M1-04, M1-06 | 2,800 |
| M1-10 | quick-record | `ui_qt/widgets/{transcription_tab_base,quick_record_tab,compact_record_controller,transcript_pane}`, `ui_qt/clipboard` | M1-01 | 3,000 |
| M1-11 | controllers-runtime | `services/application_controller`, `services/runtime/{transcription (dictation path),hotkeys,streaming}`, `ui_qt/{ui_controller,overlay_state,bootstrap}`, `main.cpp` GUI path | M1-02…M1-10 | 4,500 |

Waves: [01, 02, 03, 04, 05, 06, 07] → [08, 09, 10] → [11].

### Later milestones

M2–M9 cut the same way: 3–4 waves of 4–8 packages, about 200 k lines in total, which
matches the sum of the map estimates (about 205 k).

---

## 12. Risks, and what the native app should do better

**Risks.**

1. **Python-semantics drift** (Unicode `\w`, casefold, code-point offsets, float repr,
   `fromisoformat`). Mitigation: `fv::py` built from Python's own Unicode tables and pinned
   by vectors traced from OpenWhisper's tests. One known gap: PCRE2's `\w` and Python's
   `str.isalnum` disagree on a few categories, and the vectors will show where.
2. **The engine has no decode abort.** Cancel waits for at most one 30 s window. Long
   meeting re-decodes and file uploads see up to about 1 s of cancel latency on CPU,
   longer on very slow machines.
3. **Shared Recognizer.** Meeting chunk decoding plus previews on one thread could fall
   behind on CPU-only machines where Python had two processes. Mitigation: the preview lane
   is drop-only, and the ASR backlog refill from SQLite (a25) already handles lag.
4. **In-process crashes.** A native fault in NeMo now takes down the app, which the worker
   process isolated. Mitigation: the crash log and dump, recording journal recovery, and
   meeting recovery all exist. An optional out-of-process engine host can later reuse the
   `SpeechProcess` protocol unchanged.
5. **PortAudio host-API set.** If the vcpkg build lacks a host API sounddevice exposed,
   imported microphone rankings for that API stop matching. Mitigation: verify in M1-02,
   and fall back to the match ladder's name-only rung.
6. **Boost.Beast verbosity and FastAPI quirks.** The dashboard and MCP depend on incidental
   422, 404, 405 and Range behaviour. Mitigation: `record_http_transcripts.py` captures
   Python's responses, and the C++ server replays them as tests.
7. **Verbatim snake_case inside Qt classes** reads unusually and contradicts CLAUDE.md's
   camelCase rule for `src/app`. The owner must amend CLAUDE.md in M0.
8. **Screenshot gate brittleness.** Fonts, Qt minor version and base style must be pinned,
   and Segoe UI limits the gate to Windows.
9. **Scale and merge discipline.** About 70 packages. Mitigation: frozen shared files,
   ownership lint, and per-package ledgers.
10. **The FFmpeg build** is large and LGPL. Mitigation: shared DLLs, codec features
    trimmed, and the binary cache pre-warmed.

**What the native app should do better** (each is a ledgered, deliberate difference):

- **Startup:** no lazy imports, splash only for runtime and model load. Target: main window
  in under 0.5 s on the build machine, against Python's `startup_to_window_s`.
- **Stop to paste:** no worker IPC or temp `.f32` files; the final pass starts at once on
  the engine thread.
- **Capture:** a lock-free path, so `input_overflows` and `dropped_frames` metrics should
  read 0 in normal use.
- **No reload after meetings:** the shared model replaces the release/reload lease when the
  models match.
- **Single instance on Windows too:** named mutex plus pipe in `ui_instance_win`. A second
  launch raises the first window, which Python did only on Linux.
- **Memory:** settings snapshots are shared `const` JSON instead of pickle copies, and
  history and stats run as SQL without ORM object loading.
- **Hook latency:** the low-level hook callbacks finish in microseconds, so Windows never
  silently removes the hook. The watchdog stays as insurance.
- **Wiring coverage test:** a forgotten connection fails CI instead of silently disabling a
  control.
- **History search:** Unicode-aware case-insensitive search through a registered SQLite
  function. This is an opt-in difference, flagged in the ledger, because Python's
  ASCII-only `LIKE` is the default to match.

**Where a different owner answer changes this design.**

- **Identity.** If OpenWhisper_CPP shared OpenWhisper's data folder and names, only the identity
  block changes, and `openwhisper_import` becomes a no-op. Runtime sharing would then need
  a cross-process lock.
- **Remote wire compatibility.** If it were dropped, only `REMOTE_ENGINE_PATH` and the
  protocol constants change.
- **Native agent loop.** If the Node/Bun sidecars were kept, `meeting/agent/native` becomes
  an NDJSON supervisor. Node and Bun join the component catalog and CI, and M6 grows by the
  packaging work.
- **Server library.** If the owner accepted a Qt module like HttpServer (not installed
  today), `pylib/fastapi` could sit on it, but the Qt-free rule for `meeting/web` would
  break.
- **File decoding.** If the owner preferred Media Foundation to FFmpeg, `pylib/av` swaps
  implementation and upload golden vectors widen their tolerance.
