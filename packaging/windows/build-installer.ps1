<#
.SYNOPSIS
    Build the FioriVoce Windows installer.

.DESCRIPTION
    Configures and builds the release configuration, stages the install tree (stage.ps1:
    programs, vcpkg DLLs, windeployqt, app-local C++ runtime, notices), checks the stage with
    `fiorivoce-cli --self-test --installed` and the workflow smoke against the release health
    gate, signs the executables when a signing method is configured, compiles the setup with
    Inno Setup, verifies the signatures and writes BUILD-METADATA.json and SHA256SUMS.txt.

    Output, in out\installer:
        FioriVoce-Setup-<version>.exe, FioriVoce-<version>-win64.zip, BUILD-METADATA.json,
        SHA256SUMS.txt, windows-signing.json
    A setup of another version found there is moved to out\installer-previous first, because a
    release candidate folder holds exactly one version.

    Prerequisites:
        Visual Studio 2022 with the C++ tools, vcpkg and the Qt kit (scripts\setup-windows.ps1)
        Python 3.12 (the release tools; FIORIVOCE_PYTHON names another interpreter)
        Inno Setup 6 (winget install -e --id JRSoftware.InnoSetup); found through FIORIVOCE_ISCC,
        then PATH, then %LOCALAPPDATA%\Programs\Inno Setup 6, then %ProgramFiles(x86)%\Inno Setup 6

    Code signing is optional and off by default; tools\release\README.md lists the
    FIORIVOCE_SIGN_* variables. Unsigned, every signing step says so and passes.

.PARAMETER Version
    The product version, X.Y.Z (default: the one in CMakeLists.txt). It reaches CMake as
    -DFIORIVOCE_VERSION, so the executables report it.

.PARAMETER Clean
    Delete the stage and out\installer, and rebuild the configuration from scratch.

.PARAMETER SkipInstaller
    Stage and check only; do not run Inno Setup.

.PARAMETER Preset
    The CMake configure preset whose RelWithDebInfo tree is built and staged: windows (the
    windows-release build preset; the default) or windows-ci.

.PARAMETER SourceQualification
    passed when the full test suite ran green on this commit (CI), else not-run (the default).

.PARAMETER RequireQtLicenses
    Fail when the Qt kit has no licence files. Pass it for a build that ships.

.EXAMPLE
    .\packaging\windows\build-installer.ps1 -Clean
.EXAMPLE
    .\packaging\windows\build-installer.ps1 -Version 0.1.1
#>
[CmdletBinding()]
param(
    [string]$Version,
    [switch]$Clean,
    [switch]$SkipInstaller,
    [ValidateSet('windows', 'windows-ci')][string]$Preset = 'windows',
    [ValidateSet('passed', 'not-run')][string]$SourceQualification = 'not-run',
    [switch]$RequireQtLicenses
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\packaging-common.ps1"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $RepoRoot

$OutRoot = Join-Path $RepoRoot 'out'
$BuildPresets = @{ 'windows' = 'windows-release'; 'windows-ci' = 'windows-ci' }
$BuildPreset = $BuildPresets[$Preset]
$Config = 'RelWithDebInfo'
$BinaryDir = Join-Path $OutRoot "build\$Preset"
$Stage = Join-Path $OutRoot 'stage'
$InstallerDir = Join-Path $OutRoot 'installer'
$PreviousDir = Join-Path $OutRoot 'installer-previous'
$HealthReport = Join-Path $OutRoot 'release-health.json'
$ReleaseTools = Join-Path $RepoRoot 'tools\release'

# The version: -Version, else CMakeLists.txt's default. Always passed to CMake, so a tree a
# previous -Version run configured does not keep that version in its cache.
if (-not $Version) {
    $line = Select-String -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt') `
        -Pattern 'set\(FIORIVOCE_VERSION "(\d+\.\d+\.\d+)" CACHE' | Select-Object -First 1
    if (-not $line) { throw 'Could not read the default version from CMakeLists.txt' }
    $Version = $line.Matches[0].Groups[1].Value
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must be X.Y.Z, not '$Version'" }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or -not $env:VCToolsRedistDir) {
    # The Visual Studio developer shell points VCPKG_ROOT at its bundled copy, and vsenv.ps1
    # puts back only a VCPKG_ROOT registered for the user account. A VCPKG_ROOT this process
    # already has (a CI runner's clone at the manifest's baseline) is the one to build with.
    $vcpkgRoot = $env:VCPKG_ROOT
    . (Join-Path $RepoRoot 'scripts\vsenv.ps1')
    if ($vcpkgRoot) { $env:VCPKG_ROOT = $vcpkgRoot }
}
Write-Step "Building the installer for version $Version"
$Python = Find-Python
Write-Host "    Python: $Python"
# The product and the setup's file name, from the release identity the app embeds.
$names = @(& $Python -I -B -c "import sys; sys.path.insert(0, sys.argv[1]); import release_common as c; i = c.load_identity(); print(c.product_name(i)); print(c.setup_name(i, sys.argv[2])); print(c.archive_name(i, sys.argv[2]))" $ReleaseTools $Version)
if ($LASTEXITCODE -ne 0 -or $names.Count -ne 3) { throw 'Could not read the release identity' }
$Product = "$($names[0])".Trim()
$SetupName = "$($names[1])".Trim()
$ArchiveName = "$($names[2])".Trim()

$Iscc = $null
if (-not $SkipInstaller) {
    $candidates = @()
    if ($env:FIORIVOCE_ISCC) { $candidates += $env:FIORIVOCE_ISCC }
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }
    $candidates += "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    if (${env:ProgramFiles(x86)}) { $candidates += "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
    $Iscc = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if ($env:FIORIVOCE_ISCC -and $Iscc -ne $env:FIORIVOCE_ISCC) {
        throw "FIORIVOCE_ISCC does not name ISCC.exe: $env:FIORIVOCE_ISCC"
    }
    if ($Iscc) { Write-Host "    Inno Setup: $Iscc" }
}

