# FioriVoce

C++ rewrite of OpenWhisper (https://github.com/Knuckles92/OpenWhisper, Python
and PyQt6), keeping only the Parakeet engine on NVIDIA NeMo-Speech.cpp. A
separate project, not a fork: OpenWhisper is the behaviour reference, so read
its code for how a feature works before porting it, and say where FioriVoce
deliberately differs. The port's design is `docs/port/ARCHITECTURE.md`.

## Commands

On Windows outside VS Code, run `. .\scripts\vsenv.ps1` first to put MSVC on
`PATH`. Presets build into `out/build/<preset>` (BUILDING.md).

- Everything: `cmake --workflow --preset windows-debug` (or `linux-debug`):
  configure, build, test.
- Inner loop: `cmake --build --preset windows-debug --target fv_<dir>_tests`,
  then `ctest --preset windows-debug -L <dir>`.
- One test: `out/build/windows/bin/Debug/fv_<dir>_tests "name of the test"`.
- Before merge: `cmake --workflow --preset windows-ci` (warnings as errors,
  lints). `windows-core` builds only the Qt-free modules and their tests.
- Without vcpkg: `cmake --preset default`, `cmake --build --preset release`,
  `ctest --preset release` build only the engine, the CLI and their tests.
- Format: `wsl clang-format -i <files>` (clang-format is in WSL, not Windows).

The tests that decode audio (label `decode`) are skipped unless
`FIORIVOCE_TEST_RUNTIME_DIR`, `FIORIVOCE_TEST_MODEL` and `FIORIVOCE_TEST_WAV`
are set (see README.md). Set them when changing anything under `src/engine` or
`src/speech`.

## Structure

- `src/<dir>` is one library `fv_<dir>`, declared by one `fv_module(KIND ...)`
  call in its `CMakeLists.txt`. Kinds: `core` (no Qt; may depend on `core`),
  `qt` (QtCore and QtGui; `core`, `qt`), `ui` and `feature` (widgets; anything
  but `exe`; no third-party), `exe`. Configure and `lint.layering` fail on a
  violation. Public headers at the top, `detail/` private, `win/` with a
  `posix/` stub, `testing/` for fakes. Includes as `"<dir>/name.h"`, namespace
  `fv`.
- `tests/<dir>` is one `fv_test()` call building `fv_<dir>_tests` (Catch2),
  labelled `<dir>` and by tag: `[decode]`, `[platform]`, `[golden]`,
  `[widget]`, `[screenshot]`, else `unit`.
- Adding a module, test, source file or feature edits no shared file: the
  root globs the directories, `fv_module` and `fv_test` glob the files,
  `fv_feature(ID ... CLASS ...)` feeds the generated registry, `fv_embed`
  embeds `data/*.json`. Frozen (lead only): root `CMakeLists.txt`,
  `CMakePresets.json`, `vcpkg.json`, `cmake/*`, `settings/keys.inc`,
  `appkit/order.h`, `appkit/destinations.h`, `ui_theme/style_props.h`,
  `ui_theme/resources/theme.qss`, `AGENTS.md`, `BUILDING.md`. Every third-party
  library is in `vcpkg.json` already; packages add none.
- `src/engine` is plain C++ with no Qt, so it builds and tests with a compiler
  alone. Keep Qt out of every `core` module.
- Qt code uses Qt naming (camelCase); Qt-free code keeps Python's names in
  snake_case. Every target compiles with `QT_NO_KEYWORDS`, because Qt-free
  headers use `emit`, `slots` and `signals` as names (`fv::Signal::emit`): Qt
  code writes `Q_SIGNALS:`, `public Q_SLOTS:` and `Q_EMIT`.
- `src/app` is only `main()` for `fiorivoce.exe`; the shell is `src/ui_shell`,
  and the engine bench lives on in `src/ui_shell/dev` as a development aid.
- Only the speech thread touches a `Recognizer`: `SpeechService`'s `fv-speech`
  (and `EngineWorker`'s own thread in the bench). Never call a `Recognizer`
  from the GUI thread.
- Run tests through `ctest`, never by starting a widget test executable:
  CTest sets the offscreen display plugin. Started bare, a failure to find it
  puts a dialog on the user's screen.
- A test that takes over the desktop session is tagged `[interactive]`: one
  that installs a keyboard or mouse hook, injects keys or clicks, writes the
  clipboard, reads or changes the foreground window, opens a real audio
  device, captures the screen or shows a tray icon. The default presets
  exclude that label, because the owner is working on this machine. Never
  run `ctest --preset windows-interactive` or such a test unasked; cover the
  logic with fakes and leave the `[interactive]` test for the owner.
- `third_party/nemo_speech/include/nemo_speech/asr.h` is the vendored C ABI
  header. Do not edit it.

## Engine rules

- The runtime is loaded at run time by path (`NemoApi::get`), never linked,
  because the user picks a CPU or GPU build and may have none installed. To
  call another `nemo_speech_asr_*` function, add it to `FV_NEMO_ASR_FUNCTIONS`
  in `src/engine/nemo_api.h`.
- Never unload the runtime. `FreeLibrary` on NeMo-Speech.cpp 0.1.0 crashes the
  process, so loaded runtimes stay loaded until exit.
- Call the runtime from one thread at a time, and read `last_error` on the
  thread that made the failing call.
- Pointers passed in option structs (language, speech contexts) must outlive
  the request.
- Parakeet TDT is offline-only: the runtime rejects `streaming_recognize` on
  it. Live preview means decoding windows with `Recognizer::transcribe`.
- Engine text is UTF-8 `std::string`; convert paths with `path_from_utf8` and
  `path_to_utf8`, never `path.string()`.
- Errors the user can see are `fv::EngineError` with a full sentence.

## Style

`.clang-format` at the root (LLVM base, 4 spaces, 100 columns). Comments say
what the code cannot: the reason, the constraint, the unit.
