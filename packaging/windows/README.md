# Windows installer

FioriVoce ships on Windows as one Inno Setup 6 setup, `FioriVoce-Setup-<version>.exe`, that
installs per user (no UAC prompt) into `%LOCALAPPDATA%\Programs\FioriVoce`. User data stays in
`%LOCALAPPDATA%\FioriVoce` (or `FIORIVOCE_DATA_DIR`); the setup never writes there, and the
uninstaller removes only its `updates` folder unless the user says yes to deleting the rest
(a silent uninstall says no). In the install folder the uninstaller also removes every name
`[InstallDelete]` manages (`[UninstallDelete]` lists the same entries), because after a native
update the tree holds files no setup logged (the `.fiorivoce-update-complete` sentinel, a DLL a
later version added); with them gone Inno removes the emptied folder too, as OpenWhisper's
uninstaller does.

`[InstallDelete]` stays, a recorded decision against M10.17's plan (change 8) to drop it for the
setup helper: the helper's `prepare` only copies the managed files aside for a rollback and never
deletes a retired one, and when setup runs elevated (`IsAdmin`) no helper runs at all, so the
section is what clears a DLL or plug-in folder an older version shipped, in every install. The
comment above it in `FioriVoce.iss` says the same.

| File | What it does |
|---|---|
| `build-installer.ps1` | The single entry point: build, stage, check, sign, compile the setup, metadata |
| `stage.ps1` | The install tree from a build tree: programs, vcpkg DLLs, `windeployqt`, C++ runtime, notices |
| `FioriVoce.iss` | The Inno Setup script |
| `inno_defines.py` | The script's `/D` defines from `src/update/data/release_identity.json` |
| `smoke-setup.ps1` | Silent install, upgrade over it, uninstall, with checks (installs software) |
| `smoke-update.ps1` | The owner's real self-update: install, native update, a rollback, uninstall (installs software; written, never run) |
| `packaging-common.ps1` | Helpers the scripts share (an import-table reader, the Python lookup) |

The AppId, the publisher, the folder names, the executable names, the named mutexes
(`AppMutex`, `SetupMutex`) and the update switch (`/FIORIVOCEUPDATE=1`) all come from the
release identity, the file the application embeds, so the setup sees a running app and the
app's launch gate sees a running setup. The AppId never changes once a setup has been
installed anywhere.

## Prerequisites

- Visual Studio 2022 (or later) with the C++ tools, vcpkg and the Qt 6.10.3 MSVC kit, as for
  any build (`scripts\setup-windows.ps1`, BUILDING.md).
- Python 3.12 (the release tools under `tools\release`). `FIORIVOCE_PYTHON` names another
  `python.exe`; otherwise the `py -3.12` launcher, then `python` on `PATH`.
- Inno Setup 6: `winget install -e --id JRSoftware.InnoSetup`. `ISCC.exe` is found through
  `FIORIVOCE_ISCC`, then `PATH`, then `%LOCALAPPDATA%\Programs\Inno Setup 6`, then
  `%ProgramFiles(x86)%\Inno Setup 6`.
- Code signing is optional and off by default: `tools\release\README.md` lists the
  `FIORIVOCE_SIGN_*` variables. Unsigned, every signing step says so and passes.

## Building

```powershell
.\packaging\windows\build-installer.ps1 [-Version X.Y.Z] [-Clean] [-SkipInstaller]
```

1. Configures the `windows` preset with `-DFIORIVOCE_VERSION=<version>` (default: the version
   in `CMakeLists.txt`, passed every time so an earlier `-Version` does not linger in the
   cache) and builds `fiorivoce`, `fiorivoce-cli` and `fiorivoce-updater` with the `windows-release` build preset
   (RelWithDebInfo). `-Clean` deletes `out\stage` and `out\installer` and rebuilds from scratch.
