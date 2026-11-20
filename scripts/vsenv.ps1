<#
.SYNOPSIS
    Loads the MSVC x64 build environment (cl, link, ninja) into this PowerShell
    session. VS Code's "MSVC Dev Shell" terminal profile runs it for you; in any
    other terminal, run it once per session:  . .\scripts\vsenv.ps1
#>
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) {
    Write-Error 'No Visual Studio install with the C++ tools was found. Run scripts\setup-windows.ps1.'
    return
}

# The dev shell calls vswhere itself and complains when it is not on PATH.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
Import-Module -Name "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# The dev shell points VCPKG_ROOT at the copy bundled with Visual Studio. Prefer
# the standalone clone that setup-windows.ps1 registered for your account.
$userVcpkg = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
if ($userVcpkg) { $env:VCPKG_ROOT = $userVcpkg }

# Pick up Qt6_ROOT even in a terminal opened before setup-windows.ps1 set it.
$userQt = [Environment]::GetEnvironmentVariable('Qt6_ROOT', 'User')
if ($userQt -and -not $env:Qt6_ROOT) { $env:Qt6_ROOT = $userQt }

Write-Host "MSVC x64 environment loaded. VCPKG_ROOT=$env:VCPKG_ROOT"
