<#
.SYNOPSIS
    Install, upgrade and uninstall FioriVoce silently, and check what each step leaves behind.

.DESCRIPTION
    Installs the previous setup into a new temporary folder (/DIR, per user), plants a retired
    DLL in the install folder and a file in the user data folder, installs the new setup over
    it, then checks that the retired DLL is gone ([InstallDelete]), the user data is untouched,
    the uninstall entry reports the new version and `fiorivoce-cli --self-test --installed`
    passes in the upgraded folder. A silent uninstall must then remove the programs and keep the
    user data (its data prompt defaults to No).

    This runs setup programs and registers an uninstall entry under HKCU: it is meant for a
    disposable CI runner. On any other computer it refuses unless -OnThisComputer is passed, and
    it refuses whenever FioriVoce is already installed for this user or the machine. Whatever
    happens, it uninstalls what it installed and removes the file it planted.

.PARAMETER PreviousSetup
    The older FioriVoce-Setup-X.Y.Z.exe, installed first.

.PARAMETER NewSetup
    The setup under test, installed over the previous one.

.PARAMETER Version
    The version NewSetup installs (X.Y.Z).

.PARAMETER OnThisComputer
    Allow the run outside GitHub Actions (the owner's own check).

.EXAMPLE
    .\packaging\windows\smoke-setup.ps1 -PreviousSetup out\installer-previous\FioriVoce-Setup-0.1.0.exe `
        -NewSetup out\installer\FioriVoce-Setup-0.1.1.exe -Version 0.1.1 -OnThisComputer
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PreviousSetup,
    [Parameter(Mandatory)][string]$NewSetup,
    [Parameter(Mandatory)][string]$Version,
    [switch]$OnThisComputer
)
$ErrorActionPreference = 'Stop'

$OnRunner = $env:GITHUB_ACTIONS -eq 'true' -and $env:RUNNER_TEMP
if (-not $OnRunner -and -not $OnThisComputer) {
    throw 'This lifecycle check installs software. Run it on a disposable GitHub Actions runner, or pass -OnThisComputer.'
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must be X.Y.Z, not '$Version'" }
$PreviousSetup = (Resolve-Path -LiteralPath $PreviousSetup).Path
$NewSetup = (Resolve-Path -LiteralPath $NewSetup).Path

# The uninstall entry Inno writes is named after the AppId of the release identity.
$IdentityFile = Join-Path $PSScriptRoot '..\..\src\update\data\release_identity.json'
$Identity = Get-Content -Raw -LiteralPath $IdentityFile | ConvertFrom-Json
if ($Identity.app_id -notmatch '^\{[0-9A-F-]{36}\}$') { throw "Unexpected app_id in $IdentityFile" }
$UninstallKey = "Software\Microsoft\Windows\CurrentVersion\Uninstall\$($Identity.app_id)_is1"
foreach ($hive in @('HKCU', 'HKLM')) {
    if (Test-Path "$hive`:\$UninstallKey") {
        throw 'An existing installation is present; refusing to replace it.'
    }
}

if ($OnRunner) { $Scratch = $env:RUNNER_TEMP } else { $Scratch = [IO.Path]::GetTempPath() }
$RunId = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$Target = Join-Path $Scratch "FioriVoce-setup-smoke-$RunId"
$Logs = Join-Path $Scratch 'fiorivoce-setup-logs'
# The installed app's user data folder: the setup must never write it, and uninstall must keep it.
# It is named after the product, which begins the setup's name ("FioriVoce-Setup-").
$Product = $Identity.setup_prefix -replace '-?Setup-?$', ''
$Data = Join-Path $env:LOCALAPPDATA $Product
$DataExisted = Test-Path -LiteralPath $Data
$Preserved = Join-Path $Data "setup-smoke-user-data-$RunId.txt"
New-Item -ItemType Directory -Force -Path $Logs | Out-Null

function Wait-Exit([Diagnostics.Process]$Process, [int]$Milliseconds, [string]$Name) {
    if (-not $Process.WaitForExit($Milliseconds)) {
        $Process.Kill()
        throw "$Name timed out"
    }
    return $Process.ExitCode
}

function Run-Setup([string]$Installer, [string]$Name) {
    $process = Start-Process -FilePath $Installer -ArgumentList @(
        '/SP-', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
        '/CURRENTUSER', "/DIR=`"$Target`"", "/LOG=`"$Logs\$Name.log`""
    ) -WindowStyle Hidden -PassThru
    $code = Wait-Exit $process 300000 $Name
    if ($code -ne 0) { throw "$Name failed: $code" }
}

function Run-Uninstall {
    $uninstaller = Join-Path $Target 'unins000.exe'
    $process = Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$Logs\uninstall.log`""
    ) -WindowStyle Hidden -PassThru
    $code = Wait-Exit $process 120000 'Uninstall'
    if ($code -ne 0) { throw "Uninstall failed: $code" }
    # The uninstaller copies itself to a temporary folder and removes {app} from there after
    # this process has exited: give it a moment.
    $deadline = (Get-Date).AddSeconds(30)
    while ((Test-Path -LiteralPath (Join-Path $Target $Identity.app_exe)) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
}

$Uninstalled = $false
try {
    Run-Setup $PreviousSetup 'previous'
    $Stale = Join-Path $Target 'retired-smoke-module.dll'
    [IO.File]::WriteAllText($Stale, 'old dependency')
    New-Item -ItemType Directory -Force -Path $Data | Out-Null
    [IO.File]::WriteAllText($Preserved, 'keep my data')

    Run-Setup $NewSetup 'upgrade'
    if (Test-Path -LiteralPath $Stale) { throw 'Setup left a retired runtime file behind.' }
    if ([IO.File]::ReadAllText($Preserved) -ne 'keep my data') { throw 'Setup changed user data.' }
    $Registered = Get-ItemProperty "HKCU:\$UninstallKey"
    if ($Registered.DisplayVersion -ne $Version) { throw 'Installed version is stale.' }

    $Cli = Join-Path $Target $Identity.cli_exe
    $Test = Start-Process -FilePath $Cli -ArgumentList @('--self-test', '--installed') `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Logs\self-test.out" `
        -RedirectStandardError "$Logs\self-test.err"
    $code = Wait-Exit $Test 120000 'Upgraded installed self-test'
    if ($code -ne 0) { throw "Upgraded installed self-test failed: $(Get-Content -Raw "$Logs\self-test.err")" }

    Run-Uninstall
    $Uninstalled = $true
    if (Test-Path -LiteralPath (Join-Path $Target $Identity.app_exe)) { throw 'Uninstall left the application behind.' }
    if (Test-Path "HKCU:\$UninstallKey") { throw 'Uninstall left its uninstall entry behind.' }
    if (-not (Test-Path -LiteralPath $Preserved)) { throw 'Uninstall removed retained user data.' }
    Write-Host 'Previous release -> clean setup upgrade -> installed self-test -> uninstall passed.'
} finally {
    if (-not $Uninstalled -and (Test-Path -LiteralPath (Join-Path $Target 'unins000.exe'))) {
        try { Run-Uninstall } catch { Write-Warning "Clean-up uninstall failed: $_" }
    }
    if (Test-Path -LiteralPath $Preserved) { Remove-Item -LiteralPath $Preserved -Force }
    if (-not $DataExisted -and (Test-Path -LiteralPath $Data) -and
        -not (Get-ChildItem -LiteralPath $Data -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $Data -Force
    }
    if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target -Recurse -Force -ErrorAction SilentlyContinue }
    Write-Host "Setup logs: $Logs"
}
