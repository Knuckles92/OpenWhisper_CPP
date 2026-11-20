"""Static contracts of the Windows Inno Setup script and the defines it is compiled with.

The script takes every name it shares with the application from the release identity through
packaging/windows/inno_defines.py, so these tests read the script, run the defines over the
source tree's identity and, when Inno Setup is installed, let ISCC preprocess the two together.
Nothing here runs a setup.
"""

import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

import release_support as rs

import release_common as common

REPO = Path(__file__).resolve().parents[3]
PACKAGING = REPO / "packaging" / "windows"
INSTALLER = PACKAGING / "OpenWhisper_CPP.iss"
SOURCE_IDENTITY = REPO / "src" / "update" / "data" / "release_identity.json"
BASE_IDENTITY = REPO / "src" / "base" / "identity.cpp"

if str(PACKAGING) not in sys.path:
    sys.path.insert(0, str(PACKAGING))
import inno_defines  # noqa: E402


def section(script: str, name: str) -> list:
    """The non-comment lines of one [Section] of the script."""
    match = re.search(rf"^\[{re.escape(name)}\]\s*$(.*?)(?=^\[|\Z)", script, re.M | re.S)
    if not match:
        return []
    return [line.strip() for line in match.group(1).splitlines()
            if line.strip() and not line.strip().startswith(";")]


def entry_names(lines: list) -> list:
    return [re.search(r'Name: "([^"]+)"', line).group(1) for line in lines]


def app_identity_field(field: str) -> str:
    """A field of the application's AppIdentity, as base/identity.cpp spells it."""
    match = re.search(rf'\.{field} = "([^"]+)"', BASE_IDENTITY.read_text(encoding="utf-8"))
    assert match, f"{field} not found in {BASE_IDENTITY}"
    return match.group(1)


def find_iscc():
    named = os.environ.get("OPENWHISPER_CPP_ISCC")
    candidates = [named, shutil.which("ISCC.exe") or shutil.which("iscc")]
    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates.append(os.path.join(local, "Programs", "Inno Setup 6", "ISCC.exe"))
    x86 = os.environ.get("ProgramFiles(x86)")
    if x86:
        candidates.append(os.path.join(x86, "Inno Setup 6", "ISCC.exe"))
    return next((c for c in candidates if c and os.path.isfile(c)), None)