if ($Clean) {
    Write-Step 'Cleaning previous packaging output'
    foreach ($dir in @($Stage, $InstallerDir)) {
        if (Test-Path -LiteralPath $dir) {
            Remove-Item -LiteralPath $dir -Recurse -Force
            Write-Host "    removed $dir"
        }
    }
}

Write-Step "Configuring ($Preset, FIORIVOCE_VERSION=$Version)"
Invoke-Native cmake @('--preset', $Preset, "-DFIORIVOCE_VERSION=$Version") `
    -ErrorMessage 'CMake configure failed'

Write-Step "Building fiorivoce and fiorivoce-cli ($BuildPreset)"
$build = @('--build', '--preset', $BuildPreset, '--target', 'fiorivoce', 'fiorivoce-cli', 'fiorivoce-updater')
if ($Clean) { $build += '--clean-first' }
Invoke-Native cmake $build -ErrorMessage 'Build failed'

$stageArgs = @{ BinaryDir = $BinaryDir; Config = $Config; Stage = $Stage }
if ($RequireQtLicenses) { $stageArgs.RequireQtLicenses = $true }
& (Join-Path $PSScriptRoot 'stage.ps1') @stageArgs

Write-Step 'Verifying the stage'
$Cli = Join-Path $Stage 'fiorivoce-cli.exe'
$App = Join-Path $Stage 'fiorivoce.exe'
$reported = (& $Cli --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $reported -ne "$Product $Version") {
    throw "fiorivoce-cli --version reports '$reported', not '$Product $Version'"
}
Write-Host "    $reported"
# Every file the programs need is in the stage: the import walk, the Qt platform, windows11
# style, SVG and TLS plugins, the C++ runtime and the notices (windeployqt decides the rest).
Invoke-Native $Cli @('--self-test', '--installed') -ErrorMessage 'Installed self-test failed'
Invoke-Native $App @('--self-test-gui') -ErrorMessage 'Staged offscreen Qt self-test failed'
# Every top-level name the setup will install is one [InstallDelete] clears on an upgrade, so a
# file or plugin folder a later version stops shipping cannot outlive it.
Invoke-Native $Python @('-I', '-B', (Join-Path $PSScriptRoot 'inno_defines.py'), '--check-stage', '--source', $Stage) `
    -ErrorMessage 'The stage holds a name FioriVoce.iss does not manage'
# The headless lifecycle against the release health gate, from the staged programs. It works in
# a temporary folder of its own and never touches the data folder.
Invoke-Native $Python @(
    '-I', '-B', (Join-Path $ReleaseTools 'check_release_health.py'),
    '--executable', $Cli, '--output', $HealthReport, '--timeout', '45'
) -ErrorMessage 'Release health gate failed'
Write-Host "    release health passed ($HealthReport)" -ForegroundColor Green

