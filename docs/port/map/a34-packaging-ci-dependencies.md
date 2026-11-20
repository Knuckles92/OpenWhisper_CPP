# Packaging, installers, release CI and third-party dependencies

Area a34. Source checkout (read-only): `D:\coding\whisper_local`. Paths below are relative to it unless absolute.

Files read in full: `pyproject.toml`, `requirements.txt`, `requirements-gpu.txt`, `requirements-build.txt`,
`requirements-release-constraints.txt`, `OpenWhisper.spec`, `OpenWhisperUpdater.spec`, `installer/OpenWhisper.iss`,
`installer/linux/{PKGINFO.in,control.in,lintian-overrides,openwhisper,openwhisper.desktop}`,
`installer/macos/OpenWhisper.entitlements`, `install.cmd`, `install.sh`, `uninstall.cmd`, `uninstall.sh`,
`scripts/{install.ps1,uninstall.ps1,build_installer.ps1,build_installer.sh,build_installer_macos.sh,build_component.py,build_windows_runtime.py,pack_update_archive.py,updater_helper.py,windows_signing.py,openwhisper,openwhisper.cmd,ow,ow.cmd}`,
`.github/workflows/{ci.yml,build-installers.yml,qualify-release.yml}`, `docs/{windows-signing.md,host-updates.md,beta-releases.md,release-health.md}`,
`THIRD_PARTY_NOTICES.md`.

Read to state contracts (not in the list above): `scripts/{build_opencode_component.py,build_macos_runtime.py (head),smoke_windows_setup.ps1,smoke_arch_package.sh (head),check_release_qualification.py (head),serve_update_feed.py (head),generate_icon.py (head)}`,
`services/{update_contract.py,setup_update.py,update_data.py,opencode_catalog.py}` in full, `services/app_update_apply.py` and
`services/app_update.py` (structure plus the prepare, commit, rollback, release-parsing and fallback paths),
`services/components.py` (catalog constants, install, extract, validate, swap), `services/local_asr/nvidia_*_runtime.json`,
`config.py` (data paths), and import greps over `services`, `ui_qt`, `meeting`, `transcriber`, `main.py`, `config.py`.
The app-side update UI and state machine is area a16 and the component/model catalog is area a14; this file covers how things
are built, packed, signed, shipped and verified, and what each library is for.

Marks: **port** (needed in a C++ app), **drop** (engine-specific or Python plumbing), **undecided** (depends on the
Nemotron / cloud-API / remote-engine / meeting-sidecar decisions).

## What the user gets (shipping surface)

- **Windows**: `OpenWhisper-Setup-<ver>.exe` (Inno Setup, per-user, no UAC), plus an optional `OpenWhisper-<ver>-win64.tar.xz`
  that the in-app updater uses. Start menu entry, optional desktop and sign-in shortcuts, "Launch OpenWhisper" at the end.
- **Linux x86-64**: `OpenWhisper-<ver>-linux-amd64.deb` and `OpenWhisper-<ver>-linux-x86_64.pkg.tar.zst`. Commands `openwhisper` and
  `ow`, a desktop entry, an icon. No in-app update (package manager owns the files).
- **macOS Apple Silicon, macOS 14+**: `OpenWhisper-<ver>-macos-arm64.dmg` with a drag-to-Applications link. Ad-hoc signed by
  default; Developer ID signed and notarized when secrets exist.
- **Source checkouts**: `install.cmd` / `install.sh` put `ow` and `openwhisper` on PATH (developer convenience, not a product install).
- **On-demand components** (Downloads page): speech runtimes, GPU libraries, meeting agent runtimes, speaker model, speech models.
- **Releases**: GitHub Releases is the only update server. Stable and opt-in beta (prerelease) tracks, SHA-256 verified.

## Feature inventory

