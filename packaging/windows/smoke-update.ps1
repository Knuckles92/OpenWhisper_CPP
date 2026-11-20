<#
.SYNOPSIS
    The owner's real self-update check: install one release, update it natively to the next
    from a loopback feed, then watch a release that fails its health launch roll back.

.DESCRIPTION
    Written for M10.17 and never run by the workers who wrote it (no Windows there). It needs a
    person at the desktop: FioriVoce shows its window, and each update starts from the app's
    "Download and install".

    1. Installs <Artifacts>\FioriVoce-Setup-<From>.exe silently into a new temporary folder
       (/DIR, /CURRENTUSER) and starts it with LOCALAPPDATA pointing at a temporary folder too,
       so its settings, history and update work folder are not the user's (a data root from
       FIORIVOCE_DATA_DIR would make the install ineligible for a native update, A16-16).
    2. Serves FioriVoce-Setup-<To>.exe and FioriVoce-<To>-win64.zip with
       tools\release\serve_update_feed.py on 127.0.0.1. After the owner chooses "Download and
       install", it waits for the updated tree (its manifest names <To>), the uninstall entry's
       DisplayVersion <To>, the RunOnce recovery value cleared, the transaction folder removed,
       no apply_error.txt and the new app running.
    3. Asks the owner to quit, serves <Failing> and starts <To> again with
       FIORIVOCE_TEST_FAIL_HEALTH=1, which the updater passes on to the health launch of
       <Failing> (M10.12). After "Download and install" it waits for apply_error.txt, then for
       the tree, the uninstall entry and the running app to be <To> again.
    4. Asks the owner to quit, uninstalls silently and checks that the install folder is gone
       entirely: after a native update the tree holds files no setup logged, which
       [UninstallDelete] must remove.

    It installs software and registers the setup's uninstall entry under HKCU, so it refuses
    without -OnThisComputer, refuses on a CI runner (nobody is there to click), refuses when
    elevated (an elevated app is never offered a native update), refuses whenever FioriVoce is
    installed for this user or the machine, is running, or has an update recovery pending.
    Whatever happens it stops the feed and the apps it started, uninstalls what it installed,
    removes a RunOnce value only when it names its own temporary folder, and deletes that
    folder. As every uninstall does, the uninstall also empties %LOCALAPPDATA%\FioriVoce\updates
    (Inno resolves {localappdata} itself, not from the environment).

.PARAMETER Artifacts
    The folder holding the three releases' setups and archives, copied there after each
    build-installer.ps1 -Version run (a later build moves the earlier ones aside).

.PARAMETER From
    The version installed first (default 0.1.0).

.PARAMETER To
    The version the native update must land (default 0.1.1).

.PARAMETER Failing
    The version whose health launch must fail and roll back (default 0.1.2).

.PARAMETER SkipRollback
    Stop after the update to <To> lands (steps 1, 2 and 4).

.PARAMETER OnThisComputer
    Required: the run installs software on this computer.

.EXAMPLE
    .\packaging\windows\smoke-update.ps1 -Artifacts D:\fv-releases -OnThisComputer
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Artifacts,
    [string]$From = '0.1.0',
    [string]$To = '0.1.1',
    [string]$Failing = '0.1.2',
    [switch]$SkipRollback,
    [switch]$OnThisComputer
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\packaging-common.ps1"

if ($env:GITHUB_ACTIONS -eq 'true') {
    throw 'The self-update check needs a person to choose "Download and install"; it does not run on CI.'
}
if (-not $OnThisComputer) {
    throw 'This self-update check installs software. Pass -OnThisComputer to run it on this computer.'
}
foreach ($version in @($From, $To, $Failing)) {
    if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "Versions must be X.Y.Z, not '$version'" }
}
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this from a shell that is not elevated: an elevated FioriVoce is never offered a native update.'
}
$Artifacts = (Resolve-Path -LiteralPath $Artifacts).Path