class InstallerScript(unittest.TestCase):
    def setUp(self):
        self.script = INSTALLER.read_text(encoding="utf-8")
        self.identity = json.loads(SOURCE_IDENTITY.read_text(encoding="utf-8"))

    def test_existing_install_uses_condensed_in_place_upgrade(self):
        script = self.script
        # The AppId is the identity's braced GUID: "{{" escapes the brace in [Setup], and the
        # uninstall key the [Code] probes is the same GUID with one brace and "_is1".
        self.assertIn('#define AppIdValue "{{" + AppGuid + "}"', script)
        self.assertIn("AppId={#AppIdValue}", script)
        self.assertIn('#define UninstallKeyName "{" + AppGuid + "}_is1"', script)
        self.assertIn("'{#UninstallKeyName}'", script)
        self.assertIn("function IsExistingInstall(): Boolean;", script)
        self.assertIn("RegKeyExists(HKEY_CURRENT_USER, UninstallKey)", script)
        self.assertIn("RegKeyExists(HKEY_LOCAL_MACHINE, UninstallKey)", script)
        self.assertIn("function ShouldSkipPage(PageID: Integer): Boolean;", script)
        for page in ("wpWelcome", "wpLicense", "wpSelectDir", "wpSelectProgramGroup",
                     "wpSelectTasks"):
            self.assertIn(f"PageID = {page}", script)
        self.assertNotIn("PageID = wpReady", script)

    def test_upgrade_keeps_user_data_outside_the_replaced_install_tree(self):
        deletes = section(self.script, "InstallDelete")
        names = entry_names(deletes)
        # Every name the install tree manages is cleared before the new files land.
        for managed in (r"{app}\*.dll", r"{app}\openwhisper-cpp*.exe", r"{app}\platforms",
                        r"{app}\styles", r"{app}\imageformats", r"{app}\iconengines",
                        r"{app}\tls", r"{app}\share"):
            self.assertIn(managed, names)
        for folder in ("platforms", "styles", "imageformats", "iconengines", "tls", "share"):
            self.assertIn(f'Type: filesandordirs; Name: "{{app}}\\{folder}"', deletes)
        # The managed executable pattern covers both programs of the identity.
        exe_patterns = [n.split("\\", 1)[1] for n in names if n.endswith(".exe")]
        for program in (self.identity["app_exe"], self.identity["cli_exe"]):
            self.assertTrue(any(fnmatch.fnmatch(program, p) for p in exe_patterns), program)
        for name in names:
            self.assertTrue(name.startswith("{app}\\"), name)
            leaf = name.rsplit("\\", 1)[1].lower()
            # Inno's own uninstaller (unins000.exe, unins000.dat) must survive an upgrade.
            self.assertFalse(fnmatch.fnmatch("unins000.exe", leaf), name)
            self.assertFalse(fnmatch.fnmatch("unins000.dat", leaf), name)
            self.assertNotIn("{localappdata}", name.lower())
            self.assertNotIn("{userappdata}", name.lower())
        self.assertNotIn('Name: "{localappdata}\\{#DataDirName}"', self.script)
        self.assertNotIn('Name: "{localappdata}\\{#AppName}"', self.script)
        # The only user-data path the uninstaller removes on its own is the updates folder.
        uninstall = entry_names(section(self.script, "UninstallDelete"))
        self.assertEqual([n for n in uninstall if not n.startswith("{app}\\")],
                         [r"{localappdata}\{#DataDirName}\updates"])

    def test_uninstall_removes_the_install_names_a_native_update_left_unlogged(self):
        """After a native update the tree holds files no setup logged (the sentinel, a DLL a
        later version added): [UninstallDelete] lists every {app} name [InstallDelete] clears,
        with the same type, so they and {app} itself go with the uninstall (OpenWhisper's
        installer lists _internal, its programs, the manifest and the sentinel)."""
        installs = section(self.script, "InstallDelete")
        uninstalls = section(self.script, "UninstallDelete")
        self.assertEqual([line for line in uninstalls if '"{app}\\' in line], installs)
        names = entry_names(uninstalls)
        # The native updater's two names in the tree (update/update_contract.h, amendment 22).
        for leaf in (".openwhisper-cpp-update.json", ".openwhisper-cpp-update-complete"):
            self.assertIn("{app}\\" + leaf, names)
        contract = (REPO / "src" / "update" / "update_contract.h").read_text(encoding="utf-8")
        self.assertIn('".openwhisper-cpp-update.json" and ".openwhisper-cpp-update-complete"', contract)
        exe_patterns = [n.split("\\", 1)[1] for n in names if n.endswith(".exe")]
        updater = re.search(r'updater_name = "([^"]+)"',
                            (REPO / "src" / "update_native" / "manifest.h")
                            .read_text(encoding="utf-8")).group(1)
        for program in (self.identity["app_exe"], self.identity["cli_exe"], updater):
            self.assertTrue(any(fnmatch.fnmatch(program, p) for p in exe_patterns), program)
        for name in names:
            lowered = name.lower()
            leaf = name.rsplit("\\", 1)[1].lower()
            # Inno's own uninstaller removes itself; nothing names it or {app} as a whole.
            self.assertFalse(fnmatch.fnmatch("unins000.exe", leaf), name)
            self.assertFalse(fnmatch.fnmatch("unins000.dat", leaf), name)
            self.assertNotEqual(lowered, "{app}")
            self.assertNotIn("speech-models", lowered)
            self.assertNotIn("{userappdata}", lowered)
            self.assertTrue(lowered.startswith("{app}\\")
                            or lowered == r"{localappdata}\{#datadirname}\updates", name)
        # The staged layout stays covered: StageCheck pins [InstallDelete] against it, and the
        # first assertion keeps [UninstallDelete] equal to it.
        self.assertNotIn("dirifempty", "\n".join(uninstalls))

    def test_install_delete_stays_as_a_recorded_decision(self):
        """M10.17 change 8 planned to drop [InstallDelete] for the helper; it is kept, because
        neither the helper's prepare nor Inno removes a retired file, and an elevated setup runs
        no helper at all. The script says so where the section starts."""
        before = self.script.split("[InstallDelete]", 1)[0].rsplit("\n\n", 1)[1]
        self.assertIn("Decision (kept", before)
        self.assertIn("IsAdmin", before)
        self.assertIn("if IsAdmin then Exit;", self.script)
        readme = (PACKAGING / "README.md").read_text(encoding="utf-8")
        self.assertIn("`[InstallDelete]` stays", readme)

    def test_per_user_install_and_shared_mutexes(self):
        for directive in ("PrivilegesRequired=lowest", "PrivilegesRequiredOverridesAllowed=dialog",
                          r"DefaultDirName={autopf}\{#InstallDirName}", "MinVersion=10.0",
                          "ArchitecturesAllowed=x64compatible",
                          "ArchitecturesInstallIn64BitMode=x64compatible",
                          "CloseApplications=force", "AppMutex={#AppMutexNames}",
                          "SetupMutex={#SetupMutexName}"):
            self.assertIn(directive, section(self.script, "Setup"))

    def test_setup_rechecks_the_application_and_update_gates_before_writing(self):
        prepare = self.script.split("function PrepareToInstall", 1)[1].split("procedure CurStepChanged", 1)[0]
        self.assertLess(prepare.index("CheckForMutexes('{#AppMutexNames}')"),
                        prepare.index("RunSetupHelper('prepare')"))
        self.assertNotIn("taskkill.exe", self.script.lower())

    def test_signing_only_when_a_sign_tool_is_defined(self):
        block = re.search(r"#ifdef SignToolName\s*(.*?)#endif", self.script, re.S)
        self.assertIsNotNone(block)
        self.assertIn("SignTool={#SignToolName}", block.group(1))
        self.assertIn("SignedUninstaller=yes", block.group(1))
        self.assertEqual(self.script.count("SignTool="), 1)

    def test_update_relaunch_and_post_install_launch(self):
        runs = section(self.script, "Run")
        self.assertIn('Filename: "{app}\\{#AppExeName}"; Description: "Launch {#AppName}"; '
                      "Flags: nowait postinstall skipifsilent", runs)
        self.assertIn('Filename: "{app}\\{#AppExeName}"; Flags: nowait runasoriginaluser; '
                      "Check: IsInAppUpdate", runs)
        self.assertIn("ExpandConstant('{param:{#UpdateParam}|0}') = '1'", self.script)

    def test_uninstall_keeps_user_data_unless_asked(self):
        uninstall = self.script.split("procedure CurUninstallStepChanged", 1)[1]
        prompt = re.search(r"SuppressibleMsgBox\((.*?)\) = IDYES", uninstall, re.S)
        self.assertIsNotNone(prompt)
        # A silent uninstall (/SUPPRESSMSGBOXES) answers with the default: No.
        self.assertTrue(prompt.group(1).rstrip().endswith("mbConfirmation, MB_YESNO, IDNO"))
        self.assertIn("Choose No to keep them for a future reinstall.", prompt.group(1))
        self.assertIn("ExpandConstant('{localappdata}\\{#DataDirName}')", self.script)
        # OpenWhisper_CPP's speech models live in speech-models\ inside the data folder (base/paths.h
        # speech_models_root), so Yes deletes them: OpenWhisper's "never removed" line would be
        # false here.
        self.assertNotIn("never removed", prompt.group(1))
        self.assertIn("'Downloaded speech models are stored in this folder and are removed with "
                      "it.'", prompt.group(1))
        self.assertIn('"speech-models"',
                      (REPO / "src" / "base" / "paths.cpp").read_text(encoding="utf-8"))

    def test_tasks(self):
        tasks = section(self.script, "Tasks")
        self.assertIn('Name: "desktopicon"; Description: "Create a &desktop shortcut"; '
                      'GroupDescription: "Additional shortcuts:"', tasks)
        self.assertIn('Name: "startupicon"; Description: "Start {#AppName} when I sign in"; '
                      'GroupDescription: "Startup:"; Flags: unchecked', tasks)


