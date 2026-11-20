<#
.SYNOPSIS
    One-time Windows setup for FioriVoce development. Safe to re-run.

.DESCRIPTION
    Checks for the compiler and CMake, clones vcpkg, sets VCPKG_ROOT for your
    user account, and installs the recommended VS Code extensions.

.PARAMETER VcpkgRoot
    Where vcpkg lives. Defaults to the existing VCPKG_ROOT, or C:\vcpkg.

.PARAMETER QtRoot
    A Qt 6 MSVC kit, such as C:\Qt\6.10.3\msvc2022_64. Defaults to the existing
    Qt6_ROOT, or the newest kit under C:\Qt or D:\Qt.
#>
param(
    [string]$VcpkgRoot = $(if ([Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')) {
            [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
        } else { 'C:\vcpkg' }),
    [string]$QtRoot = [Environment]::GetEnvironmentVariable('Qt6_ROOT', 'User')
)

$ErrorActionPreference = 'Stop'
$missing = @()

# --- Compiler: Visual Studio (or Build Tools) with the C++ workload ----------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ($vs) {
    Write-Host "[ok] MSVC: $vs"
} else {
    $missing += 'MSVC  ->  winget install Microsoft.VisualStudio.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"'
}

# --- CMake and Git -----------------------------------------------------------
foreach ($tool in @(@{ Name = 'cmake'; Id = 'Kitware.CMake' }, @{ Name = 'git'; Id = 'Git.Git' })) {
    if (Get-Command $tool.Name -ErrorAction SilentlyContinue) {
        Write-Host "[ok] $($tool.Name)"
    } else {
        $missing += "$($tool.Name)  ->  winget install $($tool.Id)"
    }
}

if ($missing) {
    Write-Host "`nInstall these first, then run this script again:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  $_" }
    exit 1
}

# --- vcpkg -------------------------------------------------------------------
if (-not (Test-Path "$VcpkgRoot\.git")) {
    Write-Host "Cloning vcpkg into $VcpkgRoot ..."
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
}
if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
    & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
}
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', $VcpkgRoot, 'User')
$env:VCPKG_ROOT = $VcpkgRoot
Write-Host "[ok] vcpkg: $VcpkgRoot (VCPKG_ROOT set for your user account)"

# --- Qt 6 (optional: only the desktop app needs it) --------------------------
# Qt is a prebuilt install, not a vcpkg port (vcpkg would compile it from
# source). CMake finds it through the Qt6_ROOT environment variable.
if (-not $QtRoot) {
    $QtRoot = Get-ChildItem 'C:\Qt', 'D:\Qt' -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^6\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'msvc*_64' } |
        Select-Object -First 1 -ExpandProperty FullName
}
if ($QtRoot -and (Test-Path "$QtRoot\lib\cmake\Qt6")) {
    [Environment]::SetEnvironmentVariable('Qt6_ROOT', $QtRoot, 'User')
    $env:Qt6_ROOT = $QtRoot
    Write-Host "[ok] Qt: $QtRoot (Qt6_ROOT set for your user account)"
} else {
    Write-Host "[skip] Qt 6 not found: the engine, CLI and their tests still build, the desktop app does not."
    Write-Host "       To add it:  pip install aqtinstall;  aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 -O C:\Qt"
    Write-Host "       then run this script again (or pass -QtRoot <folder containing lib\cmake\Qt6>)."
}

# --- VS Code extensions ------------------------------------------------------
if (Get-Command code -ErrorAction SilentlyContinue) {
    $recommended = (Get-Content "$PSScriptRoot\..\.vscode\extensions.json" -Raw | ConvertFrom-Json).recommendations
    $installed = code --list-extensions
    foreach ($ext in $recommended) {
        if ($installed -notcontains $ext) { code --install-extension $ext | Out-Null }
        Write-Host "[ok] VS Code extension: $ext"
    }
} else {
    Write-Host "[skip] VS Code not on PATH; install the extensions in .vscode\extensions.json by hand"
}

Write-Host "`nDone. Open a NEW terminal so VCPKG_ROOT is visible, then see BUILDING.md." -ForegroundColor Green