$IdentityFile = Join-Path $PSScriptRoot '..\..\src\update\data\release_identity.json'
$Identity = Get-Content -Raw -LiteralPath $IdentityFile | ConvertFrom-Json
if ($Identity.app_id -notmatch '^\{[0-9A-F-]{36}\}$') { throw "Unexpected app_id in $IdentityFile" }
$Product = $Identity.setup_prefix -replace '-?Setup-?$', ''
$UninstallKey = "Software\Microsoft\Windows\CurrentVersion\Uninstall\$($Identity.app_id)_is1"
$RunOnceKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
# update/update_contract.cpp: runonce_value_name(), manifest_name(); kUpdatesDirname and the rest.
$RunOnceValue = "!$($Product)UpdateRecover"
$ManifestName = ".$($Product.ToLowerInvariant())-update.json"

function Release-File([string]$Name) {
    $path = Join-Path $Artifacts $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing $path" }
    return $path
}
$Releases = @{}
foreach ($version in @($From, $To) + $(if ($SkipRollback) { @() } else { @($Failing) })) {
    $Releases[$version] = @{
        Setup = Release-File "$($Identity.setup_prefix)$version.exe"
        Archive = Release-File "$Product-$version-win64.zip"
    }
}

foreach ($hive in @('HKCU', 'HKLM')) {
    if (Test-Path "$hive`:\$UninstallKey") {
        throw 'An existing installation is present; refusing to replace it.'
    }
}
if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Identity.app_exe)) -ErrorAction SilentlyContinue) {
    throw "Close every running $Product (installed or development copy) first."
}
if ((Get-ItemProperty -LiteralPath $RunOnceKey -ErrorAction SilentlyContinue).$RunOnceValue) {
    throw "An update recovery is pending ($RunOnceValue under $RunOnceKey); refusing to run."
}

# The failing release must honour the health-failure switch, or step 3 would install it for good.
function Test-FailHealthSwitch([string]$Archive) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq $Identity.app_exe } | Select-Object -First 1
        if (-not $entry) { throw "$Archive has no $($Identity.app_exe)" }
        $buffer = New-Object IO.MemoryStream
        $stream = $entry.Open()
        try { $stream.CopyTo($buffer) } finally { $stream.Dispose() }
        $bytes = $buffer.ToArray()
    } finally {
        $zip.Dispose()
    }
    $name = 'FIORIVOCE_TEST_FAIL_HEALTH'
    return ([Text.Encoding]::ASCII.GetString($bytes).Contains($name) -or
            [Text.Encoding]::Unicode.GetString($bytes).Contains($name))
}
if (-not $SkipRollback -and -not (Test-FailHealthSwitch $Releases[$Failing].Archive)) {
    throw "$Product $Failing does not know FIORIVOCE_TEST_FAIL_HEALTH (M10.12), so its health launch cannot be made to fail. Build it from a tree that has the switch, or pass -SkipRollback."
}

$Scratch = [IO.Path]::GetTempPath()
$RunId = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$Root = Join-Path $Scratch "$Product-update-smoke-$RunId"
# The install folder keeps the identity's name: the updater's .new-/.old-/.failed- siblings and
# the setup's .setup-backup land beside it, inside $Root.
$Target = Join-Path $Root $Identity.install_dir_name
$LocalRoot = Join-Path $Root 'LocalAppData'
$DataRoot = Join-Path $LocalRoot $Product
$Updates = Join-Path $DataRoot 'updates'
$ApplyError = Join-Path $Updates 'apply_error.txt'
$Logs = Join-Path $Scratch 'fiorivoce-update-logs'
New-Item -ItemType Directory -Force -Path $Root, $LocalRoot, $Logs | Out-Null
$Python = Find-Python
$FeedScript = Join-Path $PSScriptRoot '..\..\tools\release\serve_update_feed.py'