class ReleaseRotation(rs.TempDirTestCase):
    @unittest.skipUnless(shutil.which("pwsh") or shutil.which("powershell"), "PowerShell is not installed")
    def test_previous_version_rotates_its_setup_and_zip(self):
        build = self.tmp / "candidate"
        previous = self.tmp / "previous"
        build.mkdir()
        older = {"OpenWhisper_CPP-Setup-0.0.0.exe", "OpenWhisper_CPP-0.0.0-win64.zip"}
        current = {"OpenWhisper_CPP-Setup-0.1.0.exe", "OpenWhisper_CPP-0.1.0-win64.zip"}
        unrelated = {"OpenWhisper_CPP-Setup-notes.exe", "OpenWhisper_CPP-other-win64.zip", "notes.txt"}
        for name in older | current | unrelated:
            (build / name).write_text(name, encoding="utf-8")
        script = self.tmp / "rotate.ps1"
        script.write_text(
            "param($Helpers, $Candidate, $Previous)\n"
            "$ErrorActionPreference = 'Stop'\n"
            ". $Helpers\n"
            "Move-PreviousReleaseArtifacts $Candidate $Previous OpenWhisper_CPP 0.1.0\n",
            encoding="utf-8",
        )
        shell = shutil.which("pwsh") or shutil.which("powershell")
        subprocess.run([shell, "-NoProfile", "-File", str(script), str(PACKAGING / "packaging-common.ps1"),
                        str(build), str(previous)], check=True, capture_output=True, text=True)
        self.assertEqual({p.name for p in previous.iterdir()}, older)
        self.assertEqual({p.name for p in build.iterdir()}, current | unrelated)


