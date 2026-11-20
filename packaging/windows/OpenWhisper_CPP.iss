; Inno Setup script for OpenWhisper_CPP.
;
; packaging\windows\build-installer.ps1 compiles it. Every name the setup shares with the
; application (AppId, publisher, folders, executables, the named mutexes, the update switch)
; arrives as a /D define printed by inno_defines.py from src\update\data\release_identity.json,
; so none is typed here. To compile by hand after a build has staged the tree:
;
;   py -3.12 packaging\windows\inno_defines.py --version 0.1.0 --source out\stage
;       --output-dir out\installer   (then pass each printed line to ISCC.exe with this file)
;
; Per-user install: no UAC prompt, no admin rights. The app only needs a low-level keyboard
; hook, which runs unelevated.

#ifndef AppGuid
  #error The release identity defines are missing: build with packaging\windows\build-installer.ps1
#endif
#ifndef SourceDir
  #error SourceDir (the staged install tree) is not defined
#endif

; "{{" is Inno's escape for a literal brace: the AppId is the braced GUID.
#define AppIdValue "{{" + AppGuid + "}"
#define UninstallKeyName "{" + AppGuid + "}_is1"

[Setup]
; AppId uniquely identifies this application for upgrades and uninstall.
; NEVER change it: a new GUID makes Windows treat an upgrade as a second,
; separate installation.
AppId={#AppIdValue}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
#ifdef AppURL
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
#endif
VersionInfoVersion={#AppVersion}
;
; Code signing: build-installer.ps1 defines SignToolName and passes the
; matching /S command only when a signing method is configured. Inno then
; signs the uninstaller it embeds and the finished setup exe.
#ifdef SignToolName
SignTool={#SignToolName}
SignedUninstaller=yes
#endif

; Per-user installation into %LOCALAPPDATA%\Programs\OpenWhisper_CPP.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\{#InstallDirName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
WizardStyle=modern
; Solid compression: the payload is Qt, FFmpeg and the other DLLs, with a lot
; of cross-file redundancy.
;
; lzma2/max (32 MB dictionary) rather than ultra64 (64 MB): ISCC.exe is a
; 32-bit process, and ultra64 combined with multiple block threads exhausts
; its address space on a large payload and aborts with "Out of memory".
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=1
ShowLanguageDialog=no
DisableWelcomePage=no
; force, not yes: Restart Manager's graceful shutdown sends a close request,
; and the app answers it by minimizing to tray, so a plain `yes` ends in
; "Setup was unable to automatically close all applications".
CloseApplications=force
CloseApplicationsFilter=*.exe,*.dll
; The app holds these from the start of main(): the setup waits for a running
; app, and the app's launch gate waits while this setup holds SetupMutex.
AppMutex={#AppMutexNames}
SetupMutex={#SetupMutexName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"
Name: "startupicon"; Description: "Start {#AppName} when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

; Inno never deletes a file an older version shipped and this one does not, so
; every name the install tree manages is cleared before the new files land: a
; retired DLL or Qt plugin would otherwise stay and could be loaded. User data
; lives in {localappdata}\{#DataDirName}, outside {app}, and the uninstaller
; (unins*) is Inno's own: neither is ever listed here.
;
; Decision (kept, although M10.17 change 8 planned to drop it for the helper):
; Inno runs this section on every install, after PrepareToInstall and before
; [Files]. In a per-user setup PrepareToInstall has already run the helper's
; "prepare", which copies every managed file aside so a failed setup can be
; rolled back; the helper never deletes a retired file, so this section is
; still what clears it. When setup runs elevated (IsAdmin: the "for all users"
; choice of the privileges dialog, or an administrator's prompt) there is no
; helper and no rollback copy, and this section is the only cleanup. Keep it in
; step with [UninstallDelete] below; inno_defines.py --check-stage fails the
; build when a staged top-level name is missing here.
[InstallDelete]
Type: files; Name: "{app}\*.dll"
Type: files; Name: "{app}\openwhisper-cpp*.exe"
Type: files; Name: "{app}\.openwhisper-cpp-update.json"
Type: files; Name: "{app}\.openwhisper-cpp-update-complete"
Type: files; Name: "{app}\THIRD_PARTY_NOTICES.md"
Type: filesandordirs; Name: "{app}\licenses"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\share"
Type: filesandordirs; Name: "{app}\webui"

[Files]
Source: "{#SourceDir}\openwhisper-cpp-updater.exe"; DestDir: "{tmp}\fv-helper"; Flags: dontcopy noencryption
Source: "{#SourceDir}\*.dll"; DestDir: "{tmp}\fv-helper"; Flags: dontcopy noencryption
Source: "{#SourceDir}\.openwhisper-cpp-update.json"; DestDir: "{tmp}\fv-helper"; Flags: dontcopy noencryption
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon
Name: "{userstartup}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: startupicon

; The uninstaller removes only what setup logged. A native update swaps the
; whole install tree (openwhisper-cpp-updater), so after one the tree holds files no
; setup logged: the update sentinel, a DLL or plug-in a later version added.
; Without these entries they, and so {app} itself, would survive an uninstall
; (Inno removes {app} at the end only when it is empty, as OpenWhisper relies on).
; The {app} names are exactly [InstallDelete]'s; unins* stays Inno's own. The
; only path outside {app} is the native updater's work folder: the rest of
; {localappdata}\{#DataDirName} (settings, history, speech models) is the user's
; and goes only when the user answers Yes in CurUninstallStepChanged, and the
; updater's .new-/.old-/.failed- siblings of {app} are never named here.
[UninstallDelete]
Type: files; Name: "{app}\*.dll"
Type: files; Name: "{app}\openwhisper-cpp*.exe"
Type: files; Name: "{app}\.openwhisper-cpp-update.json"
Type: files; Name: "{app}\.openwhisper-cpp-update-complete"
Type: files; Name: "{app}\THIRD_PARTY_NOTICES.md"
Type: filesandordirs; Name: "{app}\licenses"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\tls"
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\share"
Type: filesandordirs; Name: "{app}\webui"
Type: filesandordirs; Name: "{localappdata}\{#DataDirName}\updates"

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
; An update the app handed over (/{#UpdateParam}=1) runs silently; this
; relaunches the new version as the user who started it.
Filename: "{app}\{#AppExeName}"; Flags: nowait runasoriginaluser; Check: IsInAppUpdate

[Code]
var
  SetupPrepared, SetupFinished: Boolean;

function IsInAppUpdate(): Boolean;
begin
  Result := ExpandConstant('{param:{#UpdateParam}|0}') = '1';
end;

function UserDataDir(): String;
begin
  Result := ExpandConstant('{localappdata}\{#DataDirName}');
end;

function IsExistingInstall(): Boolean;
var
  UninstallKey: String;
begin
  UninstallKey := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
    '{#UninstallKeyName}';
  Result := RegKeyExists(HKEY_CURRENT_USER, UninstallKey) or
    RegKeyExists(HKEY_LOCAL_MACHINE, UninstallKey);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := IsExistingInstall() and
    ((PageID = wpWelcome) or
     (PageID = wpLicense) or
     (PageID = wpSelectDir) or
     (PageID = wpSelectProgramGroup) or
     (PageID = wpSelectTasks));
end;

function RunSetupHelper(Action: String): String;
var
  Code: Integer;
  ErrorText: AnsiString;
  HelperDir, ErrorFile: String;
begin
  Result := '';
  HelperDir := ExpandConstant('{tmp}\fv-helper');
  ErrorFile := HelperDir + '\setup-error.txt';
  DeleteFile(ErrorFile);
  if not Exec(HelperDir + '\openwhisper-cpp-updater.exe',
    '--setup-action ' + Action + ' --app-dir "' + ExpandConstant('{app}') + '"',
    HelperDir, SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := 'Setup could not start the update helper.'
  else if Code <> 0 then
  begin
    if LoadStringFromFile(ErrorFile, ErrorText) then Result := UTF8Decode(ErrorText)
    else Result := 'Setup could not complete its update transaction.';
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if SetupPrepared then Exit;
  { An app or native update may have started since the wizard's initial mutex check. }
  if CheckForMutexes('{#AppMutexNames}') then
  begin
    Result := 'Close {#AppName} and wait for any running update to finish, then try again.';
    Exit;
  end;
  { The per-user helper never reads a user-writable transaction with elevated privileges. }
  if IsAdmin then Exit;
  ExtractTemporaryFiles('{tmp}\fv-helper\*');
  Result := RunSetupHelper('prepare');
  SetupPrepared := Result = '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Failure: String;
begin
  if (CurStep = ssPostInstall) and SetupPrepared then
  begin
    Failure := RunSetupHelper('finish');
    if Failure <> '' then RaiseException(Failure);
    SetupFinished := True;
  end;
end;

procedure DeinitializeSetup();
var
  Failure: String;
begin
  if SetupPrepared and not SetupFinished then
  begin
    Failure := RunSetupHelper('rollback');
    if Failure <> '' then
      SuppressibleMsgBox('Setup could not restore the previous installation.' + #13#10 +
        Failure, mbError, MB_OK, IDOK);
  end;
end;

{
  Uninstall: offer to delete user data.

  Settings, history database, saved recordings, and any downloaded components
  live in %LOCALAPPDATA%\OpenWhisper_CPP, outside the install directory, so they
  survive an uninstall unless the user opts in here. Left unchecked by default
  because a reinstall should keep the user's transcription history; a silent
  uninstall (/SUPPRESSMSGBOXES) takes that default, No.

  OpenWhisper's last line says downloaded speech models "are stored separately
  and are never removed". That is not true here: OpenWhisper_CPP keeps its speech
  models in speech-models\ inside this folder, so the line says they go with it.
}
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DataDir := UserDataDir();
    if DirExists(DataDir) then
    begin
      if SuppressibleMsgBox(
           'Also remove your {#AppName} settings, transcription history, and any' + #13#10 +
           'downloaded components?' + #13#10 + #13#10 +
           DataDir + #13#10 + #13#10 +
           'Choose No to keep them for a future reinstall.' + #13#10 + #13#10 +
           'Downloaded speech models are stored in this folder and are removed with it.',
           mbConfirmation, MB_YESNO, IDNO) = IDYES then
      begin
        DelTree(DataDir, True, True, True);
      end;
    end;
  end;
end;
