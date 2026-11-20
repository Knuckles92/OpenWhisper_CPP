# Development environment

How to set up a machine and build, run, test and debug FioriVoce day to day,
and how the build is organised. README.md covers what the project is and how to
get a speech runtime and model; `docs/port/ARCHITECTURE.md` the port's design.

## One-time setup

**Windows** (PowerShell, from the repository root):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup-windows.ps1
```

**WSL / Linux** (Ubuntu or Debian):

```bash
bash scripts/setup-wsl.sh
```

Both scripts are safe to re-run. They install or check the compiler, CMake,
Ninja and a debugger, clone [vcpkg](https://vcpkg.io) (`C:\vcpkg` on Windows,
`~/vcpkg` on Linux) and set `VCPKG_ROOT`. The Windows script also records where
Qt is (see Qt below). Open a new terminal afterwards.

The tree needs CMake 3.25 or later and a C++23 compiler (`std::expected`):
Visual Studio 2022 or later (`/std:c++latest`), GCC 13 or Clang 17.

## Build, test, run

One command configures, builds and runs the tests:

| Where | Command |
| --- | --- |
| Windows | `cmake --workflow --preset windows-debug` |
| WSL / Linux | `cmake --workflow --preset linux-debug` |
| Linux with ASan + UBSan (Clang) | `cmake --workflow --preset linux-asan` |

Swap `debug` for `release` to get an optimized build with debug info.

On Windows the compiler has to be on `PATH` first. VS Code's terminal does this
for you (the "MSVC Dev Shell" profile). In any other PowerShell window, run this
once per session:

```powershell
. .\scripts\vsenv.ps1
```

The individual steps, when you want just one:

```bash
cmake --preset windows                      # configure (once, or after CMake changes)
cmake --build --preset windows-debug        # build
ctest --preset windows-debug                # test
out/build/windows/bin/Debug/fiorivoce-cli --help
out/build/windows/bin/Debug/fv_engine_tests "name of one test"
```

Use `linux` in place of `windows` under WSL. Each preset builds into its own
folder, `out/build/<preset>`, so Windows and Linux builds of the same checkout
never collide.

### Every preset

| Workflow | What it builds and runs |
| --- | --- |
| `windows-debug`, `windows-release` | Everything, with Qt when found |
| `windows-ci` | Everything (Qt required) in RelWithDebInfo, warnings as errors on FioriVoce's targets, every test including the lints (Python 3.12 required) and the screenshot comparisons. A package passes this before it merges. |
| `windows-core` | The Qt-free modules and their tests only (`FIORIVOCE_BUILD_APP=OFF`): the fastest loop for a core package |
| `linux-debug`, `linux-release` | Everything that builds on Linux |
| `linux-core` | Every `core` module and its tests with GCC, no Qt |
| `linux-asan` | As `linux-debug`, under ASan and UBSan with Clang |
| `parity` | Builds as `windows-ci`, then runs only the `golden` and `screenshot` tests with the pinned Qt environment |

The `default` preset (`cmake --preset default`, building into `build/`) needs no
vcpkg and no setup script: it uses the platform's default generator, fetches
Catch2 itself and builds only the engine, `fiorivoce-cli` and their tests
(`FIORIVOCE_ENGINE_ONLY`).

### A package's inner loop

```bash
cmake --build --preset windows-debug --target fv_<dir>_tests
ctest --preset windows-debug -L <dir>
```

`-L` also takes a kind: `ctest --preset windows-debug -L decode` runs only the
tests that decode real audio, `-L lint` the lints, `-L cmake` the tests of the
build system itself.

The decode tests are skipped unless `FIORIVOCE_TEST_RUNTIME_DIR`,
`FIORIVOCE_TEST_MODEL` and `FIORIVOCE_TEST_WAV` are set (README.md). The PDF text tests
need `FIORIVOCE_TEST_PDFIUM`, and the speaker embedding tests need
`FIORIVOCE_TEST_ONNXRUNTIME` and `FIORIVOCE_TEST_SPEAKER_MODEL`; without them those tests
skip.

## How the tree is organised

Each directory under `src/` is one library, `fv_<dir>`, and each directory under
`tests/` one Catch2 executable, `fv_<dir>_tests`. The top-level `CMakeLists.txt`
adds every `src/*/CMakeLists.txt` and `tests/*/CMakeLists.txt` it finds, in
sorted order, and re-runs CMake by itself when one appears or disappears.

### Module kinds and what they may link

| Kind | Qt | Third-party | May depend on |
| --- | --- | --- | --- |
| `core` | none | the ports named for it in the architecture | `core` |
| `qt` | Core, Gui (Widgets only in `appkit`) | as named | `core`, `qt` |
| `ui` | Core, Gui, Widgets, Svg, SvgWidgets (Network only in `ui_shell`) | none | anything but `exe` |
| `feature` | as `ui`, plus a feature registered with `fv_feature` | none | anything but `exe` |
| `exe` | as needed | as needed | anything |

Configure fails when a module breaks these rules, and the `lint.layering` test
fails when a file includes a header from outside its module's `DEPS` closure,
another module's `detail/` header, or (in a `core` module) a Qt header.

### Adding a module

Create `src/<dir>/CMakeLists.txt` with one call and put the sources beside it:

```cmake
fv_module(KIND core
          DEPS fv_base fv_pyport
          THIRD_PARTY PCRE2::8BIT          # linked publicly: its headers may appear in yours
          PRIVATE_THIRD_PARTY CURL::libcurl # linked privately: only your .cpp files see it
          WIN32_LIBS user32)               # Windows only