| ID | Feature | Exact behavior | Source | Status |
|---|---|---|---|---|
| A34-01 | PyInstaller onedir freeze | `OpenWhisper.spec`: onedir, windowed, `uac_admin=False` (asInvoker), no UPX, Windows `VSVERSIONINFO` (CompanyName Fiori Labs, version from `_version.py`), icon `.ico`. Data lives under `<root>/ui_qt/...` and is found by `config.bundle_root()`. Hidden imports and `collect_all` lists for plugin-style packages. | `OpenWhisper.spec` | drop (replaced by CMake install + windeployqt); the asset list and exclusions inform the C++ layout |
| A34-02 | Qt trimming | Excludes QML, Quick, WebEngine, Multimedia, Pdf, Designer, Bluetooth, translations; drops Qt's FFmpeg DLLs (only under Qt paths, because PyAV's `av.libs` has same-named DLLs). Keeps `opengl32sw.dll` on purpose as insurance for machines with broken GPU drivers. Copies ICU (`icuuc.dll` etc.) next to Qt6Core because PyQt6 6.11 wheels omit it. | spec `QT_EXCLUDE_PATTERNS`, `_strip_qt`, `_qt_icu_binaries` | port the intent: windeployqt only deploys what the exe links; keep an explicit smoke check for ICU and platform plugin |
| A34-03 | Bundled read-only assets | `services/local_asr` json, `whisper_models.json`, `ui_qt/styles/theme.qss`, `ui_qt/assets` (icons, tabler SVGs, `check.svg`, spin chevrons), `integrations/omarchy`, `webui/dist`, `docs/linux-system-audio.md`, `THIRD_PARTY_NOTICES.md`, PyQt6 and Qt license texts under `third_party_licenses/`. | spec `datas` | port (as Qt resources and installed data; see section 6) |
| A34-04 | Per-platform keyring backend freeze | Explicit `keyring.backends.Windows` / `macOS` / `SecretService`. | spec | drop (QtKeychain or OS API) |
| A34-05 | macOS bundle | `OpenWhisper.app`, bundle id `tech.fiorilabs.openwhisper`, min macOS 14.0, `NSMicrophoneUsageDescription`, `NSAudioCaptureUsageDescription`, no screen-capture key (guarded by the build script), arm64 only. | spec, `build_installer_macos.sh` | port |
| A34-06 | Updater helper freeze | One-file, windowless `OpenWhisperUpdater.exe`, stdlib-only imports, version resource. Copied into the app dir and re-staged into the transaction dir before each update. | `OpenWhisperUpdater.spec`, `updater_helper.py` | port (as a small native exe) |
| A34-07 | Windows build script | Version from `_version.py`, icon generation, freeze helper, freeze app, copy helper in, verify bundle (forbidden `torch nvidia scipy sympy networkx`, required assets, PyAV FFmpeg libs present), run `--self-test` and the release-health gate, sign, pack update archive, run ISCC, verify signatures, print sizes and SHA-256. `-SkipInstaller`, `-Clean`. | `build_installer.ps1` | port (PowerShell stays) |
| A34-08 | Inno Setup installer | See section 2. | `installer/OpenWhisper.iss` | port (new AppId) |
| A34-09 | Linux package builder | Freeze under xvfb, strip build-host rpaths with patchelf, verify bundle, `ldd` every ELF, compute minimum glibc from `readelf`, stage `/usr` tree, build `.deb` (xz -9, lintian `--fail-on error`) and Arch package (`.PKGINFO`, `.MTREE`, zstd -19) from the same stage; reproducible via `SOURCE_DATE_EPOCH`. | `build_installer.sh` + `installer/linux/*` | port (shell stays, simpler) |
| A34-10 | macOS DMG builder | Freeze, verify Info.plist, assets, forbidden packages, run `--version` and `--self-test` and the health gate, `codesign --verify`, audit every Mach-O (arm64 slice, `minos` <= 14.0, no absolute host library paths), optional notarize and staple (app, then DMG), UDZO zlib-9 DMG with retry on "Resource busy", mount the DMG, copy the app out and re-run the checks. | `build_installer_macos.sh` | port |
| A34-11 | Windows code signing | Optional, env-configured, three methods (store thumbprint, PFX, Azure Trusted Signing). See section 2. | `windows_signing.py`, `docs/windows-signing.md` | port (script reusable as is) |
| A34-12 | Update archive | `OpenWhisper-<ver>-win64.tar.xz` with embedded per-file SHA-256 manifest. See section 4. | `pack_update_archive.py`, `app_update_apply.py` | port |
| A34-13 | Updater helper flow | Directory-swap transaction with journal, health check, rollback and crash recovery. See section 4. | `updater_helper.py`, `app_update_apply.py`, `setup_update.py`, `update_data.py` | port (phased, see section 6) |
| A34-14 | Component builders | `gpu-accel`, `meeting-agent`, `meeting-agent-opencode`, runtime wheel pinning. See section 3. | `build_component.py`, `build_opencode_component.py`, `build_windows_runtime.py`, `build_macos_runtime.py`, `build_mlx_runtime.py` | split: NeMo pins port; the rest drop or undecided |
| A34-15 | CI | `ci.yml` (7 jobs), `build-installers.yml` (source, qualification, 3 platform builds, bundle), `qualify-release.yml`. See section 5. | workflows | port (re-shaped) |
| A34-16 | Release qualification | `check_release_qualification.py`: SHA256SUMS, `BUILD-METADATA.json` schema 3, exact artifact inventory, draft-asset validation. `check_release_health.py` timing gate. | scripts | port (qualification script reusable) |
| A34-17 | Beta track and update soak | Prerelease flag is the only difference; `serve_update_feed.py` serves a loopback feed; env `OPENWHISPER_UPDATE_FEED_URL`. | `docs/beta-releases.md`, `app_update.py` | port |
| A34-18 | Developer PATH installers | `install.cmd`/`install.ps1` add `scripts\` to `HKCU\Environment\Path` via the .NET API (not `setx`, which truncates at 1024 chars); `install.sh` writes a marked block into `~/.bashrc`, `~/.zprofile`, macOS `~/.bash_profile`, fish `config.fish`; idempotent; uninstallers remove the block or PATH entry; Linux runs `check_linux_dependencies()` first. | `install.*`, `uninstall.*`, `scripts/install.ps1`, `uninstall.ps1` | drop (no venv); the `ow` alias is an open question |
| A34-19 | Source launchers | `scripts/openwhisper(.cmd)` and `ow(.cmd)`: find `.venv`/`venv`, `cd` to the repo, Linux adds NVIDIA wheel `lib` dirs to `LD_LIBRARY_PATH`, runs CLI modes (`-h --help --version --diagnostics --api --channel --self-test --workflow-smoke`) in the foreground, otherwise starts in the background with `nohup` and logs to `openwhisper.launch.log`; macOS uses the venv's `Python.app` so TCC attributes permissions to a named app. | `scripts/openwhisper*`, `ow*` | drop (Python venv plumbing) |
| A34-20 | Third-party notices | Hand-written `THIRD_PARTY_NOTICES.md`: PyQt6 GPL-3, Qt LGPL-3, Tabler (MIT), React (MIT), DM Sans and Instrument Serif (OFL), runtime and model license tables, on-demand components table. | `THIRD_PARTY_NOTICES.md` | port (generate from vcpkg `copyright` files) |
| A34-21 | Shared speech host updates | `org.openwhisper.Control.PrepareUpdate`, `CancelUpdate`, `UpdateStatus` D-Bus calls let an unattended updater reserve a shared remote-engine host. | `docs/host-updates.md` | undecided (remote engine) |

## 1. Dependency table

Evidence column names the files that import the package (confirmed by `grep` of `^\s*(import|from) <pkg>` under `services`, `ui_qt`,
`meeting`, `transcriber`, `main.py`, `config.py`). Versions are the pins in `requirements-release-constraints.txt`.

### 1a. Direct runtime dependencies (`requirements.txt`)

| Package (pin) | What the app uses it for | C++ answer | Status |
|---|---|---|---|
| **PyQt6** 6.11.0, PyQt6-Qt6 6.11.2, PyQt6_sip 13.12.0 | Entire UI. 209 files; only `QtCore`, `QtWidgets`, `QtGui` plus `QtDBus` in `services/omarchy_controls.py`. SVG icons need the Qt SVG image plugin (`Qt6Svg.dll`, `qsvg`). `QPdfWriter` (QtGui) writes walkthrough PDFs. No `QtNetwork`, `QtMultimedia`, `QtSql`. | Qt 6 modules: Core, Gui, Widgets, Svg, DBus (Linux). Qt is a prebuilt install, not vcpkg (BUILDING.md). | port |
| **sounddevice** 0.5.6 (bundles PortAudio) | Microphone capture and playback: `services/recorder.py`, `audio_player.py`, `audio_devices.py`, `meeting/capture/{sd_stream,devices}.py`. Its bundled PortAudio exposes WASAPI "[Loopback]" input devices, which Meeting Mode uses for "Others" audio on Windows. | vcpkg `miniaudio` (WASAPI loopback, Core Audio, PulseAudio/ALSA backends in one header; no loopback patch needed). Fallback: vcpkg `portaudio` 19.7 (stock builds do not expose loopback devices; verify before choosing it). | port |
| **numpy** 2.5.2 | Sample buffers, level meters, resampling and VAD maths in the audio path (38 files). | Not needed: `std::vector<float>` / `std::span`, hand-written loops. | port (as plain C++) |
| **keyboard** 0.13.5 (Windows) | Global hotkeys with per-key suppression (`services/_hotkey_keyboard.py`), synthetic Ctrl+V for auto-paste (`services/synthetic_keys.py`), key capture widget (`ui_qt/widgets/hotkey_capture.py`). | OS API: `SetWindowsHookEx(WH_KEYBOARD_LL)`, `RegisterHotKey`, `SendInput`. | port |
| **pynput** 1.8.2 (macOS, Linux) | Global hotkeys and synthetic paste (`services/_hotkey_pynput.py`, `_hotkey_carbon.py`, `_x11_key_grab.py`, `synthetic_keys.py`). | OS API: macOS Carbon `RegisterEventHotKey` / `CGEventTap` + `CGEventPost`; Linux Xlib `XGrabKey` (vcpkg `libx11`, `libxtst`, `xcb-keysyms`) and the Wayland GlobalShortcuts portal over QtDBus. | port |
| **python-xlib** 0.33 (transitive of pynput, imported directly) | `services/_x11_key_grab.py`, `services/focus_context/_linux.py`: key grabs, active window and focus lookup on X11. | vcpkg `libx11`, `libxtst`, or system libs on Linux. | port |
| **pywin32** 312 / pywin32-ctypes | Only `pythoncom`/`win32com` (`services/actions/launch.py`: open action links through Explorer's desktop so they run as the shell would) and `win32api`, `win32gui`, `pywintypes` (`ui_qt/snap/capture.py`, `pointed.py`: pointed-window capture for Snap). | OS API (COM `IShellDispatch`/`ShellExecute`, user32/gdi32). | port |
| **openai** 3.6.0 | OpenAI-compatible HTTP client: cloud transcription (`transcriber/openai_backend.py`), text cleanup and rewrite (`services/text_llm.py`, `transcript_cleanup.py`), meeting cloud diarization pass (`meeting/diarize/cloud_pass.py`), OpenRouter meeting agent path (`meeting/agent/openrouter_direct.py`), `services/openai_retirement.py`. | QtNetwork (`QNetworkAccessManager`, `QHttpMultiPart` for uploads, SSE line parser) plus `QJsonDocument` or vcpkg `nlohmann-json`. No C++ SDK needed. | port for text LLM; undecided for cloud transcription |
| **certifi** 2026.7.22 | CA bundle for `ssl` contexts in `services/http_tls.py` (frozen Python has no OS trust store by default). | Not needed: Qt/OpenSSL use the OS trust store (SChannel, Security.framework, system OpenSSL). | not needed |
| **faster-whisper** 1.2.1 | Local Whisper engine: model aliases, `hf_access.py` download coordinator, VAD model data. | None. | drop |
| **av** (PyAV) 18.1.0, pinned below 19 | Audio file decode and resample to 16 kHz mono s16 for file transcription and incremental dictation (`services/local_asr/audio.py`, `audio_processor.py`, `incremental_dictation.py`, `application_controller.py`, `meeting/diarize/cloud_audio.py`). Bundles its own FFmpeg (`av.libs`). | vcpkg `ffmpeg` with only `avcodec`, `avformat`, `swresample` (LGPL, shared). Alternative if only WAV/FLAC/MP3 are required: `miniaudio` decoders + `libsndfile`. | port |
| **python-dotenv** 1.2.3 | `.env` credential fallback (`services/credentials.py`). | Not needed (`qEnvironmentVariable` + a 30-line parser if `.env` parity is wanted). | port (trivial) |
| **keyring** 25.7.0 + **SecretStorage** 3.5.0 + jeepney | API keys in Windows Credential Manager, macOS Keychain, Secret Service (`services/credentials.py`, 591 lines: explicit backend choice, timeouts for a locked keyring, no accidental keyring creation on read). | vcpkg `qtkeychain-qt6` 0.18.0 (verify it matches the Qt kit's version), or direct OS APIs (`CredRead`/`CredWrite`, `SecItem*`, vcpkg `libsecret`). | port |
| **sqlalchemy** 2.0.52 (+ greenlet) | SQLite ORM for history, stats, meetings, agent API store (`services/database.py`, `models.py`, `backup.py`, `meeting/persist/repository.py`, `agent_api/store.py`, `dictation_stats.py`). Backup relies on SQLite's online backup API. | vcpkg `sqlite3` (3.54.1) used directly, with a thin repository layer; `sqlitecpp` or `sqlite-orm` optional. Qt Sql (`QSQLITE`) is a workable alternative but hides `sqlite3_backup`. | port |
| **pypdf** 6.16.2 | Meeting knowledge-folder PDF text extraction (`meeting/context_folder.py`). | Qt PDF module (`QPdfDocument::getAllText`, Qt 6.6+; confirm the module is in the installed Qt kit) or vcpkg `poppler` (GPL). | port |
| **python-docx** 1.2.0 | Reads `.docx` in the knowledge folder and writes Word export of walkthroughs (`services/walkthrough/word.py`, 10 imports). | vcpkg `libzip` + `pugixml` (OOXML is zip + XML). | port |
| **python-pptx** 1.0.2, **openpyxl** 3.1.5, **lxml** 6.1.2, et_xmlfile | Knowledge-folder extractors for `.pptx` and `.xlsx` (`meeting/context_folder.py`). lxml is a docx/pptx dependency. | Same `libzip` + `pugixml` code path. | port |
| **fastapi** 0.141.1, **starlette** 1.6.0, **uvicorn[standard]** 0.52.4 (uvloop, httptools, watchfiles), sse-starlette, python-multipart | Three local HTTP servers: meeting dashboard + WebSocket (`meeting/web/{api,ws,server}.py`), agent API (`services/agent_api`), automation API (`services/automation/api.py`), and the MCP HTTP runtime (`services/agent_mcp/runtime.py`). | Qt `HttpServer` + Qt `WebSockets` (both Qt "additional libraries"; `qthttpserver` and `qtwebsockets` exist in vcpkg 6.11.3), or vcpkg `cpp-httplib` 0.66.0 for REST only, or `boost-beast` for HTTP + WebSocket. | port (decision belongs to areas a20/a24) |
| **pydantic** 2.13.5 (+ core, annotated-types, typing-inspection) | Request and response models for agent API and MCP (`services/agent_api/{models,store,federation,app}.py`, `agent_mcp`, `remote_history/client.py`). | Not needed: plain structs + `nlohmann::json` with explicit validation. | port (as hand-written validation) |
| **mcp** 2.2.0 (+ mcp-types, jsonschema, referencing, rpds-py, attrs, jsonschema-specifications, pyjwt) | Official MCP SDK: agent access to saved history (`services/agent_mcp/app.py`, `events_ext.py`). | No MCP SDK in vcpkg. Hand-rolled JSON-RPC 2.0 over stdio and HTTP (`nlohmann-json`; `json-rpc-cxx` exists as an optional helper). | port |
| **httpx** 0.28.1 (+ httpcore, h11, anyio, sniffio, idna) | Outbound HTTP: automation webhooks, local text servers, speech endpoints, component downloads, MCP event subscriptions (12 files). | QtNetwork. | port |
| **websockets** 17.1 | Remote engine: TLS WebSocket host and client (`services/remote_asr/{host,client}.py`). | Qt WebSockets (`QWebSocketServer` with `QSslConfiguration`). | undecided (remote engine) |
| **cryptography** 50.0.1 (+ cffi, pycparser) | Self-signed certificate for the remote engine host; clients pin it at pairing (`services/remote_asr/tls.py`). | vcpkg `openssl` (X509 API). Qt cannot generate certificates. | undecided (remote engine) |
| **soundcard** 0.4.6 (Windows, Linux) | System-audio loopback: WASAPI fallback on Windows, production path on Linux via Pulse/PipeWire-Pulse (`meeting/capture/soundcard_stream.py`, `linux_audio.py`). | `miniaudio` (loopback device type; PulseAudio backend); `pactl` subprocess calls stay for diagnostics. | port |
| **pyobjc-framework-ScreenCaptureKit / CoreMedia / Quartz** 12.2.2 (+ Cocoa, CoreText, ApplicationServices, pyobjc-core) | macOS Meeting Mode system audio (ScreenCaptureKit stream, CoreMedia buffer decode), Accessibility (HIServices), Snap capture, focus context, mouse monitor (`meeting/capture/sck_stream.py`, `ui_qt/snap/macos*.py`, `services/focus_context/_mac.py`, `_mouse_monitor_mac.py`, `synthetic_keys.py`). | OS API in Objective-C++: ScreenCaptureKit, CoreMedia, CoreGraphics, ApplicationServices, AppKit. | port (macOS only) |

### 1b. Imported but not in `requirements.txt`, and optional packages

| Package | Use | C++ answer | Status |
|---|---|---|---|
| **huggingface_hub** (transitive of faster-whisper) | Model downloads and cache layout (`services/hf_access.py`, `whisper_models`, WeSpeaker model in `components.py`). | Own downloader (resumable, pinned size and SHA-256; section 3) in QtNetwork. | port (downloader); hub client drop |
| **tqdm** | Progress adapter for hub downloads. | Not needed. | drop |
| **onnxruntime** 1.29.0 (transitive) | WeSpeaker ResNet34-LM speaker embeddings on CPU (`meeting/diarize/embedder.py`); faster-whisper VAD. | vcpkg `onnxruntime` 1.31.1. | port (meeting diarization); VAD part drop |
| **ctranslate2** 4.8.1, **tokenizers**, **flatbuffers**, **protobuf** | Whisper engine and its model loading. | None. | drop |
| **torch** (not shipped) | Two `try: import torch` blocks in `services/local_asr/worker.py`; Qwen runtime component. | None. | drop |
| **nvidia-cublas-cu12**, **nvidia-cuda-runtime-cu12**, nvidia-cuda-nvrtc-cu12 (`requirements-gpu.txt`) | CUDA libraries for CTranslate2 (cuBLAS only; cuDNN deliberately omitted). Shipped as the `gpu-accel` component (3 wheels, about 633 MB download). | Not needed: the NeMo-Speech.cpp CUDA release archives carry their own cudart and cuBLAS (NOTICES: "the Linux CUDA archive includes NVIDIA's CUDA runtime and cuBLAS"; `nvidia.py` comments the same for `RUNPATH=$ORIGIN`). | not needed |
| **scipy**, sympy, networkx, matplotlib, tkinter | Excluded from the freeze and rejected by a build check. | None. | not needed |
| **pillow** 12.3.0, **PyYAML**, **truststore**, **xlsxwriter**, **psutil**, **requests**, **markdown** | Pins or incidental (greps show no direct imports of pillow, yaml, truststore, xlsxwriter; `psutil` and `requests` hit only in text matches). `scripts/generate_icon.py` deliberately avoids Pillow. | None. | not needed |
| **evdev** 2.0.0 | Pin for pynput on Linux; not imported. | None. | not needed |

### 1c. Build, test and tooling dependencies

| Tool (pin) | Use | C++ answer |
|---|---|---|
| PyInstaller 6.22.2 (+ hooks-contrib, altgraph, pefile) | Freeze. | CMake `install()`, Qt deploy script (`qt_generate_deploy_app_script`) or `windeployqt` / `macdeployqt`, CMake runtime dependency sets. |
| pytest 9.1.1, pytest-xdist, pytest-qt style `qt` marker, pypdfium2 5.14.0 (reads PDFs back; "QtPdf left PyQt6 in 6.12") | Tests. | Catch2 (already in `vcpkg.json`) + CTest. PDF read-back: Qt PDF or `poppler` in tests only. |
| ruff 0.16.4, mypy 2.4.1, uv 0.11.8 (`uv.lock`, `scripts/sync_uv_dependencies.py`) | Lint, types, lock. | `.clang-format`, `.clang-tidy`, vcpkg baseline and `overrides`. |
| pip, packaging | Component pin builders resolve wheels with `pip install --dry-run --report`. | Not needed. |
| Inno Setup 6.4.0 (choco), Windows SDK SignTool, NuGet `Microsoft.Trusted.Signing.Client` 1.0.53 | Windows installer and signing. | Same. |
| dpkg-dev, lintian, desktop-file-utils, patchelf, libarchive-tools (bsdtar), zstd, xvfb, xauth, binutils, file, numfmt | Linux packaging. | Same, minus xvfb for the build (Qt offscreen is enough) and patchelf (we control RPATH). |
| Xcode CLT: codesign, hdiutil, iconutil, lipo, otool, plutil, notarytool, stapler, ditto, spctl | macOS packaging. | Same. |
| Node 22 + npm: `@earendil-works/pi-coding-agent` 0.84.1, `typebox` 1.3.11, esbuild, typescript (`sidecar/`) | Pi meeting agent bundle. | Undecided (area a27: keep Node sidecar or native agent loop). |
| Bun 1.3.14: `@opencode/sdk` and `@opencode/plugin` 2.0.18 (`sidecar-opencode/`) | OpenCode meeting agent. | Undecided (a27). |
| Vite 8, React 18.3, TypeScript, jsdom; `@fontsource` DM Sans and Instrument Serif (`webui/`) | Meeting dashboard bundle committed as `webui/dist`; CI fails if stale. | Keep the build as is if the dashboard stays a web page served by the C++ HTTP server. |
| Tabler Icons (`ui_qt/assets/tabler`, MIT) | Icon set. | Copy the SVGs into Qt resources. |

### 1d. Runtime artifacts the app downloads (not Python packages)

| Artifact | Source | C++ answer | Status |
|---|---|---|---|
| NeMo-Speech.cpp v0.1.0 release archives (Windows CPU 4.7 MB zip, Windows CUDA 106 MB zip; Linux CPU, CUDA, Vulkan tar.gz; macOS CPU, Metal, Intel CPU) | `github.com/NVIDIA/NeMo-Speech.cpp/releases`, pinned by URL + SHA-256 in `services/local_asr/nvidia_*_runtime.json` | Same archives; OpenWhisper_CPP already loads the C ABI at run time by path (`NemoApi::get`). | port |
| `python-3.12.10-embed-amd64.zip` (11 MB download) shipped inside every Windows NeMo runtime component | python.org | Not needed: the Python worker is gone, so OpenWhisper_CPP's runtime component is the NeMo archive alone. | drop |
| Parakeet GGUF weights (`parakeet-tdt-0.6b-v3.q8_0.gguf`, commit-pinned Hugging Face URLs) | `models.json` | Same files, same pinning. | port |
| Nemotron 3.5 streaming GGUF | `models.json` | Same runtime. | undecided |
| Parakeet Unified EN, Orukeet, Qwen3-ASR, Moonshine, MLX runtimes and weights | `models.json`, `*_runtime.json` | None. | drop |
| WeSpeaker ONNX (`voxceleb_resnet34_LM.onnx`, repo revision `main`, verified after download) | Hugging Face | Download with the shared downloader; consider pinning a commit (OpenWhisper uses the mutable `main`). | port (meeting diarization) |
| Node 22.23.2 per platform + Pi `bundle.cjs`; Bun 1.3.14 + OpenCode payload | nodejs.org, GitHub release assets | See section 3. | undecided |

### 1e. What the C++ app needs, in one list

- Qt 6 modules (from the Qt kit): `Core`, `Gui`, `Widgets`, `Svg` (plugin), `Network`, `WebSockets`, `HttpServer`, `Pdf`, `DBus` (Linux only).
- vcpkg ports (verified present in the local vcpkg checkout): `miniaudio`, `ffmpeg` (features `avcodec avformat swresample`), `sqlite3`, `libzip`, `pugixml`, `nlohmann-json`, `openssl`, `onnxruntime`, `qtkeychain-qt6`, `catch2` (already). For updating and packing: `libarchive` (with `liblzma`) or `liblzma` + `libarchive`; SHA-256 can use Windows CNG/CommonCrypto/OpenSSL instead. Linux global hotkeys: `libx11`, `libxtst`, `xcb-keysyms`.
- OS APIs, no package: Win32 (hooks, `SendInput`, COM, registry, mutexes, `CredRead`), Carbon/CoreGraphics/ScreenCaptureKit/AppKit/Security on macOS, X11 and D-Bus on Linux.

## 2. What the installers do

### Windows (Inno Setup 6, `installer/OpenWhisper.iss`)

| Aspect | Behavior |
|---|---|
| Install location | `{autopf}\OpenWhisper`; default is per-user `%LOCALAPPDATA%\Programs\OpenWhisper` because `PrivilegesRequired=lowest`. `PrivilegesRequiredOverridesAllowed=dialog` lets the user pick all-users (`Program Files`, HKLM); the app then updates through setup, never natively. |
| Identity | `AppId={CA36AD0A-13B9-4737-87AD-ADB54A28EFC9}` (never change; a new GUID makes an upgrade a second install). Uninstall key `...\Uninstall\{GUID}_is1`. Publisher "Fiori Labs", URL `openwhisper.fiorilabs.tech`. |
| Platform gate | `MinVersion=10.0`, `ArchitecturesAllowed=x64compatible`, 64-bit install mode. `ShowLanguageDialog=no`, English only, `WizardStyle=modern`, license page shows `LICENSE`. |
| Payload | Entire `dist\OpenWhisper` (about 300 MB unpacked: Python 3.12, Qt, CTranslate2, onnxruntime, FFmpeg DLLs) with `lzma2/max`, solid compression, `LZMANumBlockThreads=1` (32-bit ISCC runs out of memory at `ultra64`). `OpenWhisperUpdater.exe` is also embedded with `dontcopy` and extracted to `{tmp}` for setup actions. |
| Shortcuts | Start menu group `{group}\OpenWhisper` and `Uninstall OpenWhisper` (no program-group page); optional tasks: desktop shortcut (checked) and "Start OpenWhisper when I sign in" (`{userstartup}` shortcut, unchecked). No PATH change, no file associations, no service, no firewall rule, no registry settings of its own. |
| Launch | `[Run]` "Launch OpenWhisper" (`postinstall nowait skipifsilent`); a second entry (`runasoriginaluser`, only when called with `/OPENWHISPERUPDATE=1`) restarts the app after an in-app setup update. |
| Closing the app | `CloseApplications=force`, filter `*.exe,*.dll`; `AppMutex` lists four names (app and update mutex, plain and `Global\`), `SetupMutex=Global\OpenWhisper-Setup-<GUID>`. `PrepareToInstall` also runs `taskkill /f /im OpenWhisper.exe` ("force" is reported to leave processes; setup is the recovery path for builds that cannot quit). |
| Stale files | Inno does not delete files an older version had. `PrepareToInstall` runs the bundled helper `OpenWhisperUpdater.exe --setup-action prepare --app-dir {app} --error-file ...`, which copies the managed set (`OpenWhisper.exe`, `OpenWhisperUpdater.exe`, `.openwhisper-update.json`, `_internal\`, the completion sentinel) to `<app>.setup-backup` and removes them; `ssPostInstall` runs `finish` (verify the new tree against the embedded manifest, tolerating legacy `_internal\nvidia`, then delete the backup); `DeinitializeSetup` runs `rollback` if setup was prepared but not finished. User data is never in this set. The smoke test plants a retired DLL and asserts it is gone. |
| Upgrades | If the uninstall key exists in HKCU or HKLM, the welcome, license, directory, program-group and tasks pages are skipped. |
| Uninstall | Removes `_internal`, the exe, the helper, the manifest and sentinel, `%LOCALAPPDATA%\OpenWhisper\updates`. Then asks (suppressible; default **No** when silent) whether to delete `%LOCALAPPDATA%\OpenWhisper` (settings, history DB, recordings, downloaded components). Model caches under `~\.cache\huggingface` are never touched. |
| Data kept | Everything in `%LOCALAPPDATA%\OpenWhisper` (settings, DB, logs, recordings, meetings, `components\`) unless the user says yes. |
| Signing | `SignTool=openwhisper` and `SignedUninstaller=yes` only when `/DSignToolName` is passed; see below. |
| Silent switches used in CI | `/SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER /DIR=... /LOG=...`. |

Windows code signing (`scripts/windows_signing.py`, `docs/windows-signing.md`):

- Off unless configured; a partial or conflicting configuration fails the build. Exactly one of: `OPENWHISPER_SIGN_CERT_SHA1` (store thumbprint, includes tokens and cloud HSMs), `OPENWHISPER_SIGN_PFX` + `OPENWHISPER_SIGN_PASS`, or `OPENWHISPER_SIGN_DLIB` + `OPENWHISPER_SIGN_METADATA` (Azure Trusted Signing; credentials from `AZURE_*`).
- SHA-256 file digest and RFC 3161 timestamp (`timestamp.digicert.com`, or `timestamp.acs.microsoft.com` for Trusted Signing). Signs `OpenWhisper.exe` and `OpenWhisperUpdater.exe` before the update archive is packed; Inno signs the uninstaller and the setup exe via `/Sopenwhisper=...` built by `inno-args` (with `$` escaped for Inno). Qt and Python DLLs are not signed.
- `verify` runs `Get-AuthenticodeSignature` (status, signer, timestamp) and `signtool verify /pa /tw` on the three files, requires one signer for all, writes `build/windows-signing.json` (schema 1). An unsigned build must show no signature at all. `metadata` binds the report to the exact setup exe by SHA-256 and emits `windows_signed` and `windows_signing` for `BUILD-METADATA.json`.
- CI imports a base64 PFX into the user cert store (and deletes it after), or installs the NuGet dlib and writes a metadata JSON. **No signing path has ever run end to end** (the doc says so); only the argument building, Inno quoting, report validation and metadata are unit-tested.
- Unsigned builds are the default; the build prints that SmartScreen will warn.

### Linux (`.deb` and Arch package; same staged tree)

| Aspect | Behavior |
|---|---|
| Layout | `/usr/lib/openwhisper/` (the whole frozen tree), `/usr/bin/openwhisper` (launcher script), `/usr/bin/ow` (symlink), `/usr/share/applications/openwhisper.desktop`, `/usr/share/icons/hicolor/256x256/apps/openwhisper.png`, `/usr/share/doc/openwhisper/{copyright,README.md,linux-system-audio.md,THIRD_PARTY_NOTICES.md,changelog.gz,third-party/*}`, `/usr/share/licenses/openwhisper/{LICENSE,PyQt6-GPL-3.0.txt,Qt-LGPL-3.0.txt}`. System-wide, root-owned, no per-user install. |
| Launcher | If called with no arguments and `gdbus` exists, tries `org.openwhisper.Control.Trigger show false` on the session bus to raise a running instance; otherwise `exec /usr/lib/openwhisper/OpenWhisper "$@"`. Never runs in the install dir (keeps relative user paths out of `/usr/lib`). |
| Desktop entry | `Categories=AudioVideo;Audio;`, `StartupWMClass=OpenWhisper`, `Terminal=false`, `Exec=openwhisper`. |
| Dependencies | Deb: `libc6 (>= <computed glibc>), libdrm2, libegl1, libgl1, libportaudio2, libwayland-client0, libwayland-cursor0, libwayland-egl1, libxcb-cursor0, libxcb-icccm4, libxcb-keysyms1, libxcb-shape0, libxcb-xkb1, libxkbcommon-x11-0`; Recommends `gnome-keyring, pulseaudio-utils`. Arch: `glibc>=, libdrm, libegl, libgl, portaudio, wayland, xcb-util-cursor, xcb-util-wm, xcb-util-keysyms, libxcb, libxkbcommon`; optdepends gnome-keyring, libpulse. The glibc minimum is the highest `GLIBC_x.y` symbol version found by `readelf` over every ELF (fallback 2.17). Built on Ubuntu 22.04 (glibc 2.35). |
| Hygiene | Strips build-host rpath (`patchelf`), refuses bundled `libstdc++`/`libgcc_s` (they break Arch's newer Mesa), forbids broken symlinks and unsafe modes (`-perm /0022 -o /6000`), normalizes `.so` modes to 0644, removes `__pycache__`. Lintian with `--fail-on error` and three documented overrides (embedded-library, unstripped, library-not-linked-against-libc). |
| Update | None in-app: notify only; the package manager owns the files. |
| Uninstall | `apt remove` / `pacman -R`. No maintainer scripts. User data in `~/.local/share/OpenWhisper` (or `XDG_DATA_HOME`) is untouched. |
| Signing | None (no repository, no GPG signing). |
| Reproducibility | `SOURCE_DATE_EPOCH` from `git log -1`, `touch -h`, `gzip -n`, bsdtar with uid/gid 0, mtree, `dpkg-deb -Zxz -z9`, zstd -19. |

### macOS (`build_installer_macos.sh`, `OpenWhisper.spec`)

- Output `OpenWhisper-<ver>-macos-arm64.dmg`: UDZO zlib-9 image of `OpenWhisper.app` plus an `/Applications` symlink. Drag-to-install; no installer package.
- Bundle id `tech.fiorilabs.openwhisper`, `LSMinimumSystemVersion` 14.0, `NSHighResolutionCapable`, microphone and audio-capture usage strings. `NSScreenCaptureUsageDescription` is forbidden by the build (ScreenCaptureKit audio capture is used under `NSAudioCaptureUsageDescription`).
- Signing: ad-hoc by default (users approve via Privacy & Security, Open Anyway; the script says never to advise disabling Gatekeeper). With `OPENWHISPER_MACOS_CODESIGN_IDENTITY` the freeze signs with that Developer ID under the hardened runtime and `installer/macos/OpenWhisper.entitlements`; with a notary profile or an API key (`.p8`, key id, issuer) it also notarizes and staples the app, then signs, notarizes and staples the DMG, and runs `spctl --assess`.
- Entitlements: `device.audio-input`, `cs.disable-library-validation` (downloaded runtimes are signed by their publishers, not by this team), `cs.allow-jit` and `cs.allow-unsigned-executable-memory` (Numba/llvmlite and ctypes callbacks in the Python runtimes). No App Sandbox.
- Audits: every Mach-O has an arm64 slice, deployment target <= 14.0, no absolute build-host library paths (only `@rpath`, `@loader_path`, `@executable_path`, `/System`, `/usr/lib`).
- Update: in-app DMG flow (area a16). Uninstall: drag the app to the Trash; data in `~/Library/Application Support/OpenWhisper`.

### Source checkouts (developer)

`install.cmd` and `install.sh` register `scripts/` on PATH so `ow` and `openwhisper` run the venv's Python; `uninstall.*` reverse it. Neither touches the venv or code. The source launchers resolve symlinks, export `VIRTUAL_ENV`, handle the Linux NVIDIA wheel `LD_LIBRARY_PATH`, and run `check_linux_dependencies()`. All of this disappears in C++.

## 3. Separately downloaded components: build, version, verify, install

### Catalog model (`services/components.py`, "the only catalog of component download URLs")

- An entry has `version`, `component_api` (currently 1; bump when the shell changes in a way that invalidates payloads), `platform`, `install_bytes`, and `archives[]` of `{name, url, sha256, size_bytes, extract, member?, root?}`. Platform tags: `win_amd64`, `linux_x86_64`, `linux_aarch64`, `darwin_arm64`, `darwin_x86_64`. `published: False` hides a platform until its archive exists and is pinned. Speech runtime manifests are `services/local_asr/*_runtime.json` (hand-edited for NeMo, generated for wheels).
- Extract kinds: `zip`, `nvidia-wheel` (only `nvidia/*/bin/*.dll`), `python-wheel`, `node-exe`, `node-tar`, `nemo-tar` and `transcribe-tar` (renames the archive's top folder to `nemo-speech`), default safe zip extract. All reject absolute paths, `..`, links outside staging; tar uses `filter="data"`.
- Install algorithm (`_install_component`): compatibility check (ABI, `macos_min`) -> free space check (downloads + `install_bytes`) -> resumable verified download of each archive into the cache (`.part`, `Range` resume, size and SHA-256 must match, fsync then rename; speech runtimes re-check a cached archive and discard it on mismatch) -> extract into `staging/<id>.<pid>` -> per-component payload validation -> write `manifest.json` then the `.installed` sentinel last -> swap with two atomic renames (`<dest>` to `<dest>.old`, staging to `<dest>`), restoring on failure, retry up to 15 s because Windows scanners hold new files -> delete rollback and cached archives. A tree without the sentinel is not installed. Orphans are pruned at start. Components live in `%LOCALAPPDATA%\OpenWhisper\components\` (outside the install dir so app upgrades keep them). OpenCode installs under a cross-process lease.
- Validation per component: NVIDIA Windows needs `python.exe`, `python312.dll`, `python312.zip`, `bin/nemo_speech_asr_c.dll` and writes `python312._pth`; Linux needs `nemo-speech/lib/libnemo_speech_asr_c.so`; macOS needs `libnemo_speech_asr_c.dylib` (+ `libggml-metal.dylib` for Metal); GPU needs `cublas64_12.dll` / `libcublas.so.12`; meeting agent runs `node --version` (exact match to the pin) and `node --check bundle.cjs` with a 10 s timeout, requiring non-empty, non-symlink, executable files; OpenCode verifies every file's SHA-256 from `payload.json` and runs an offline self-test.
- Update detection is by `version` string; installed copies with another version are offered the new payload.

### Builders

| Component | Builder | What it pins and how |
|---|---|---|
| `gpu-accel` (CUDA 12.9 libs for CTranslate2) | `build_component.py gpu-accel` | `pip install --dry-run --report` against `requirements-gpu.txt` for `win_amd64` / cp312 resolves three wheels; digests from pip's report (PyPI wheels are immutable); downloads each to measure the extracted `.dll` bytes (`install_bytes`); prints a paste-ready `_BUILTIN_GPU_ARCHIVES` block. Fails if the resolved set is not exactly cublas, nvrtc, cuda-runtime. Human pastes it and bumps `GPU_COMPONENT_VERSION` (`cuda12.9`). Linux archives were pinned by hand from PyPI's JSON API. | drop (not needed for NeMo) |
| `meeting-agent` (Pi) | `build_component.py meeting-agent` | Downloads Node 22.23.2 for five platforms, verifies against `SHASUMS256.txt` (digest and size measured), `npm ci` and `npm run build` in `sidecar/` (esbuild bundle to `dist/bundle.cjs`), zips the bundle to `dist/components/meeting-agent-win_amd64-node22-pi2.zip`, emits catalog entries. The bundle is attached by hand to the GitHub Release tagged `MEETING_AGENT_RELEASE_TAG` (`v2.6.01`, deliberately not the latest app tag). Bump `MEETING_AGENT_COMPONENT_VERSION` when Node or the bundle changes. | undecided (a27) |
| `meeting-agent-opencode` | `build_component.py meeting-agent-opencode` -> `build_opencode_component.py` | Per-platform Bun archive with pinned SHA-256 (baseline x64 builds for CPUs without AVX2); `bun install --production --frozen-lockfile --ignore-scripts --linker hoisted`; deletes `.bin` shims; builds `main.mjs` and `self-test.mjs`; collects license text for every package into `THIRD_PARTY_NOTICES.txt` and `dependencies.json`; writes `payload.json` with a SHA-256 for every file and POSIX modes (zip drops them); refuses symlinks; runs `validate_payload` (all hashes plus the offline SDK self-test); builds a deterministic zip (sorted, fixed 2026-01-01 timestamps, level 9); writes a `.catalog.json`. After upload to the release `component-opencode-2.0.18-1` and a download back, `--pin` copies the measured pins into `services/opencode_catalog.py`. Run on each target platform (it executes the payload). CI builds and tests the Windows one. | undecided (a27) |
| Windows Qwen runtime | `build_windows_runtime.py asr-qwen` | Pip dry-run for embedded Python 3.12 win_amd64 with an extra PyTorch index; keeps every existing pin unless the runtime lists it in `upgrade`; checks wheel dependency closure with a Windows marker environment; rejects wheels whose modules sit under `*.data/purelib`; downloads with a custom User-Agent (PyTorch's mirror rejects urllib's); prefers PyPI URLs when PyPI hosts the exact file; writes `qwen_runtime.json` (version defaults to the date, e.g. `2026-11-14.1`). | drop |
| macOS Moonshine/Qwen/MLX runtimes | `build_macos_runtime.py`, `build_mlx_runtime.py` | Same pattern for Apple Silicon; computes `macos_min` from the newest wheel tag. | drop |
| NeMo-Speech.cpp runtimes | none | `nvidia_*_runtime.json` are edited by hand to point at the vendor's `v0.1.0` GitHub release assets with their SHA-256 (Windows CPU 4.7 MB, CUDA 106 MB; Linux CPU/CUDA/Vulkan; macOS CPU/Metal/Intel). | port |
| WeSpeaker speaker model | in-app | Downloads `voxceleb_resnet34_LM.onnx` from the Hugging Face repo at revision `main`, validates it loads; `OPENWHISPER_SPEAKER_MODEL` overrides. | port |

Model weights follow the same pinning idea in `services/local_asr/models.json`: commit-hash URLs, size and SHA-256 per file; the app never follows a moving ref (WeSpeaker is the exception).

Licensing: `THIRD_PARTY_NOTICES.md` tells users that the OpenWhisper MIT license does not relicense runtimes or weights; each downloaded archive keeps its notices (`nemo-speech/share/licenses`).

## 4. Update archive and updater helper

### Release side

- Assets per release: `OpenWhisper-Setup-<ver>.exe`, `OpenWhisper-<ver>-win64.tar.xz`, `OpenWhisper-<ver>-linux-amd64.deb`, `OpenWhisper-<ver>-linux-x86_64.pkg.tar.zst`, `OpenWhisper-<ver>-macos-arm64.dmg`, `SHA256SUMS.txt`, `BUILD-METADATA.json`. Tags are `vMAJOR.MINOR.PATCH`; the tag must equal `_version.py`.
- Version is `major.minor.patch` only (`parse_strict_version`); suffixed tags are never offered or applied. Setup-only "bridge" releases (version <= `MINIMUM_UPDATER_VERSION` 2.5.2) omit the archive because old helpers cannot run the new protocol. This legacy exists only because OpenWhisper has an installed base; OpenWhisper_CPP does not need it.

### Archive format (`pack_update_archive.py`, `app_update_apply.pack_tar_xz`, `build_update_manifest`)

- `tar.xz` (preset 9) whose members are the contents of `dist\OpenWhisper` with relative paths: `OpenWhisper.exe`, `OpenWhisperUpdater.exe`, `.openwhisper-update.json`, and everything under `_internal/`. Only regular files; links, FIFOs, devices are rejected on pack and extract. Packed after signing, so it carries signed binaries.
- Manifest `.openwhisper-update.json`: `schema_version` 1, `product`, `app_id` (the Inno GUID), `version`, `architecture` `win-x64`, `topology_revision` 1, `minimum_updater_version`, `unpacked_bytes`, `member_count`, and `files{path: {size, sha256}}`. Topology rule: only `OpenWhisper.exe`, `OpenWhisperUpdater.exe`, the manifest and the `_internal/` tree may exist (so user content can never be replaced). A different schema or topology revision raises "must use the installer".
- Validation limits: 50,000 members, depth 20, name length 240, Windows reserved names (CON, PRN, NUL, COM1..9, LPT1..9) and path-traversal rejected, unpacked size at most 2 GiB, declared totals must equal the sum of file sizes. After packing, the script reads the manifest back, extracts to a temp dir and re-verifies every file.

### In-app check and download (`services/app_update.py`; UI in a16)

- `GET https://api.github.com/repos/Knuckles92/OpenWhisper/releases/latest` (stable) or `/releases?per_page=30` (beta; highest version wins, stable beats prerelease on a tie). Assets must match the exact name, state `uploaded`, URL exactly `https://github.com/<repo>/releases/download/<tag>/<name>`, size 0 < n <= 2 GiB, and carry GitHub's `digest: sha256:...`. Without a digest the app only notifies.
- If the API is rate-limited: API-free fallback: `HEAD /releases/latest` redirect gives the tag; `SHA256SUMS.txt` (<= 64 KiB, strict line format) gives digests; asset sizes from `HEAD`. Regular releases only.
- Redirect allow-list: `api.github.com`, `github.com`, `release-assets.githubusercontent.com`; at most 5 redirects. Downloads use the shared resumable verified downloader into `%LOCALAPPDATA%\OpenWhisper\updates`; verified files are reused within a 30-second window; stale downloads are pruned.
- Apply mode: `NATIVE` (valid HKCU Inno install, not under Program Files, not elevated, helper present, archive asset ready), `SETUP` (otherwise on Windows, or when the native payload requires setup), `MACOS_DMG` (Apple Silicon with a DMG asset), `NOTIFY_ONLY` (Linux, source, git).
- Soak: env `OPENWHISPER_UPDATE_FEED_URL` (loopback only) with `scripts/serve_update_feed.py [--prerelease]` serves a local build as the latest release; digests and manifests are still checked.

### Prepare, commit, rollback (`app_update_apply.py`, `updater_helper.py`)

1. **Prepare (inside the running app).** Create `updates\tx\<32-hex id>\` with `journal.json` (state `preparing`; old and new version, exe SHA-256s, parent pid, exe and creation time, 128-bit health token). Check free space for the old tree + unpacked size + a copy of the archive, reject reparse points (junctions/symlinks) in the install chain. Extract the archive to a sibling `<app>.new-<id8>`, verifying every file against the manifest as it goes (cancellable), copy the uninstaller (`unins*.exe/dat`) and legacy `_internal\nvidia` from the old tree, fsync the tree, write the `.openwhisper-update-complete` sentinel, stage a copy of the helper into the transaction dir (so the commit process is outside the install dir), set state `prepared`. The app returns `native:<id>` and quits.
2. **Commit (helper, `--transaction-id <id>`).** Relaunches itself from the transaction dir if it was started inside the install dir. Takes the setup mutex and the update mutexes (fails with "setup program is already running" or "Another update is already in progress"), waits up to 120 s for the parent pid (verified by exe path and process creation time; force-closes it if it hangs), takes the app mutexes (plain and `Global\`), re-reads the Add/Remove Programs registration and checks that the install did not change, re-verifies the candidate tree against the journal, snapshots `openwhisper_settings.json` and `openwhisper.db` (SQLite online backup, so WAL data is included, with SHA-256 in `data-snapshot.json`), writes a `RunOnce` value `!OpenWhisperUpdateRecover` (the `!` delays deletion until the command succeeds) pointing at `helper --transaction-id <id> --recover`, persists `old_moved`, renames `<app>` to `<app>.old-<id8>`, persists `new_active`, renames the candidate into `<app>`.
3. **Health check.** Releases the app mutexes, launches the new exe with `--update-health <token>`, waits up to 180 s for the app to write the matching `healthy` file in the transaction dir (`write_health_acknowledgement`). On success: update the ARP entry (`DisplayVersion`, `EstimatedSize`, `InstallDate`), state `healthy`, clear RunOnce, delete the transaction and the rollback tree (`--cleanup-transaction` helper run waits for the parent and stays silent).
4. **Rollback.** Any failure terminates the new process, takes the app mutex, restores the old tree, restores the data snapshot (verifying hashes, deleting `-wal`/`-shm`), restores ARP, deletes the candidate, writes `updates\apply_error.txt`, sets `rolled_back`, relaunches the old exe unless the original app is still running. A failure after the health check passed only records an error.
5. **Crash recovery.** `recover_before_start()` at app start and the RunOnce command reconcile the journal against the three directory names, so power loss between either rename is safe; `maybe_exit_if_update_in_progress()` makes ordinary launches exit while an update holds the mutexes. Abandoned transactions are pruned (legacy preparations after 24 h). The next launch shows the "Update failed" box from `apply_error.txt`.
6. **Setup fallback.** Any native failure can fall back to the verified setup exe, run silently over the install. Setup first retires native transactions (restores a half-applied one and marks it `superseded`).

Constants worth keeping: parent wait 120 s, health wait 180 s, lock retry 60 s (cleanup 300 s), free-space margin 1.15x, unpacked cap 2 GiB.

### Shared speech host (`docs/host-updates.md`)

On Omarchy (Linux, Hyprland), an unattended updater calls `org.openwhisper.Control.PrepareUpdate` (JSON reply with `reason` to defer or a `token` valid for `expires_in` seconds), `CancelUpdate <token>`, and polls `UpdateStatus` (`engine_ready`, `recordings`, `requests`, `untracked_clients`, `maintenance`) after restart. New capture and speech requests get a temporary refusal while reserved. Undecided with the remote engine.

## 5. CI and release qualification

### `ci.yml` (PR, push to main, and `workflow_call` with `source_ref`)

| Job | Runner | What it runs |
|---|---|---|
| python | ubuntu-22.04, 25 min | `sync_uv_dependencies.py --check`, `uv lock --check`; apt Qt runtime libs; `pip install -r requirements*.txt -c release-constraints`; `pip check`; `ruff check --select F`; `mypy` (opt-in module list); full pytest under `xvfb-run` with `-n auto`, `faulthandler_timeout=120`, `timeout 20m` |
| macos | macos-15, 30 min | Loopback alias `127.0.0.2`, clean venv, `check_testclient_compat.py`, full pytest |
| windows | windows-2022, 40 min | Full pytest; Bun 1.3.14; `sidecar-opencode` typecheck and tests; builds the Windows OpenCode component and runs its packaged-SDK test; uploads the component |
| desktop-core | matrix: windows 3.12 release pins, macos 3.12 release pins, windows 3.11 source deps, macos 3.12 source deps | Cross-platform recording, cancellation, persistence, backup, meeting recovery, remote host, walkthrough regressions; `check_release_health.py`; uploads `pip freeze` and timing JSON |
| webui | ubuntu-22.04 | `npm ci`, `npm test`, `npm run build`, fails if `webui/dist` differs from the committed bundle |
| sidecar | ubuntu-22.04 | `npm ci`, typecheck, tests for `sidecar/` |

All actions are pinned to commit SHAs; `permissions: contents: read`; concurrency groups cancel superseded PR runs.

### `build-installers.yml` (manual dispatch: `release_tag`, `setup_only_windows`)

1. `source`: resolves the tag or SHA to one commit; every later job checks out that commit.
2. `qualification`: calls `ci.yml` on that commit (the full suite runs once, so the builders go straight to packaging).
3. `windows` (windows-2022, 90 min): validates tag equals `_version.py`; installs pinned deps + Inno Setup 6.4.0 via choco; ASGI compat check; updater regression tests; configures signing (PFX or Azure Trusted Signing, never both; secrets scoped to the steps that need them); `build_installer.ps1 -Clean`; removes the cert in an `always()` step; uploads `windows-signing.json` and `release-health.json`; runs the frozen-helper protocol test; downloads the **latest published** setup (requires one asset with a `sha256:` digest and verifies it), then `smoke_windows_setup.ps1`: install the previous release silently, plant a stale DLL and a user-data file, install the new setup over it, assert the stale file is gone, user data kept, ARP `DisplayVersion` updated, `--self-test` passes, silent uninstall removes the app but keeps data; uploads setup logs.
4. `linux` (ubuntu-22.04): apt packaging deps and Qt libs; `build_installer.sh --clean`; in clean `ubuntu:22.04` and `debian:12` containers installs the `.deb`, checks `--version` for `openwhisper` and `ow`, dpkg version, desktop file and icon, runs `ldd` on every ELF, runs `xvfb-run openwhisper --self-test`, removes the package and checks that nothing remains; in `archlinux:latest` runs `smoke_arch_package.sh` (`pacman -U`, version, license files, `pacman -Qkk`, loader `--list` per ELF, advisory `namcap`).
5. `macos` (macos-15, 150 min, Apple Silicon only): imports the Developer ID p12 and notary key into a temporary keychain when all five secrets exist; `build_installer_macos.sh --clean`; deletes the keychain always.
6. `bundle`: expects exactly 4 artifacts (5 with the Windows archive), checks each name pattern, writes `BUILD-METADATA.json` (schema 3: `source_qualification: passed`, `windows_setup_only`, signing record, version, commit, ref, run id and attempt, UTC time) and `SHA256SUMS.txt`; uploads `openwhisper-release-candidate`. Artifacts retained 14 days.

### `qualify-release.yml` (manual: `candidate_run_id`, `release_tag`, `require_windows_signed`)

Checks that the candidate run succeeded, was a dispatch of `build-installers.yml` in this repository, downloads its bundle, runs `check_release_qualification.py` (every SHA-256 matches, metadata schema 3, exact artifact inventory, commit equals the tag's commit, run id matches, optional signed requirement), confirms the target release is still a **draft** whose tag points at that commit, validates existing draft assets (rejects unrelated ones), uploads with `gh release upload --clobber` (deleting a stale win64 archive when a setup-only candidate replaces it), and re-validates the uploaded asset list. Publishing stays manual: publish as a prerelease (`gh release edit vX.Y.Z --draft=false --prerelease --latest=false`) to soak with beta users, then promote (`--prerelease=false --latest`) without rebuilding. Never replace assets or reuse a tag; fix forward.

### Release health gate (`scripts/check_release_health.py`, `docs/release-health.md`)

Runs on source and on the frozen executable (and, on macOS, on the app copied out of the DMG): process exits within 45 s, and bounded startup, stop-to-result p95, cancel, restart, worker cleanup and peak RSS using a deterministic fixture decoder and the real queued-signal path; decodes a known WAV through production PyAV conversion; persists and reopens a SQLite history entry; recovers a recording journal without duplicating audio; creates a SQLite backup and applies a staged restore. Optional `--baseline` with 50% regression limit (100 ms and 50 MB noise floors). It does not measure ASR accuracy, capture, model load, GPU memory or window startup (the report says so). The frozen app has `--self-test` (module and asset load plus Qt bootstrap) and `--workflow-smoke`. `--diagnostics out.json` writes a safe, allowlisted support report (never overwrites, no audio, transcripts, keys, paths or hostnames; last 50 failure categories and numeric metrics).

Optional manual hardware checks (cold start, stop-to-result p95, cancel during decode and download, one-hour recording, mic removal, disk full, forced termination, GPU) are guidance, not release gates.

## 6. Proposal for OpenWhisper_CPP

Constraints: OpenWhisper_CPP has no installed base, so none of OpenWhisper's bridge-release, legacy-layout or Python-worker logic is needed. It must not reuse OpenWhisper's Inno `AppId`, mutex names, bundle id or `%LOCALAPPDATA%\OpenWhisper` folder (open question: branding and publisher name). The NeMo runtime is loaded by path and never linked.

### 6.1 Layout

Windows (`{app}` is `%LOCALAPPDATA%\Programs\OpenWhisper_CPP`):

```
openwhisper-cpp.exe               GUI, Qt + vcpkg DLLs beside it (loader requirement)
openwhisper-cpp-cli.exe           console companion (already built): --version, --self-test, --diagnostics
openwhisper-cpp-updater.exe       phase 2
Qt6*.dll, plugins\{platforms,styles,imageformats,iconengines,tls,networkinformation}
avcodec-*.dll, avformat-*.dll, swresample-*.dll, sqlite3.dll, onnxruntime.dll, libssl/libcrypto, ...  (miniaudio is header-only)
vcruntime140*.dll, msvcp140*.dll   app-local CRT (no redistributable prompt)
share\webui\                webui/dist if the dashboard stays a web page
share\licenses\, THIRD_PARTY_NOTICES.md
runtime\cpu\                optional: NeMo CPU runtime (see 6.5)
```

QSS, fonts and Tabler SVGs go into a Qt resource file (`qt_add_resources`), not loose files, so there is no `bundle_root()` equivalent. A GUI-subsystem exe prints nothing to a console (OpenWhisper's windowed build has the same limit), so CI uses `openwhisper-cpp-cli --self-test` (or the GUI calls `AttachConsole(ATTACH_PARENT_PROCESS)` for `--version`).

Linux: `/usr/lib/openwhisper-cpp/{bin,lib,plugins}` with `RPATH=$ORIGIN/../lib`, launcher `/usr/bin/openwhisper-cpp` (same `gdbus ... Trigger show` raise-existing trick if the Control interface is kept), `.desktop`, hicolor icon, docs, licenses. macOS: `OpenWhisper_CPP.app` produced by `MACOSX_BUNDLE`, Qt via `macdeployqt` or the CMake deploy script.

### 6.2 CMake install rules

Add to `src/app/CMakeLists.txt` (and the top-level file for data and licenses):

```cmake
include(GNUInstallDirs)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP OFF)
set(CMAKE_INSTALL_UCRT_LIBRARIES ON)          # app-local CRT on Windows
include(InstallRequiredSystemLibraries)

install(TARGETS openwhisper-cpp openwhisper-cpp-cli
        RUNTIME_DEPENDENCY_SET fv_runtime_deps
        BUNDLE DESTINATION . RUNTIME DESTINATION ${FV_BIN_DIR})
install(RUNTIME_DEPENDENCY_SET fv_runtime_deps
        PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-" "^libc\\.so" "^libm\\.so"
        POST_EXCLUDE_REGEXES ".*[/\\\\]system32[/\\\\].*" "^/(lib|usr/lib)/.*"
        DIRECTORIES $<TARGET_FILE_DIR:Qt6::Core>)     # vcpkg DLLs resolved via vcpkg's bin dir

qt_generate_deploy_app_script(TARGET openwhisper-cpp OUTPUT_SCRIPT fv_deploy_script
        NO_TRANSLATIONS NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${fv_deploy_script})            # windeployqt / macdeployqt / Linux plugin copy

install(DIRECTORY webui/dist/ DESTINATION share/openwhisper-cpp/webui)          # if kept
install(FILES LICENSE README.md DESTINATION share/doc/openwhisper-cpp)
install(FILES "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.md" DESTINATION share/doc/openwhisper-cpp)
install(DIRECTORY "${CMAKE_BINARY_DIR}/licenses/" DESTINATION share/licenses/openwhisper-cpp)
```

`licenses/` and `THIRD_PARTY_NOTICES.md` are generated at install time from `vcpkg_installed/<triplet>/share/<port>/copyright` plus a hand-written fragment for Qt, Tabler, the NeMo runtime and model licenses, replacing the manually maintained OpenWhisper table. Use `windeployqt` flags (check `windeployqt --help` for the installed Qt version): `--no-translations`, no compiler-runtime flag (CRT comes from `InstallRequiredSystemLibraries`), and decide about `opengl32sw.dll` (OpenWhisper keeps it as insurance; a Widgets raster app normally never loads it; keep it at first for parity, then drop after a clean-VM test). Smoke-check that ICU (`icuuc.dll`) and the TLS plugin are present, because OpenWhisper had to patch the ICU omission by hand.

Build the release with `cmake --preset windows` + `cmake --build --preset windows-release` (RelWithDebInfo; ship PDBs separately to a symbol store or release asset). Set `VCPKG_APPLOCAL_DEPS=ON` for development builds, and use the install rules above for packaging. A `package` CMake target runs `cmake --install --prefix stage` and then the platform script, so `ctest` and CI call one entry point.

### 6.3 Windows installer: `installer/OpenWhisper_CPP.iss` (Inno Setup 6)

Port `OpenWhisper.iss` nearly line for line, with these changes:

- New `AppId` GUID, `AppName=OpenWhisper_CPP`, new mutex names, new `%LOCALAPPDATA%\OpenWhisper_CPP` data directory, version from `/DAppVersion` passed by the build script (read from CMake `project(VERSION)`, single source of truth).
- Keep per-user install, `PrivilegesRequiredOverridesAllowed=dialog`, `CloseApplications=force`, `AppMutex`, `taskkill`, `SetupMutex`, skipped pages on upgrade, desktop and sign-in tasks, post-install launch, the uninstall-time "also delete your data?" prompt (default No when silent), and `Compression=lzma2/max` + `SolidCompression=yes` (payload is much smaller without Python: expect tens of MB, so `lzma2/ultra64` and multiple block threads become viable).
- Phase 1 stale-file strategy without a helper: `[InstallDelete]` entries for each managed name (`{app}\*.dll`, `{app}\openwhisper-cpp*.exe`, `{app}\plugins`, `{app}\platforms`, `{app}\imageformats`, `{app}\iconengines`, `{app}\styles`, `{app}\tls`, `{app}\share`, `{app}\runtime`), never `{app}\unins*`. User data lives outside `{app}`, so this is safe. Test with an equivalent of `smoke_windows_setup.ps1`. (Phase 2 replaces this with the helper protocol and manifest verification.)
- Drop the `dontcopy` helper embedding and the `prepare`/`finish`/`rollback` Code section in phase 1; reinstate it when `openwhisper-cpp-updater.exe` exists.
- Keep `/OPENWHISPERUPDATE=1`-style relaunch (`runasoriginaluser`) renamed to `/OPENWHISPER_CPPUPDATE=1`.
- Signing: keep the `/DSignToolName` + `/S` mechanism (generated by `windows_signing.py inno-args`); sign `openwhisper-cpp.exe`, `openwhisper-cpp-cli.exe`, `openwhisper-cpp-updater.exe`, the uninstaller and the setup exe. Consider signing our own DLLs too (a native app is a more visible SmartScreen and antivirus target than a PyInstaller bootloader; unsigned native binaries get less reputation). Reuse `windows_signing.py` unchanged apart from names (stdlib only, 349 lines), or port it to PowerShell to remove Python from CI; the Azure Trusted Signing action is a simpler alternative to the NuGet dlib dance. Test the first signed build by hand, as OpenWhisper's doc admits it never ran.
- Output: `OpenWhisper_CPP-Setup-<ver>.exe`, `OpenWhisper_CPP-<ver>-win64.zip` or `.tar.xz` (update archive, phase 2).

### 6.4 Linux and macOS packaging

- Linux: keep the stage-then-pack pattern from `build_installer.sh` (one staged `/usr` tree, two packers), but drop PyInstaller verification steps and `xvfb` (use `QT_QPA_PLATFORM=offscreen`). With a Qt kit bundled under `/usr/lib/openwhisper-cpp`, `dpkg-shlibdeps` will not see distro Qt, so keep an explicit `Depends` list (Qt's xcb platform plugin needs `libxcb-cursor0`, `libxcb-icccm4`, `libxcb-keysyms1`, `libxcb-shape0`, `libxcb-xkb1`, `libxkbcommon-x11-0`, plus `libportaudio2` becomes unnecessary with miniaudio, `libpulse0` only if linked). CPack's DEB generator can build the deb from the same install rules; keep the lintian gate. Keep the Arch package and the distro container smoke tests. Optional AppImage via `linuxdeploy` + Qt plugin. The package-time glibc computation (`readelf`) still applies and is cheap. The preflight in `install.sh` is unnecessary for packaged builds because Depends are enforced by dpkg/pacman.
- macOS: `MACOSX_BUNDLE` with an `Info.plist.in` carrying bundle id, `LSMinimumSystemVersion` 14.0 and the two usage strings; `qt_generate_deploy_app_script` or `macdeployqt` (without `-dmg`); keep `build_installer_macos.sh` almost verbatim (Info.plist checks, Mach-O audits, ad-hoc or Developer ID signing, notarization, DMG, mount-and-test). Entitlements shrink to `device.audio-input` and `cs.disable-library-validation` (the downloaded NeMo `.dylib` is signed by NVIDIA, not by us); `allow-jit` and `allow-unsigned-executable-memory` existed for Numba and ctypes and should not be carried over unless a library demands them. Apple Silicon arm64 first; Intel is an option because NeMo ships an Intel CPU runtime.

### 6.5 Components

- NeMo runtime: keep the catalog model (JSON, `version`, `component_api`, `platform`, `install_bytes`, archives with URL, SHA-256, size, `extract`, optional `root`), the verified resumable downloader, staging + two-rename swap, `.installed` sentinel last, per-component validation (`bin/nemo_speech_asr_c.dll`, `nemo-speech/lib/libnemo_speech_asr_c.{so,dylib}`, Metal extra dylib), path-safe extraction (zip-slip, link and permission filter), 15 s Windows scanner retry. Port these to a small C++ module (area a14) that OpenWhisper_CPP's `NemoApi::get` loads from `components/<id>/`. Drop the embedded Python zip: the Windows component is just the NeMo archive.
- **Decision for the owner:** ship the NeMo CPU runtime inside the installer (Windows CPU zip is 4.7 MB; Linux CPU 4.6 MB; macOS CPU 3.3 MB) so dictation works the moment a model is downloaded, and keep CUDA/Vulkan/Metal as optional downloads. The `gpu-accel` component, `build_component.py gpu-accel` and all Python wheel pinning scripts are not needed.
- Pin tooling: a small Python or CMake-script tool (`tools/pin-component`) that downloads an archive, computes SHA-256 and sizes, measures extracted bytes and emits the JSON entry (the `nvidia_*_runtime.json` files are currently hand-edited), plus a CI job that re-downloads every pinned URL and checks the digest weekly (OpenWhisper relies on the pins staying valid).
- Meeting agent runtimes (Node/Bun) stay undecided; if kept, port the Node SHASUMS verification and the deterministic OpenCode zip builder as CI scripts unchanged (they only need Python and the network, not the app).
- Pin the WeSpeaker model to a commit instead of `main`.

### 6.6 Update flow (phased)

- Phase 1 (release one): in-app check against GitHub Releases (same API, same API-free fallback, same host allow-list, same digest rules, beta track via prerelease), download the verified setup exe, run it silently with `/OPENWHISPER_CPPUPDATE=1`, quit. Linux and macOS notify only (macOS in-place swap is a later item, area a16). This needs no helper and about 700 lines of C++ in the app (a16).
- Phase 2: port the native swap: `openwhisper-cpp-updater.exe` (no Qt, Win32 + `std::filesystem`, `nlohmann-json`, `libarchive`+`liblzma` for `tar.xz`, CNG SHA-256), the transaction journal, mutexes, `RunOnce` recovery, health token, data snapshot (SQLite online backup of settings and DB), rollback; plus `fv-pack-update` (builds the manifest and archive from the install tree, then self-verifies). Share the manifest and journal code between the updater, the packer and the app in one static library `fv_update` so there is one definition of the topology and manifest schema. Replace the Inno stale-file `[InstallDelete]` with the helper's `prepare/finish/rollback` only if native updates prove worth their weight (it is about 3,500 lines in Python).
- Keep a rule that new archive formats use new `topology_revision` and `minimum_updater_version` so older helpers refuse rather than corrupt.

### 6.7 CI for OpenWhisper_CPP

Workflows (GitHub Actions, pinned by SHA, `contents: read`, concurrency groups):

1. `ci.yml` (PR, push, `workflow_call`): matrix `windows-2022` (MSVC, `cmake --workflow --preset windows-release`), `ubuntu-22.04` (`linux-release`), `macos-15`, plus `linux-asan` (Clang ASan+UBSan). Qt via a pinned install (`jurplel/install-qt-action` or `aqtinstall`) with `Qt6_ROOT`; vcpkg with `VCPKG_BINARY_SOURCES=clear;files,<cache>,readwrite` and `actions/cache` keyed on `vcpkg.json` + the baseline (vcpkg's `x-gha` source depends on GitHub's legacy cache service, which GitHub has been retiring; prefer the `files` source and verify before relying on `x-gha`). Steps: configure and build, `ctest` (engine tests, app tests under offscreen), `clang-format --dry-run -Werror`, `clang-tidy` on changed files, a separate job for the Qt-less engine build (compiler only) to enforce "Qt out of `src/engine`".
2. Decode tests: nightly and on `src/engine` changes, set `OPENWHISPER_CPP_TEST_RUNTIME_DIR`, `OPENWHISPER_CPP_TEST_MODEL`, `OPENWHISPER_CPP_TEST_WAV`; cache the NeMo CPU runtime and the Q8 model keyed on their pinned SHA-256 (a few hundred MB).
3. `release-build.yml` (manual `release_tag`): `source` resolves one commit, `qualification` calls `ci.yml`, then `windows`, `linux`, `macos` build the installers (the same steps as section 5, minus Python), `bundle` writes `BUILD-METADATA.json` (restart at schema 1) and `SHA256SUMS.txt`. Windows job: install Inno Setup (pin 6.4.x), configure signing from secrets, `scripts/build-installer.ps1`, signature verification, smoke test upgrade-from-previous-release and uninstall on the runner. Linux job: deb + Arch, container smoke tests on Ubuntu 22.04, Debian 12 and Arch. macOS job: DMG, mount-and-run checks, optional notarization.
4. `qualify-release.yml`: port as is (the Python script is generic; rename artifact patterns).
5. Weekly: re-verify component URLs and digests, run Dependabot or Renovate for GitHub Actions and the vcpkg baseline, vcpkg license collection, SBOM export (`vcpkg` SPDX).
6. Self-test and health gate: implement `openwhisper-cpp-cli --self-test` (loads Qt offscreen, finds required resources and plugins, opens SQLite, resolves the runtime path, optionally decodes a bundled 1 s WAV if a runtime is present) and a C++ health harness mirroring the Python gate (exit within 45 s, startup, stop-to-result p95, cancel, restart, worker cleanup, RSS bounds, DB round-trip, backup and restore). Use a fake `Recognizer` for determinism, as the Python gate does, and run the real engine in the nightly job.

Not needed: `uv lock`, ruff, mypy, `pip check`, ASGI test-client compatibility, Python version matrix, the PyInstaller bundle checks (forbidden packages, PyAV DLL presence, `_internal` assets).

Retained only if the features stay: the `webui` job (npm, vitest, stale-bundle diff), the `sidecar` job, and the Windows OpenCode component build.

### 6.8 Size of the proposal

| Item | Language | Approx. lines |
|---|---|---|
| `openwhisper-cpp-updater.exe` + shared `fv_update` library (manifest, journal, mutexes, registry/ARP, tar.xz verify and extract, data snapshot, rollback, recovery) | C++ | 2,600 (OpenWhisper: about 3,450 Python lines across `app_update_apply.py`, `setup_update.py`, `update_data.py`, `updater_helper.py`) |
| `fv-pack-update` packer and self-verifier | C++ | 450 |
| `--self-test`, `--diagnostics` plumbing and release-health harness | C++ | 450 |
| Windows glue shared with the app (named mutexes, health acknowledgement, startup recovery gate) | C++ | 300 |
| **C++ implied by this proposal** | | **about 3,800** (about 900 if only phase 1 is built) |
| CMake install, deploy, notice generation, package targets | CMake | about 350 |
| `OpenWhisper_CPP.iss` | Inno | about 220 (about 300 with the helper code) |
| `build-installer.ps1`, `package-linux.sh`, `package-macos.sh`, smoke scripts, signing script | PowerShell / shell / Python | about 1,500 |
| Workflows | YAML | about 900 |

The in-app update client (release check, parsing, download UI) is counted in area a16, and the component installer in area a14.

## Data and formats

- Windows data root `%LOCALAPPDATA%\OpenWhisper` (`OPENWHISPER_DATA_DIR` overrides); macOS `~/Library/Application Support/OpenWhisper`; Linux `$XDG_DATA_HOME` or `~/.local/share/OpenWhisper`. Components in `<root>\components\<id>\` with `manifest.json` and `.installed`; downloads in `components\cache`, staging in `components\staging`; updates in `<root>\updates\{tx\<id>\{journal.json,healthy,data-snapshot.json,data\},apply_error.txt}`.
- `openwhisper_settings.json` and `openwhisper.db` are the two files snapshotted before an update.
- `BUILD-METADATA.json` schema 3, `windows-signing.json` schema 1, `release-health.json`, `payload.json` and `.catalog.json` (OpenCode), `SHA256SUMS.txt` line format `<64 hex>  <name>` (two spaces).

## Environment variables

`OPENWHISPER_PYTHON`, `OPENWHISPER_SIGN_CERT_SHA1`, `OPENWHISPER_SIGN_PFX`, `OPENWHISPER_SIGN_PASS`, `OPENWHISPER_SIGN_DLIB`, `OPENWHISPER_SIGN_METADATA`, `OPENWHISPER_SIGN_TIMESTAMP_URL`, `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`, `OPENWHISPER_MACOS_CODESIGN_IDENTITY`, `OPENWHISPER_MACOS_NOTARY_PROFILE`, `OPENWHISPER_MACOS_NOTARY_KEY`, `OPENWHISPER_MACOS_NOTARY_KEY_ID`, `OPENWHISPER_MACOS_NOTARY_ISSUER`, `SOURCE_DATE_EPOCH`, `OPENWHISPER_UPDATE_FEED_URL`, `OPENWHISPER_DATA_DIR`, `OPENWHISPER_SPEAKER_MODEL`, `OPENWHISPER_TEST_FROZEN_HELPER`, `OPENWHISPER_UPDATER_RELAUNCHED`, `MEETING_AGENT_RELEASE_TAG`, `OPENWHISPER_ATTACHED`, `OPENWHISPER_MACOS_LAUNCH_APP`.

## Platform-specific code in this area

Windows only: Inno script, SignTool, named mutexes, `HKCU`/`HKLM` Uninstall key, `RunOnce`, reparse-point checks, `taskkill`. Linux only: deb/pacman packaging, `gdbus` raise-existing launcher, glibc floor, rpath stripping. macOS only: Info.plist, entitlements, notarization, `renamex_np` swap (a16), `Python.app` launcher. Everything in sections 3 and 4 except `Global\` mutexes and the registry is portable in design.

## Engine-specific (not ported)

Faster-whisper, CTranslate2, tokenizers, torch, `gpu-accel`, the embedded Python zip in NeMo components, `build_windows_runtime.py`, `build_macos_runtime.py`, `build_mlx_runtime.py`, Transcribe/Parakeet Unified, Moonshine, Qwen3-ASR and MLX runtime manifests, the PyInstaller specs, the `--isolated-worker` mode, `services/native_libraries.py` CUDA preloading, and the Qt DLL and ICU workarounds that exist only because of the PyQt6 wheels.

## Dependencies on other areas

a14 (component catalog, verified downloader, runtime manifests), a16 (in-app update client and UI, macOS DMG apply, Linux preflight), a10 (settings paths), a20 (agent API and MCP servers, FastAPI replacement), a24 and a27 (meeting web server, sidecars, Node/Bun), a17 (remote engine TLS and WebSocket, host-update D-Bus), a01 (`--self-test`, `--version`, `--diagnostics`, startup recovery gate).

## Tests in the source project that cover this area

`tests/test_windows_signing.py`, `test_release_qualification.py`, `test_app_update*.py`, `test_updater_helper.py`, `test_frozen_updater.py` (needs the frozen helper), `test_update_startup_gate.py`, `test_update_resilience.py`, `test_release_health.py`, `test_main_entrypoint.py`, `test_opencode_sidecar.py` (needs a built payload), plus the lifecycle scripts `smoke_windows_setup.ps1` and `smoke_arch_package.sh`. These are the porting oracles for the manifest, journal and signing logic; the manifest, journal and report formats above can be replayed as golden vectors.

## Porting notes

1. A fresh project does not need most of the update-compat machinery: setup bridge, `minimum_updater_version`, legacy NVIDIA directory, setup-time backup protocol. Ship phase 1 (setup-exe update) first.
2. Inno will not remove files a newer build no longer ships; plan an `[InstallDelete]` list or the helper from day one, and test it with a planted stale DLL as the Python smoke test does.
3. Portions of the freeze checks have direct C++ equivalents worth keeping: required-asset list, forbidden-dependency list (reframed: no Python, no torch, no MSVC debug CRT), ELF `ldd` sweep, Mach-O arch/minos/link audit, version-string check from the packaged binary, self-test and health gate on the packaged artifact (and on the app copied out of the DMG), and the install-over-previous-release-then-uninstall smoke.
4. SmartScreen and antivirus behave differently for a native unsigned exe than for PyInstaller's; sign from the start.
5. Keep GitHub as the only network source, keep the digest and exact-URL rules, keep the "never follow a moving ref" rule for models.
6. Generate notices instead of maintaining them: `vcpkg` copyright files, Qt's own license set, the NeMo runtime's `share/licenses`, Tabler, and model attributions (CC-BY-4.0 requires attribution; Orukeet is ShareAlike, so not bundled).

## Open questions for the project owner

1. Product naming and identity for OpenWhisper_CPP: Inno `AppId`, publisher string, macOS bundle id, data folder name, update repository, website URL, and whether to keep a short command alias like `ow` (Linux packages symlink it; Windows never put it on PATH).
2. Bundle the NeMo CPU runtime in the installer, or keep it a first-run download like OpenWhisper (the CPU archive is only about 4 MB on Windows)?
3. Native in-app update on Windows in release one, or setup-exe update first and the transactional helper later?
4. Is the meeting dashboard staying a React web page served by a local HTTP server (needs Qt HttpServer or Boost.Beast, an npm build in CI, and WebSockets), or should it become native Qt widgets?
5. Windows signing: which method (Azure Trusted Signing, OV certificate), and is an unsigned first release acceptable? macOS: Developer ID and notarization from the first release, and Intel support?
