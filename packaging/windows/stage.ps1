<#
.SYNOPSIS
    Stage the FioriVoce install tree from a build tree.

.DESCRIPTION
    Copies the desktop, CLI and native archive helper with the vcpkg DLLs they load (followed through
    their import tables) from the build tree's runtime folder, lets windeployqt add the Qt DLLs
    and plugins, adds the app-local Visual C++ runtime and the meeting dashboard bundle
    (webui\dist) and writes THIRD_PARTY_NOTICES.md and licenses\. The result is exactly what the setup installs into {app}.

    Prerequisites: an MSVC environment (. .\scripts\vsenv.ps1) for VCToolsRedistDir, a built
    RelWithDebInfo tree, and Python 3.12 for the notice generator.

.PARAMETER BinaryDir
    The CMake build tree (default out\build\windows, the windows-release preset's).

.PARAMETER Config
    The configuration whose executables are staged (default RelWithDebInfo).

.PARAMETER Stage
    Where the install tree goes (default out\stage). It is deleted first, so it must be inside
    the repository's out folder.

.PARAMETER QtRoot
    The Qt kit whose windeployqt runs (default Qt6_ROOT, else D:\Qt\6.10.3\msvc2022_64).

.PARAMETER RequireQtLicenses
    Fail when the Qt kit has no licence files (pass it for a build that ships).

.EXAMPLE
    .\packaging\windows\stage.ps1 -BinaryDir out\build\windows
#>
[CmdletBinding()]
param(
    [string]$BinaryDir,
    [string]$Config = 'RelWithDebInfo',
    [string]$Stage,
    [string]$QtRoot,
    [switch]$RequireQtLicenses
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\packaging-common.ps1"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$OutRoot = Join-Path $RepoRoot 'out'

if (-not $BinaryDir) { $BinaryDir = Join-Path $OutRoot 'build\windows' }
if (-not $Stage) { $Stage = Join-Path $OutRoot 'stage' }
# Relative paths are the repository's; Combine keeps an absolute one as it is.
$BinaryDir = [IO.Path]::GetFullPath([IO.Path]::Combine($RepoRoot, $BinaryDir))
$Stage = Assert-InsideFolder ([IO.Path]::Combine($RepoRoot, $Stage)) $OutRoot 'The stage'

$Bin = Join-Path $BinaryDir "bin\$Config"
# The executables and DLLs the stage takes from the build tree, not from windeployqt.
$Programs = @('fiorivoce.exe', 'fiorivoce-cli.exe', 'fiorivoce-updater.exe')
foreach ($program in $Programs) {
    if (-not (Test-Path -LiteralPath (Join-Path $Bin $program))) {
        throw "$program is not in $Bin. Build the $Config configuration first."
    }
}

if (-not $QtRoot) { $QtRoot = $env:Qt6_ROOT }
if (-not $QtRoot) { $QtRoot = 'D:\Qt\6.10.3\msvc2022_64' }
$WinDeployQt = Join-Path $QtRoot 'bin\windeployqt.exe'
if (-not (Test-Path -LiteralPath $WinDeployQt)) {
    throw "windeployqt.exe not found in $QtRoot\bin. Set Qt6_ROOT to the Qt kit."
}

if (-not $env:VCToolsRedistDir) {
    throw 'VCToolsRedistDir is not set. Run . .\scripts\vsenv.ps1 first.'
}
$Crt = Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory `
    -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $Crt) {
    throw "No Microsoft.VC*.CRT folder in $env:VCToolsRedistDir\x64. Install the MSVC redistributable component."
}

$Python = Find-Python
$Triplet = 'x64-windows'
$VcpkgInstalled = Join-Path $BinaryDir 'vcpkg_installed'
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgInstalled $Triplet))) {
    throw "No vcpkg_installed\$Triplet in $BinaryDir. Configure the build tree first."
}

Write-Step "Staging the install tree into $Stage"
if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

# The programs, then every DLL beside them in the build tree that they import, directly or
# through another such DLL. The build folder also holds the test programs' DLLs; only what the
# two programs reach is staged. Qt's DLLs are windeployqt's to choose.
$pending = New-Object System.Collections.Generic.Queue[string]
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
foreach ($program in $Programs) { $pending.Enqueue($program) }
while ($pending.Count -gt 0) {
    $name = $pending.Dequeue()
    if (-not $seen.Add($name)) { continue }
    $source = Join-Path $Bin $name
    Copy-Item -LiteralPath $source -Destination $Stage
    foreach ($import in (Get-PeImports $source)) {
        if ($import -match '^Qt6') { continue }
        if (Test-Path -LiteralPath (Join-Path $Bin $import) -PathType Leaf) {
            $pending.Enqueue($import)
        }
    }
}
$copied = @($seen | Where-Object { $_ -notin $Programs } | Sort-Object)
Write-Host "    $($Programs -join ', ') and $($copied.Count) libraries from the build tree"
foreach ($dll in $copied) { Write-Host "      $dll" }

Write-Step 'Deploying Qt with windeployqt'
# --release: the kit's release DLLs for a RelWithDebInfo build. The C++ runtime is staged from
# the redistributable below, which also covers the non-Qt DLLs.
Invoke-Native $WinDeployQt @(
    '--release', '--no-translations', '--no-compiler-runtime', '--include-plugins', 'qoffscreen',
    '--verbose', '1',
    (Join-Path $Stage 'fiorivoce.exe')
) -ErrorMessage 'windeployqt failed'

Write-Step "Adding the app-local C++ runtime ($($Crt.Name))"
foreach ($dll in (Get-ChildItem -LiteralPath $Crt.FullName -Filter '*.dll')) {
    Copy-Item -LiteralPath $dll.FullName -Destination $Stage
    Write-Host "      $($dll.Name)"
}

Write-Step 'Adding the meeting dashboard bundle (webui\dist)'
# The dashboard server serves <folder of fiorivoce.exe>\webui\dist. The committed bundle is
# taken from the source tree, not the build's copy of it.
$WebuiDist = Join-Path $RepoRoot 'webui\dist'
if (-not (Test-Path -LiteralPath (Join-Path $WebuiDist 'index.html'))) {
    throw "No index.html in $WebuiDist."
}
$WebuiStage = Join-Path $Stage 'webui'
New-Item -ItemType Directory -Force -Path $WebuiStage | Out-Null
Copy-Item -LiteralPath $WebuiDist -Destination $WebuiStage -Recurse
Write-Host "      webui\dist ($(Format-Size (Get-TreeSize (Join-Path $WebuiStage 'dist'))))"

Write-Step 'Writing the third-party notices'
$notices = @(
    '-I', '-B', (Join-Path $RepoRoot 'tools\release\generate_notices.py'),
    '--vcpkg-installed', $VcpkgInstalled, '--triplet', $Triplet,
    '--qt', $QtRoot, '--out', $Stage
)
if ($RequireQtLicenses) { $notices += '--require-qt-licenses' }
Invoke-Native $Python $notices -ErrorMessage 'Generating the third-party notices failed'

# A debug Qt or C++ runtime DLL means the wrong kit or configuration was deployed: the release
# programs would load two runtimes, or fail on a machine without Visual Studio.
$debug = @(Get-ChildItem -LiteralPath $Stage -Recurse -File -Include 'Qt6*d.dll', '*140d.dll', 'ucrtbased.dll' |
    Where-Object { $_.Name -match '^(Qt6\w+d|\w+140d|ucrtbased)\.dll$' })
if ($debug.Count -gt 0) {
    throw "Debug libraries in the stage: $(($debug | ForEach-Object Name) -join ', ')"
}

Write-Host ""
Write-Host "    Stage: $Stage ($(Format-Size (Get-TreeSize $Stage)))" -ForegroundColor Green