$Updater = Join-Path $Stage 'fiorivoce-updater.exe'
# Signing changes the image digest, so generate the installed manifest afterwards.
Write-Step 'Code signing the executables'
Invoke-Native $Python @('-I', '-B', (Join-Path $ReleaseTools 'windows_signing.py'), 'sign', $App, $Cli, $Updater) `
    -ErrorMessage 'Signing the executables failed'
Invoke-Native $Updater @('--write-manifest', $Stage, '--version', $Version) -ErrorMessage 'Writing the installed release manifest failed'
# Empty when unsigned; otherwise Inno signs the uninstaller and the setup exe.
$InnoSignArgs = @(& $Python -I -B (Join-Path $ReleaseTools 'windows_signing.py') inno-args)
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare installer signing' }

$StageSize = Get-TreeSize $Stage
if ($SkipInstaller -or -not $Iscc) {
    Write-Step 'Done (installer skipped)'
    Write-Host "    Stage   : $Stage ($(Format-Size $StageSize))"
    Write-Host "    Version : $Version"
    if ($SkipInstaller) { exit 0 }
    Write-Host ''
    Write-Host '    Inno Setup was not found, so the installer step was skipped. Install it with' -ForegroundColor Yellow
    Write-Host '    winget install -e --id JRSoftware.InnoSetup, set FIORIVOCE_ISCC, or pass -SkipInstaller.' -ForegroundColor Yellow
    exit 1
}

Write-Step 'Preparing the release candidate folder'
New-Item -ItemType Directory -Force -Path $InstallerDir | Out-Null

Move-PreviousReleaseArtifacts $InstallerDir $PreviousDir $Product $Version
foreach ($generated in @('BUILD-METADATA.json', 'SHA256SUMS.txt', 'windows-signing.json', $SetupName, $ArchiveName)) {
    $path = Join-Path $InstallerDir $generated
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}

Write-Step 'Building the installer with Inno Setup'
$Defines = @(& $Python -I -B (Join-Path $PSScriptRoot 'inno_defines.py') `
        --version $Version --source $Stage --output-dir $InstallerDir)
if ($LASTEXITCODE -ne 0 -or $Defines.Count -eq 0) { throw 'Could not read the release identity for Inno Setup' }
Invoke-Native $Iscc (@('/Q') + $Defines + $InnoSignArgs + @((Join-Path $PSScriptRoot 'FioriVoce.iss'))) `
    -ErrorMessage 'Inno Setup failed'
$SetupPath = Join-Path $InstallerDir $SetupName
if (-not (Test-Path -LiteralPath $SetupPath)) { throw "Installer not found at $SetupPath" }

# Every signature must verify, chain to a trusted root and carry a timestamp; an unsigned build
# must not carry a stray signature either.
Write-Step 'Verifying code signatures'
$SigningReport = Join-Path $InstallerDir 'windows-signing.json'
Invoke-Native $Python @(
    '-I', '-B', (Join-Path $ReleaseTools 'windows_signing.py'), 'verify', '--report', $SigningReport,
    $App, $Cli, $Updater, $SetupPath
) -ErrorMessage 'Code signature verification failed'

Write-Step 'Packing the native update archive'
$ArchivePath = Join-Path $InstallerDir $ArchiveName
Invoke-Native $Updater @('--pack-archive', $Stage, '--version', $Version, '--out', $ArchivePath) -ErrorMessage 'Packing the native update archive failed'
Invoke-Native $Updater @('--verify-archive', $ArchivePath, '--version', $Version) -ErrorMessage 'Native update archive verification failed'

Write-Step 'Writing build metadata and checksums'
$Commit = (& git -C $RepoRoot rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $Commit -notmatch '^[0-9a-f]{40}$') { throw 'Could not read the commit with git' }
$dirty = (& git -C $RepoRoot status --porcelain --untracked-files=no | Out-String).Trim()
if ($dirty) {
    Write-Host "    the working tree has uncommitted changes: the metadata names $Commit, not them" -ForegroundColor Yellow
}
$metadata = @(
    '-I', '-B', (Join-Path $ReleaseTools 'build_metadata.py'), '--dir', $InstallerDir,
    '--version', $Version, '--commit', $Commit, '--signing', $SigningReport,
    '--source-qualification', $SourceQualification
)
# On a CI runner the candidate records the run that built it.
if ($env:GITHUB_ACTIONS -eq 'true') {
    $metadata += @('--ref', $env:GITHUB_REF, '--repository', $env:GITHUB_REPOSITORY,
        '--run-id', $env:GITHUB_RUN_ID, '--run-attempt', $env:GITHUB_RUN_ATTEMPT,
        '--run-url', "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID")
}
Invoke-Native $Python $metadata -ErrorMessage 'Build metadata failed'

$SetupSize = (Get-Item -LiteralPath $SetupPath).Length
$Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $SetupPath).Hash.ToLower()

Write-Step 'Build complete'
Write-Host ''
Write-Host "  Installer : $SetupPath"
Write-Host "  Version   : $Version"
Write-Host "  Commit    : $Commit"
Write-Host "  Setup     : $(Format-Size $SetupSize)  (unpacked $(Format-Size $StageSize))"
Write-Host "  SHA-256   : $Hash"
Write-Host ''
Write-Host '  The candidate folder holds the setup, native archive, BUILD-METADATA.json,'
Write-Host '  SHA256SUMS.txt and windows-signing.json.'
Write-Host ''