$SavedEnvironment = @{}
foreach ($name in @('LOCALAPPDATA', 'FIORIVOCE_DATA_DIR', 'FIORIVOCE_UPDATE_FEED_URL', 'FIORIVOCE_TEST_FAIL_HEALTH')) {
    $SavedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
function Restore-Environment {
    foreach ($name in $SavedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $SavedEnvironment[$name], 'Process')
    }
}
# Child processes (the setups and their helper, the app, the updater it starts, the health
# launch) inherit this: their data root is $DataRoot, the default for that LOCALAPPDATA.
function Use-SmokeEnvironment([string]$FeedUrl, [bool]$FailHealth) {
    $env:LOCALAPPDATA = $LocalRoot
    Remove-Item Env:FIORIVOCE_DATA_DIR -ErrorAction SilentlyContinue
    if ($FeedUrl) { $env:FIORIVOCE_UPDATE_FEED_URL = $FeedUrl } else { Remove-Item Env:FIORIVOCE_UPDATE_FEED_URL -ErrorAction SilentlyContinue }
    if ($FailHealth) { $env:FIORIVOCE_TEST_FAIL_HEALTH = '1' } else { Remove-Item Env:FIORIVOCE_TEST_FAIL_HEALTH -ErrorAction SilentlyContinue }
}

function Wait-Exit([Diagnostics.Process]$Process, [int]$Milliseconds, [string]$Name) {
    if (-not $Process.WaitForExit($Milliseconds)) {
        $Process.Kill()
        throw "$Name timed out"
    }
    return $Process.ExitCode
}

function Wait-Until([scriptblock]$Condition, [int]$Seconds, [string]$What, [scriptblock]$FailIf) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if ($FailIf) { $failure = & $FailIf; if ($failure) { throw $failure } }
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 500
    }
    throw "Timed out after $Seconds s waiting for $What."
}

function App-Processes {
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Identity.app_exe)) -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) }
}

function Installed-Version {
    try {
        return (Get-Content -Raw -LiteralPath (Join-Path $Target $ManifestName) | ConvertFrom-Json).version
    } catch {
        return $null  # absent while the updater swaps the trees
    }
}

function Registered-Version {
    return (Get-ItemProperty -LiteralPath "HKCU:\$UninstallKey" -ErrorAction SilentlyContinue).DisplayVersion
}

function Recovery-Pending {
    return [bool](Get-ItemProperty -LiteralPath $RunOnceKey -ErrorAction SilentlyContinue).$RunOnceValue
}

function Open-Transactions {
    $tx = Join-Path $Updates 'tx'
    if (-not (Test-Path -LiteralPath $tx)) { return 0 }
    return @(Get-ChildItem -LiteralPath $tx -Directory -Force).Count
}