class Defines(rs.TempDirTestCase):
    def defines(self, identity=None, version="0.1.0"):
        identity = identity or json.loads(SOURCE_IDENTITY.read_text(encoding="utf-8"))
        return inno_defines.defines(identity, version=version, source=self.tmp / "stage",
                                    output_dir=self.tmp / "installer")

    def test_names_come_from_the_release_identity(self):
        identity = json.loads(SOURCE_IDENTITY.read_text(encoding="utf-8"))
        values = self.defines()
        bare = identity["app_id"].strip("{}")
        product = app_identity_field("product")
        self.assertEqual(values["AppGuid"], bare)
        self.assertEqual("{" + values["AppGuid"] + "}", identity["app_id"])
        self.assertEqual(values["AppName"], product)
        self.assertEqual(values["DataDirName"], app_identity_field("data_dir_name"))
        self.assertEqual(values["AppPublisher"], identity["publisher"])
        self.assertEqual(values["InstallDirName"], identity["install_dir_name"])
        self.assertEqual(values["AppExeName"], identity["app_exe"])
        self.assertEqual(values["CliExeName"], identity["cli_exe"])
        self.assertEqual(values["OutputBaseFilename"], f"{product}-Setup-0.1.0")
        self.assertEqual(values["AppVersion"], "0.1.0")
        # ReleaseIdentity::app_mutexes() and setup_mutex() (src/update/release_identity.cpp).
        self.assertEqual(values["AppMutexNames"],
                         f"{product}-App-{bare},Global\\{product}-App-{bare}")
        self.assertEqual(values["SetupMutexName"], f"Global\\{product}-Setup-{bare}")
        # setup_update_switch(): "/" + upper(product) + "UPDATE=1".
        self.assertEqual(values["UpdateParam"], product.upper() + "UPDATE")
        self.assertEqual(Path(values["SourceDir"]), (self.tmp / "stage").resolve())
        # No publisher URL in the identity: the three URL directives stay out.
        self.assertEqual(identity["publisher_url"], "")
        self.assertNotIn("AppURL", values)

    def test_publisher_url_is_passed_when_the_identity_has_one(self):
        identity = {**rs.IDENTITY, "publisher_url": "https://example.test/openwhisper-cpp"}
        self.assertEqual(self.defines(identity)["AppURL"], "https://example.test/openwhisper-cpp")

    def test_malformed_input_is_refused(self):
        with self.assertRaises(common.ReleaseToolError):
            self.defines(version="0.1")
        with self.assertRaises(common.ReleaseToolError):
            self.defines({**rs.IDENTITY, "app_id": "{3d200c15-5c89-4cdf-a222-2f7ba53b9e6c}"})
        with self.assertRaises(common.ReleaseToolError):
            self.defines({**rs.IDENTITY, "publisher": 'Fiori "Labs"'})
        with self.assertRaises(common.ReleaseToolError):
            self.defines({**rs.IDENTITY, "cli_exe": ""})

    def test_cli_prints_one_iscc_argument_per_line(self):
        completed = subprocess.run(
            [sys.executable, "-I", "-B", str(PACKAGING / "inno_defines.py"), "--version", "0.1.0",
             "--source", str(self.tmp / "stage"), "--output-dir", str(self.tmp / "out"),
             "--identity", str(rs.IDENTITY_FILE)],
            capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        lines = completed.stdout.splitlines()
        self.assertTrue(all(line.startswith("/D") for line in lines))
        self.assertIn("/DAppVersion=0.1.0", lines)
        bad = subprocess.run(
            [sys.executable, "-I", "-B", str(PACKAGING / "inno_defines.py"), "--version", "x",
             "--source", ".", "--output-dir", ".", "--identity", str(rs.IDENTITY_FILE)],
            capture_output=True, text=True, check=False)
        self.assertEqual(bad.returncode, 1)
        self.assertIn("Version must be X.Y.Z", bad.stderr)

    @unittest.skipUnless(find_iscc(), "Inno Setup (ISCC.exe) is not installed")
    def test_script_preprocesses_with_the_identity_defines(self):
        """ISCC preprocesses (and syntax-checks, /O- writes no setup) the script with the source
        tree's identity, and the expanded text holds the exact AppId and mutex names."""
        stage = self.tmp / "stage"
        (stage / "platforms").mkdir(parents=True)
        for name in ("openwhisper-cpp.exe", "openwhisper-cpp-updater.exe", "runtime.dll",
                     ".openwhisper-cpp-update.json"):
            (stage / name).write_bytes(b"stand-in")
        (stage / "platforms" / "qwindows.dll").write_bytes(b"stand-in")
        expanded = self.tmp / "expanded.iss"
        wrapper = self.tmp / "wrapper.iss"
        wrapper.write_text(f'#include "{INSTALLER}"\n#expr SaveToFile("{expanded}")\n',
                           encoding="utf-8")
        values = self.defines()
        completed = subprocess.run(
            [find_iscc(), "/Q", "/O-", *inno_defines.iscc_arguments(values), str(wrapper)],
            capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        text = expanded.read_text(encoding="utf-8", errors="replace")
        identity = json.loads(SOURCE_IDENTITY.read_text(encoding="utf-8"))
        bare = identity["app_id"].strip("{}")
        self.assertIn("AppId={" + identity["app_id"] + "\n", text)
        self.assertIn(f"AppMutex=OpenWhisper_CPP-App-{bare},Global\\OpenWhisper_CPP-App-{bare}\n", text)
        self.assertIn(f"SetupMutex=Global\\OpenWhisper_CPP-Setup-{bare}\n", text)
        self.assertIn(f"'{identity['app_id']}_is1'", text)
        self.assertIn("{param:OPENWHISPER_CPPUPDATE|0}", text)
        self.assertIn('Name: "{localappdata}\\OpenWhisper_CPP\\updates"', text)


class StageCheck(rs.TempDirTestCase):
    """inno_defines.py --check-stage: [InstallDelete] clears every top-level name of the stage."""

    # The top level of the tree stage.ps1 writes (windeployqt 6.10.3, vcpkg x64-windows).
    STAGED_FILES = ("openwhisper-cpp.exe", "openwhisper-cpp-cli.exe", "Qt6Core.dll", "avcodec-61.dll",
                    "msvcp140.dll", "opengl32sw.dll", "THIRD_PARTY_NOTICES.md")
    STAGED_FOLDERS = ("generic", "iconengines", "imageformats", "licenses",
                      "networkinformation", "platforms", "styles", "tls", "webui")

    def stage(self, files=STAGED_FILES, folders=STAGED_FOLDERS):
        stage = self.tmp / "stage"
        stage.mkdir()
        for name in files:
            (stage / name).write_bytes(b"stand-in")
        for name in folders:
            (stage / name).mkdir()
            (stage / name / "plugin.dll").write_bytes(b"stand-in")
        return stage

    def test_the_staged_layout_is_managed(self):
        script = INSTALLER.read_text(encoding="utf-8")
        self.assertEqual(inno_defines.unmanaged_entries(self.stage(), script), [])

    def test_a_name_no_entry_clears_is_reported(self):
        script = INSTALLER.read_text(encoding="utf-8")
        stage = self.stage(files=self.STAGED_FILES + ("qt.conf",),
                           folders=self.STAGED_FOLDERS + ("sqldrivers", "odd.dll"))
        # "{app}\*.dll" is a files entry: it does not clear a folder whose name matches it.
        self.assertEqual(inno_defines.unmanaged_entries(stage, script),
                         ["odd.dll\\", "qt.conf", "sqldrivers\\"])

    def test_patterns_are_read_from_install_delete_only(self):
        script = ('[InstallDelete]\nType: files; Name: "{app}\\*.DLL"\n'
                  '; Type: filesandordirs; Name: "{app}\\commented"\n'
                  'Type: filesandordirs; Name: "{app}\\Plugins"\n'
                  '[UninstallDelete]\nType: filesandordirs; Name: "{app}\\later"\n')
        self.assertEqual(inno_defines.install_delete_patterns(script),
                         [("files", "*.dll"), ("filesandordirs", "plugins")])
        stage = self.stage(files=("a.dll",), folders=("plugins", "commented", "later"))
        self.assertEqual(inno_defines.unmanaged_entries(stage, script),
                         ["commented\\", "later\\"])

    def test_cli_check_stage(self):
        def run(stage):
            return subprocess.run(
                [sys.executable, "-I", "-B", str(PACKAGING / "inno_defines.py"), "--check-stage",
                 "--source", str(stage)], capture_output=True, text=True, check=False)

        managed = run(self.stage())
        self.assertEqual(managed.returncode, 0, managed.stderr)
        (self.tmp / "stage" / "translations").mkdir()
        unmanaged = run(self.tmp / "stage")
        self.assertEqual(unmanaged.returncode, 1)
        self.assertIn("OpenWhisper_CPP.iss [InstallDelete] does not clear {app}\\translations\\",
                      unmanaged.stderr)

    def test_build_installer_checks_the_stage_before_the_setup(self):
        script = (PACKAGING / "build-installer.ps1").read_text(encoding="utf-8")
        check = script.index("'--check-stage', '--source', $Stage")
        self.assertLess(script.index("'--self-test', '--installed'"), check)
        self.assertLess(check, script.index("Invoke-Native $Iscc"))


class SmokeUpdateScript(unittest.TestCase):
    """Static checks of smoke-update.ps1, the owner's real self-update (M10.17). It was written
    without Windows and has never run, so these pin what it may touch and in what order."""

    def setUp(self):
        self.path = PACKAGING / "smoke-update.ps1"
        self.script = self.path.read_text(encoding="utf-8")
        self.body = self.script.split("#>", 1)[1]

    def test_it_refuses_unless_the_owner_runs_it_here(self):
        body = self.body
        guards = [body.index(text) for text in (
            "$env:GITHUB_ACTIONS -eq 'true'", "if (-not $OnThisComputer)",
            "WindowsBuiltInRole]::Administrator", "foreach ($hive in @('HKCU', 'HKLM'))",
            "An update recovery is pending")]
        first_change = min(body.index("Run-Setup $Releases"), body.index("New-Item"))
        self.assertTrue(all(index < first_change for index in guards))
        self.assertIn("[switch]$OnThisComputer", body)
        self.assertNotIn("$OnRunner", body)  # unlike smoke-setup.ps1, never on CI

    def test_it_installs_and_keeps_data_only_in_its_temporary_folder(self):
        body = self.body
        self.assertIn("$Root = Join-Path $Scratch", body)
        self.assertIn("$Target = Join-Path $Root $Identity.install_dir_name", body)
        self.assertIn("$LocalRoot = Join-Path $Root 'LocalAppData'", body)
        self.assertIn("'/CURRENTUSER', \"/DIR=`\"$Target`\"\"", body)
        self.assertNotIn("Programs", body)
        # Every child (setup, app, updater, health launch) inherits the temporary LOCALAPPDATA,
        # and OPENWHISPER_CPP_DATA_DIR is cleared (a non-default data root is not eligible, A16-16).
        self.assertIn("$env:LOCALAPPDATA = $LocalRoot", body)
        self.assertIn("Remove-Item Env:OPENWHISPER_CPP_DATA_DIR", body)
        self.assertIn("Restore-Environment", body.split("} finally {", 1)[1])
        # Nothing but its own folder is ever deleted.
        for line in body.splitlines():
            if "Remove-Item " in line and "Env:" not in line:
                self.assertIn("-LiteralPath $Root", line)
        removed = [line for line in body.splitlines() if "Remove-ItemProperty" in line]
        self.assertEqual(len(removed), 1)
        cleanup = body.split("} finally {", 1)[1]
        self.assertIn("$pending.IndexOf($Root, [StringComparison]::OrdinalIgnoreCase) -ge 0",
                      cleanup)

    def test_the_feed_is_the_release_tool_on_loopback(self):
        self.assertIn("serve_update_feed.py", self.body)
        self.assertIn("'--host', '127.0.0.1'", self.body)
        self.assertIn("[Net.IPAddress]::Loopback", self.body)
        self.assertNotRegex(self.body, r"https?://(?!127\.0\.0\.1)")

    def test_update_then_rollback_then_uninstall_in_order(self):
        body = self.body.split("$Uninstalled = $false\ntry {", 1)[1]
        steps = [body.index(text) for text in (
            "Run-Setup $Releases[$From].Setup",
            "Start-App $url $false",
            '"the update to $To to land"',
            "Start-App $url $true",
            '"the update to $Failing to fail"',
            '"the rollback to $To"',
            "Run-Uninstall",
            "if (Test-Path -LiteralPath $Target)")]
        self.assertEqual(steps, sorted(steps))
        # The health-failure switch is set for the failing release's hand-off only.
        self.assertEqual(self.body.count("Start-App $url $true"), 1)
        self.assertIn("$env:OPENWHISPER_CPP_TEST_FAIL_HEALTH = '1'", self.body)
        # It refuses a failing build that does not know the switch rather than install it.
        self.assertLess(self.body.index("Test-FailHealthSwitch $Releases[$Failing].Archive"),
                        self.body.index("$Uninstalled = $false"))
        # The names it reads are the updater's (update/update_contract.cpp).
        contract = (REPO / "src" / "update" / "update_contract.cpp").read_text(encoding="utf-8")
        self.assertIn('"!" + std::string(identity().product) + "UpdateRecover"', contract)
        self.assertIn('$RunOnceValue = "!$($Product)UpdateRecover"', self.body)
        self.assertIn("'apply_error.txt'", self.body)
        self.assertIn('kErrorName = "apply_error.txt"',
                      (REPO / "src" / "update" / "update_contract.h").read_text(encoding="utf-8"))

    def test_it_cleans_up_whatever_happens(self):
        cleanup = self.body.split("} finally {", 1)[1]
        for step in ("Stop-Feed", "App-Processes", "Run-Uninstall",
                     "Remove-Item -LiteralPath $Root -Recurse"):
            self.assertIn(step, cleanup)

    def test_the_readme_says_it_was_never_run_and_no_workflow_runs_it(self):
        readme = (PACKAGING / "README.md").read_text(encoding="utf-8")
        self.assertIn("## Self-update check (written, never run)", readme)
        self.assertIn("**has never been run**", readme)
        self.assertIn("| `smoke-update.ps1` |", readme)
        for workflow in (REPO / ".github" / "workflows").glob("*.yml"):
            self.assertNotIn("smoke-update", workflow.read_text(encoding="utf-8"), workflow.name)

    @unittest.skipUnless(shutil.which("pwsh") or shutil.which("powershell"),
                         "PowerShell is not installed")
    def test_both_smoke_scripts_parse(self):
        shell = shutil.which("pwsh") or shutil.which("powershell")
        for name in ("smoke-setup.ps1", "smoke-update.ps1"):
            command = ("$e = $null; [System.Management.Automation.Language.Parser]::ParseFile("
                       f"'{PACKAGING / name}', [ref]$null, [ref]$e) | Out-Null; "
                       "if ($e) { $e | ForEach-Object { $_.ToString() }; exit 1 }")
            completed = subprocess.run([shell, "-NoProfile", "-Command", command],
                                       capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, name + completed.stdout + completed.stderr)


class BuildEnvironment(unittest.TestCase):
    def test_the_developer_shell_keeps_the_callers_vcpkg(self):
        """vsenv.ps1's developer shell points VCPKG_ROOT at Visual Studio's bundled vcpkg; the
        build must keep the one the caller set (CI's clone at the manifest's baseline)."""
        script = (PACKAGING / "build-installer.ps1").read_text(encoding="utf-8")
        saved = script.index("$vcpkgRoot = $env:VCPKG_ROOT")
        entered = script.index("scripts\\vsenv.ps1')")
        restored = script.index("if ($vcpkgRoot) { $env:VCPKG_ROOT = $vcpkgRoot }")
        self.assertLess(saved, entered)
        self.assertLess(entered, restored)
        self.assertIn("$env:VCPKG_ROOT = $vcpkg", (REPO / ".github" / "workflows" / "ci.yml")
                      .read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