```

`fv_module` globs `*.cpp`, `*.h`, `*.inc`, `detail/`, `testing/`, and `win/` on
Windows or `posix/` elsewhere. A directory with headers only becomes an
`INTERFACE` library, which is how the section 3 contracts exist before their
implementations; its first `.cpp` turns it into a static library. Qt kinds also
get AUTOMOC and compile `resources/` into `:/<dir>/`. `QT <components>` adds
the named exceptions (`Widgets` for `appkit`, `Network` for `ui_shell`). An
executable is `fv_module(KIND exe NAME fiorivoce GUI FEATURES DEPS ...)`: `GUI`
makes it a Windows GUI program, `FEATURES` links the generated feature
registry. Third-party targets come from `cmake/FioriVoceDeps.cmake`, which holds
every `find_package`.

### Adding a test

Create `tests/<dir>/CMakeLists.txt` with `fv_test()` and put `test_*.cpp` beside
it. `fv_test` links `fv_<dir>`, Catch2's main and the `DEPS` you name, and
registers every test case with CTest under the label `<dir>` and its kind, taken
from the test's Catch2 tag: `[decode]`, `[platform]`, `[golden]`, `[widget]`,
`[screenshot]`, or `unit` when it has none. `QT <components>` builds a Qt test;
`NO_MAIN` links Catch2 without its main (for a custom one). CTest gives every
test case its own `FIORIVOCE_DATA_DIR` (a folder under the test directory's
build folder, so `ctest -j` never shares one between cases) and, for Qt tests,
`QT_QPA_PLATFORM=offscreen`.

Run widget tests through `ctest`, never by starting the executable directly:
started bare, a failure to find the display plugin puts a dialog on the screen.

### Adding a feature

In a `KIND feature` module, after `fv_module`:

```cmake
fv_feature(ID meeting CLASS fv::MeetingFeature)   # HEADER defaults to <dir>/feature.h
```

At the end of configure CMake writes `generated/fv_feature_registry/registry.cpp`,
which lists every feature's factory sorted by id (`fv::registered_features()` in
`appkit/feature.h`); an executable with `FEATURES` links it. No static
registrars and no whole-archive linking.

### Embedding data

`fv_embed(data/catalog.json)` after `fv_module` makes the header
`"<dir>/data/catalog.json.h"` (in the build tree) with
`fv::embedded::<dir>_catalog`, a `constexpr std::string_view` of the file's
bytes, regenerated when the file changes.

### Frozen files

The lead edits these, at milestone boundaries only: the top-level
`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/*`,
`src/settings/keys.inc`, `src/appkit/order.h`, `src/appkit/destinations.h`,
`src/ui_theme/style_props.h`, `src/ui_theme/resources/theme.qss`, `CLAUDE.md`
and `BUILDING.md`. A package changes only the directories it owns;
`lint.ownership` checks that when `FIORIVOCE_OWNS` names them (for example
`FIORIVOCE_OWNS="src/settings/;tools/gen/settings_keys.py"`).

### Lints

The lints are Python 3.12 standard-library scripts in `tools/lint`, run as CTest
tests named `lint.<check>`: `layering`, `ownership`, `qss_names`, `brand`
(no string literal names OpenWhisper outside the identity and import code) and
`style_props` (widget code sets dynamic properties only through
`setStyleProperty`). Each script also runs alone:
`python tools/lint/check_layering.py`.

### Build speed

- Two shared precompiled headers (`cmake/pch/core.h`, `cmake/pch/qt.h`) are
  compiled once per set of compile definitions and reused by every target
  (`FIORIVOCE_PCH`, on by default).
- Qt's DLLs and plugins are copied into `bin/` once, by the `fv_qt_runtime`
  target, rather than by `windeployqt` after each executable.
- `sccache` is used when it is on `PATH`; nothing depends on it.

## Qt (the desktop app)

The Qt modules, the desktop app (`fiorivoce`) and their tests are built only
when CMake finds Qt 6.8 or later (Core, Gui, Widgets, Svg, SvgWidgets, Network,
Test); without it, every `core` module and its tests still build. Qt is a
prebuilt install rather than a vcpkg dependency, because vcpkg would compile all
of Qt from source.

- **Windows**: `setup-windows.ps1` looks for a Qt MSVC kit under `C:\Qt` or
  `D:\Qt` and records it in the `Qt6_ROOT` environment variable, which every
  preset picks up. Pass `-QtRoot <kit folder>` for a kit somewhere else.
- **WSL / Linux**: not set up. Ubuntu 24.04 packages Qt 6.4, which is too old,
  so the Linux presets skip the app. To build it there, install a newer Qt
  (for example with `aqtinstall`) and export `Qt6_ROOT` to its `gcc_64` folder.

## VS Code

Open the folder and accept the recommended extensions (C/C++, CMake Tools). For
Linux builds, run **WSL: Reopen Folder in WSL** and install the same extensions
there when prompted.

- **Pick a preset**: CMake Tools asks on first open; choose `Windows (MSVC x64)`
  or `Linux (GCC)`. Change it later from the status bar.
- **Build**: `F7`. **Run tests**: the Testing panel, or `CMake: Run Tests`.
- **Debug**: choose the target in the status bar (`fiorivoce`, `fiorivoce-cli`
  or an `fv_<dir>_tests`), then `F5`. Program arguments go in `args` in
  `.vscode/launch.json`.
- Files format on save with `.clang-format`; clang-tidy warnings from
  `.clang-tidy` show up as squiggles.

## Dependencies

Every third-party library the port uses is in `vcpkg.json`, pinned by its
`builtin-baseline` (a vcpkg commit); packages do not add any. vcpkg builds each
port once per machine and keeps the result in its default per-user binary cache
(`%LOCALAPPDATA%\vcpkg\archives` on Windows), so a new worktree or a fresh build
folder restores FFmpeg, Boost and OpenSSL from archives instead of compiling
them. The first build of all the ports on a machine takes about a quarter of an
hour.

To move every dependency forward, update your vcpkg clone (`git pull`) and run
`vcpkg x-update-baseline` in the repository root; that is a lead change.

## Lint from the command line (WSL)

```bash
clang-tidy -p out/build/linux src/engine/wav.cpp
clang-format -i src/engine/wav.cpp
```

On Windows, clang-format runs in WSL:
`wsl clang-format --dry-run -Werror /mnt/d/<path to the checkout>/src/<file>`.

## What lives where

| File | Purpose |
| --- | --- |
| `CMakeLists.txt` | Finds the modules and tests; options |
| `cmake/FioriVoceModules.cmake` | `fv_module`, `fv_test`, `fv_feature`, `fv_embed`, the layering check, the feature registry |
| `cmake/FioriVoceDeps.cmake` | Every `find_package` |
| `CMakePresets.json` | Every supported way to configure, build and test |
| `vcpkg.json` | Third-party libraries and the pinned versions |
| `tools/lint/` | The lints and their tests |
| `.clang-format`, `.clang-tidy` | Formatting and lint rules |
| `.vscode/` | Shared editor settings, debug configurations, extension list |
| `scripts/` | Machine setup, and `vsenv.ps1` to load MSVC into a terminal |