$Feed = $null
function Start-Feed([string]$Version) {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    $script:Feed = Start-Process -FilePath $Python -ArgumentList @(
        '-I', '-B', "`"$FeedScript`"", '--setup', "`"$($Releases[$Version].Setup)`"",
        '--archive', "`"$($Releases[$Version].Archive)`"", '--version', $Version,
        '--host', '127.0.0.1', '--port', $port
    ) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$Logs\feed-$Version.out" `
        -RedirectStandardError "$Logs\feed-$Version.err"
    $url = "http://127.0.0.1:$port/releases/latest"
    Wait-Until {
        try { (Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200 } catch { $false }
    } 30 "the feed for $Version" { if ($script:Feed.HasExited) { "The feed for $Version stopped: $(Get-Content -Raw "$Logs\feed-$Version.err")" } }
    return $url
}
function Stop-Feed {
    if ($script:Feed -and -not $script:Feed.HasExited) { $script:Feed.Kill(); $script:Feed.WaitForExit(10000) | Out-Null }
    $script:Feed = $null
}

function Run-Setup([string]$Installer, [string]$Name) {
    Use-SmokeEnvironment $null $false
    $process = Start-Process -FilePath $Installer -ArgumentList @(
        '/SP-', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
        '/CURRENTUSER', "/DIR=`"$Target`"", "/LOG=`"$Logs\$Name.log`""
    ) -WindowStyle Hidden -PassThru
    $code = Wait-Exit $process 300000 $Name
    if ($code -ne 0) { throw "$Name failed: $code" }
}

function Start-App([string]$FeedUrl, [bool]$FailHealth) {
    Use-SmokeEnvironment $FeedUrl $FailHealth
    Start-Process -FilePath (Join-Path $Target $Identity.app_exe) | Out-Null
    Wait-Until { [bool](App-Processes) } 60 "$Product to start"
}

function Wait-Quit {
    Write-Host "Quit $Product from its tray menu (Quit, not Close)." -ForegroundColor Yellow
    Wait-Until { -not (App-Processes) } 600 "$Product to quit"
}

function Run-Uninstall {
    $uninstaller = Join-Path $Target 'unins000.exe'
    $process = Start-Process -FilePath $uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$Logs\uninstall.log`""
    ) -WindowStyle Hidden -PassThru
    $code = Wait-Exit $process 120000 'Uninstall'
    if ($code -ne 0) { throw "Uninstall failed: $code" }
    # The uninstaller finishes from a temporary copy after this process has exited.
    $deadline = (Get-Date).AddSeconds(30)
    while ((Test-Path -LiteralPath $Target) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
}

$Uninstalled = $false
try {
    Write-Host "Install $Product $From into $Target"
    Run-Setup $Releases[$From].Setup "setup-$From"
    if ((Installed-Version) -ne $From -or (Registered-Version) -ne $From) { throw "Setup did not install $From." }

    Write-Host "Update $From -> $To"
    $url = Start-Feed $To
    Start-App $url $false
    Write-Host "In $Product, choose `"Update to $To`", then `"Download and install`" (skip the guided setup if it opens)." -ForegroundColor Yellow
    Wait-Until {
        (Installed-Version) -eq $To -and (Registered-Version) -eq $To -and -not (Recovery-Pending) -and
            (Open-Transactions) -eq 0 -and [bool](App-Processes)
    } 900 "the update to $To to land" {
        if (Test-Path -LiteralPath $ApplyError) { "The update to $To failed: $(Get-Content -Raw -LiteralPath $ApplyError)" }
    }
    Stop-Feed
    Write-Host "Updated to ${To}: tree, uninstall entry, recovery value and transaction folder are clean."

    if (-not $SkipRollback) {
        Write-Host "Update $To -> $Failing, whose health launch fails"
        Wait-Quit
        $url = Start-Feed $Failing
        Start-App $url $true
        Write-Host "In $Product, choose `"Update to $Failing`", then `"Download and install`"." -ForegroundColor Yellow
        Wait-Until { Test-Path -LiteralPath $ApplyError } 900 "the update to $Failing to fail" {
            if ((Installed-Version) -eq $Failing -and (Registered-Version) -eq $Failing -and
                (Open-Transactions) -eq 0 -and -not (Recovery-Pending)) {
                "$Failing was installed: its health launch did not fail, so nothing rolled back."
            }
        }
        Copy-Item -LiteralPath $ApplyError -Destination "$Logs\apply_error.txt" -Force
        Wait-Until {
            (Installed-Version) -eq $To -and (Registered-Version) -eq $To -and -not (Recovery-Pending) -and
                [bool](App-Processes)
        } 300 "the rollback to $To"
        Stop-Feed
        Write-Host "Rolled back to $To. The updater's reason: $(Get-Content -Raw -LiteralPath $ApplyError)"
    }

    Wait-Quit
    Run-Uninstall
    $Uninstalled = $true
    if (Test-Path "HKCU:\$UninstallKey") { throw 'Uninstall left its uninstall entry behind.' }
    if (Test-Path -LiteralPath $Target) {
        $left = (Get-ChildItem -LiteralPath $Target -Force | ForEach-Object Name) -join ', '
        throw "Uninstall after a native update left the install folder behind: $left"
    }
    if (Recovery-Pending) { throw 'An update recovery value is still pending.' }
    Write-Host "Install $From -> native update to $To$(if (-not $SkipRollback) { " -> failed $Failing rolled back" }) -> uninstall passed."
} finally {
    Stop-Feed
    App-Processes | ForEach-Object { try { $_.Kill(); $_.WaitForExit(10000) | Out-Null } catch {} }
    Restore-Environment
    if (-not $Uninstalled -and (Test-Path -LiteralPath (Join-Path $Target 'unins000.exe'))) {
        try { Run-Uninstall } catch { Write-Warning "Clean-up uninstall failed: $_" }
    }
    $pending = (Get-ItemProperty -LiteralPath $RunOnceKey -ErrorAction SilentlyContinue).$RunOnceValue
    if ($pending -and $pending.IndexOf($Root, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        Remove-ItemProperty -LiteralPath $RunOnceKey -Name $RunOnceValue -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue }
    Write-Host "Logs: $Logs"
}