2. `stage.ps1` stages the three programs in `out\stage` and collects every DLL beside them in the build tree
   that they import (followed through the import tables), then `windeployqt --release
   --no-translations --no-compiler-runtime --include-plugins qoffscreen` for Qt (including
   the offscreen package check and `opengl32sw.dll`), the app-local C++ runtime from `VCToolsRedistDir`, the meeting dashboard bundle
  (`webui\dist`, copied from the source tree), and
   `generate_notices.py` (`THIRD_PARTY_NOTICES.md`, `licenses\`).
3. Checks the stage: `fiorivoce-cli --version` names the version, `fiorivoce-cli --self-test
   --installed` finds every file the programs import plus the Qt platform, `windows11` style,
   SVG and Schannel TLS plugins. `fiorivoce --self-test-gui` exercises the staged Qt plugins offscreen. `inno_defines.py --check-stage` finds an `[InstallDelete]`
   entry for every top-level file and folder (a folder windeployqt starts deploying has to be
   added there, or an upgrade past the version that drops it would leave it behind), and
   `check_release_health.py` runs `--workflow-smoke` from the stage (in a temporary folder of
   its own) into `out\release-health.json`.
4. `windows_signing.py sign` (a visible no-op when unsigned), `ISCC.exe` with the identity's
   defines and `windows_signing.py inno-args`, followed by `windows_signing.py verify`.
   The native updater packs and verifies the update ZIP before `build_metadata.py` records both artifacts.
   Packaging another version moves the prior setup and ZIP to `out\installer-previous`.

The staged `fiorivoce-updater.exe` can pack and verify a native release archive locally.
Give it a new archive path outside the staging directory.

```powershell
out\stage\fiorivoce-updater.exe --pack-archive out\stage --version 0.1.0 --out out\FioriVoce-0.1.0-win64.zip
out\stage\fiorivoce-updater.exe --verify-archive out\FioriVoce-0.1.0-win64.zip --version 0.1.0
```

Eligible per-user installations can apply the native archive. The staged helper verifies the
payload, waits for the app to exit, snapshots settings and SQLite, swaps the installation
directories and waits for the new app's health acknowledgement. An interrupted or failed update
can restore the earlier release. The desktop offers Inno Setup when an installation needs it.

The candidate in `out\installer` contains `FioriVoce-Setup-<version>.exe`,
`FioriVoce-<version>-win64.zip`, `BUILD-METADATA.json`, `SHA256SUMS.txt` and
`windows-signing.json`.

Without Inno Setup the script still builds, stages and checks everything, says the installer
step was skipped and exits 1; with `-SkipInstaller` it stops there and exits 0.

From Windows PowerShell 5.1, quote a version passed to CMake by hand
(`cmake --preset windows "-DFIORIVOCE_VERSION=0.1.1"`): unquoted, the shell splits it at the
first dot. The script passes it as one argument. After a `-Version 0.1.1` build the `windows`
tree's cache holds 0.1.1 until the script runs again without `-Version` or CMake is configured
with the default.

`-Preset windows-ci` stages the merge gate's RelWithDebInfo tree instead (`out\build\windows-ci`),
which saves a second build tree on a machine that already has one.

## Upgrade smoke test

```powershell
.\packaging\windows\smoke-setup.ps1 -PreviousSetup <older setup> -NewSetup <newer setup> `
    -Version <newer version> [-OnThisComputer]
```

Installs the older setup silently into a new temporary folder (`/DIR`, `/CURRENTUSER`), plants a
retired DLL in it and a file in `%LOCALAPPDATA%\FioriVoce`, installs the newer setup over it,
and checks that the DLL is gone (`[InstallDelete]`), the user file is unchanged, the uninstall
entry's `DisplayVersion` is the new version and `fiorivoce-cli --self-test --installed` passes;
then uninstalls silently and checks the programs are gone and the user file kept. Logs go to
`%TEMP%\fiorivoce-setup-logs` (`$RUNNER_TEMP` on CI).

It installs software, so it refuses to run outside GitHub Actions without `-OnThisComputer`,
and refuses whenever FioriVoce is already installed for this user or the machine (the uninstall
entry is shared with a real install). It never touches `%LOCALAPPDATA%\Programs\FioriVoce`, and
it uninstalls what it installed and removes the file it planted even when a check fails.

Close FioriVoce and any running development copy before this test. Setup checks the application
and update mutexes and asks for a retry while either is held. The silent installs create the
desktop shortcut (the task is
ticked by default) and the uninstall removes it again, and the uninstall also empties
`%LOCALAPPDATA%\FioriVoce\updates`, as every uninstall does.

## Self-update check (written, never run)

`smoke-update.ps1` was written for M10.17 on a machine without Windows and **has never been run**;
expect to fix it on its first run. It is the owner's check of the real native update, with the
desktop: FioriVoce shows its window and each update starts from its "Download and install".

```powershell
# After each of build-installer.ps1 -Version 0.1.0, -Version 0.1.1 and -Version 0.1.2, copy
# out\installer\FioriVoce-Setup-<v>.exe and FioriVoce-<v>-win64.zip into one folder.
.\packaging\windows\smoke-update.ps1 -Artifacts <that folder> -OnThisComputer `
    [-From 0.1.0] [-To 0.1.1] [-Failing 0.1.2] [-SkipRollback]
```

1. Installs `<From>` silently into a new temporary folder (`/DIR`, `/CURRENTUSER`) and starts it
   with `LOCALAPPDATA` set to a temporary folder as well, so the app, its updater and the health
   launch keep their settings, history and `updates` work folder there, not in the user's
   `%LOCALAPPDATA%\FioriVoce` (`FIORIVOCE_DATA_DIR` is cleared: a data root that is not the
   default makes the install ineligible for a native update).
2. Serves `<To>`'s setup and archive with `tools\release\serve_update_feed.py` on 127.0.0.1 and
   points the app at it (`FIORIVOCE_UPDATE_FEED_URL`). Choose "Update to `<To>`", then "Download
   and install". It waits for the tree's `.fiorivoce-update.json` and the uninstall entry's
   `DisplayVersion` to say `<To>`, the RunOnce recovery value to be gone, the transaction folder
   to be removed, no `apply_error.txt`, and the updated app to be running.
3. Quit FioriVoce from the tray when asked. It serves `<Failing>` and starts `<To>` again with
   `FIORIVOCE_TEST_FAIL_HEALTH=1`, which reaches `<Failing>`'s health launch through the updater;
   choose "Download and install" again. It waits for `apply_error.txt` (copied to the logs), then
   for the tree, the uninstall entry and the running app to be `<To>` again. It refuses to start
   when `<Failing>`'s `fiorivoce.exe` does not contain that switch's name: the switch is M10.12's
   (`docs/port/packages/M10.md`) and, without it, step 3 would install `<Failing>` for good.
   `-SkipRollback` leaves this step out.
4. Quit FioriVoce again. It uninstalls silently and checks that the uninstall entry and the whole
   install folder are gone (the `[UninstallDelete]` entries above).

It refuses without `-OnThisComputer`, on a CI runner, in an elevated shell, and whenever
FioriVoce is installed, running or has an update recovery pending. It touches only its temporary
folder (`%TEMP%\FioriVoce-update-smoke-<id>`, deleted at the end), the setup's uninstall entry
under HKCU, and the updater's own RunOnce value, which it removes at the end only when the value
names that folder; as every uninstall does, the uninstall also empties
`%LOCALAPPDATA%\FioriVoce\updates`. Logs, the feed's output and `apply_error.txt` go to
`%TEMP%\fiorivoce-update-logs`.

## Continuous integration

`.github/workflows/ci.yml` (Windows `windows-ci` workflow preset with the Qt 6.10.3 kit from
aqtinstall and vcpkg at the manifest's baseline with a `files` binary cache; Linux
`linux-core`; the lints, the parity ledger, the release tools' tests and clang-format 18) and
`.github/workflows/release-build.yml` (resolve one commit, run `ci.yml` on it, build a 0.0.0
setup and the candidate, smoke the upgrade, qualify the candidate and keep it as workflow
artifacts). Both start only by hand (`workflow_dispatch`); `ci.yml` is also called by
`release-build.yml`. Neither uploads to a release, pushes or tags.

`.github/workflows/qualify-release.yml` (M10.17; written, never run) takes a kept candidate to
an existing draft release: started by hand with `candidate_run_id`, `release_tag` and
`require_windows_signed`, its `qualify` job checks that the run is a successful hand-started
`release-build.yml` run of this repository and that the candidate passes
`check_release_qualification.py` for the tag's commit and that run; its `upload` job repeats
the check on the bytes it uploads, checks that the release is a draft tagged `v<version>` at that
commit and holds nothing the candidate does not replace, runs `gh release upload --clobber` and
validates the draft's assets afterwards (`--draft-assets ... --uploaded`). It never publishes the
draft, pushes or tags. Every job is skipped unless the repository variable
`FIORIVOCE_RELEASE_REPO` is set, and it must name the repository the workflow runs in.
`tools/release/tests/test_workflows.py` allows `gh release upload` there and nowhere else.

## Milestone demo (M3)

The owner runs this: it installs software, starts FioriVoce and takes over the desktop. Run the
upgrade smoke test (above) first, if wanted: it refuses once FioriVoce is installed.

1. Build and install 0.1.0:

   ```powershell
   powershell -File packaging\windows\build-installer.ps1 -Clean
   ```

   This builds RelWithDebInfo, stages, writes the notices, runs `fiorivoce-cli --self-test
   --installed` on the stage and compiles `out\installer\FioriVoce-Setup-0.1.0.exe` (unsigned:
   no signing variables are set). Run the setup: it installs into
   `%LOCALAPPDATA%\Programs\FioriVoce` without UAC, adds the Start menu entry and starts
   FioriVoce.
2. Import: on the first run the guided setup's Welcome page offers "Bring over your OpenWhisper
   data" because `%LOCALAPPDATA%\OpenWhisper` exists. The import dialog lists what it found;
   with OpenWhisper running it says to close it; otherwise "Import and restart" stages the
   data, copies the API keys when ticked, adopts the runtime and model, restarts with exit code
   73 and applies the stage. History shows OpenWhisper's entries, Settings its choices, the keys
   are in Credential Manager under `FioriVoce`, and `fiorivoce_settings.json` records
   `fv_import`.
3. Backup: Settings > Backup creates `FioriVoce-backup.fvbackup`, inspects it, and "Restore and
   restart" brings it back; the previous data stays in `.fiorivoce-restore\previous-*` and is
   shown on the page. A daily schedule saves `FioriVoce-auto-*.fvbackup` files and the calendar
   shows them.
4. Update to 0.1.1:

   ```powershell
   powershell -File packaging\windows\build-installer.ps1 -Version 0.1.1
   py -3.12 tools\release\serve_update_feed.py --setup out\installer\FioriVoce-Setup-0.1.1.exe `
       --archive out\installer\FioriVoce-0.1.1-win64.zip --version 0.1.1
   ```

   The first command moves the previous version's setup and ZIP to `out\installer-previous`.
   Use the printed loopback feed URL to start the installed 0.1.0 with
   `$env:FIORIVOCE_UPDATE_FEED_URL = 'http://127.0.0.1:<port>/releases/latest'` set in that
   shell (`& "$env:LOCALAPPDATA\Programs\FioriVoce\fiorivoce.exe"`). The title bar and the tray
   show "Update to 0.1.1". On an eligible per-user installation, "Download and install"
   verifies the archive and hands it to the native updater. The helper waits for the old
   application to exit, applies the update and waits for the new version's health check.
   Help > About should then report that 0.1.1 is up to date. To exercise the setup fallback,
   repeat on a separate disposable installation with only the setup in the feed. Stop the feed
   with Ctrl+C.

Afterwards, uninstall from Settings > Apps (or `unins000.exe` in the install folder) and answer
the data question as wanted.
