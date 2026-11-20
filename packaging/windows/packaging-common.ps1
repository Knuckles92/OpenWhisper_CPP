<#
.SYNOPSIS
    Helpers the packaging scripts share. Dot-source it: . "$PSScriptRoot\packaging-common.ps1"
#>

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Invoke-Native {
    <#
    .SYNOPSIS
        Run a native executable and fail on a non-zero exit code.
    .DESCRIPTION
        CMake, windeployqt and ISCC write progress to stderr. Under
        $ErrorActionPreference = 'Stop', Windows PowerShell wraps each stderr
        line in a NativeCommandError and aborts a perfectly successful build,
        so stderr is demoted to normal output here and success is judged by
        $LASTEXITCODE, which is the only reliable signal for a native command.
    #>
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][string]$ErrorMessage
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object { "$_" } | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "$ErrorMessage (exit code $LASTEXITCODE)"
        }
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Move-PreviousReleaseArtifacts([string]$Directory, [string]$Previous, [string]$Product,
                                       [string]$Version) {
    $name = [regex]::Escape($Product)
    $setup = "^$name-Setup-(?<version>\d+\.\d+\.\d+)\.exe$"
    $archive = "^$name-(?<version>\d+\.\d+\.\d+)-win64\.zip$"
    foreach ($artifact in Get-ChildItem -LiteralPath $Directory -File) {
        if (($artifact.Name -match $setup -or $artifact.Name -match $archive) -and
            $Matches['version'] -ne $Version) {
            New-Item -ItemType Directory -Force -Path $Previous | Out-Null
            Move-Item -LiteralPath $artifact.FullName -Destination (Join-Path $Previous $artifact.Name) -Force
            Write-Host "    moved $($artifact.Name) to $Previous"
        }
    }
}

function Format-Size([double]$Bytes) {
    if ($Bytes -ge 1GB) { return "{0:N2} GB" -f ($Bytes / 1GB) }
    if ($Bytes -ge 1MB) { return "{0:N1} MB" -f ($Bytes / 1MB) }
    return "{0:N0} KB" -f ($Bytes / 1KB)
}

function Get-TreeSize([string]$Path) {
    $items = Get-ChildItem -LiteralPath $Path -Recurse -File -ErrorAction SilentlyContinue
    if (-not $items) { return 0 }
    return ($items | Measure-Object -Property Length -Sum).Sum
}

function Find-Python {
    <#
    .SYNOPSIS
        The Python 3.12 interpreter the release tools run on: FIORIVOCE_PYTHON, then the
        py launcher's 3.12, then python on PATH when it is 3.12 or newer.
    #>
    if ($env:FIORIVOCE_PYTHON) {
        if (-not (Test-Path -LiteralPath $env:FIORIVOCE_PYTHON)) {
            throw "FIORIVOCE_PYTHON does not name a file: $env:FIORIVOCE_PYTHON"
        }
        return $env:FIORIVOCE_PYTHON
    }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if (Get-Command py -ErrorAction SilentlyContinue) {
            $found = & py -3.12 -c "import sys; print(sys.executable)" 2>$null
            if ($LASTEXITCODE -eq 0 -and $found) { return "$found".Trim() }
        }
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($python) {
            & $python.Source -c "import sys; sys.exit(sys.version_info < (3, 12))" 2>$null
            if ($LASTEXITCODE -eq 0) { return $python.Source }
        }
    } finally {
        $ErrorActionPreference = $previous
    }
    throw 'Python 3.12 not found. Install it, or set FIORIVOCE_PYTHON to its python.exe.'
}

function Get-PeImports([string]$Path) {
    <#
    .SYNOPSIS
        The DLL names a PE file imports, directly and delay-loaded, in table order.
    .DESCRIPTION
        Read from the file's import and delay-import directories, so no SDK tool is needed.
        Throws when the file is not a PE image.
    #>
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Not a Windows executable: $Path"
    }
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($pe -lt 0 -or $pe + 24 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $pe) -ne 0x00004550) {
        throw "Not a Windows executable: $Path"
    }
    $sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
    $optional = $pe + 24
    $magic = [BitConverter]::ToUInt16($bytes, $optional)
    # Data directories start 96 bytes into a PE32 optional header and 112 into PE32+.
    if ($magic -eq 0x20B) { $directories = $optional + 112 }
    elseif ($magic -eq 0x10B) { $directories = $optional + 96 }
    else { throw "Not a Windows executable: $Path" }
    $sectionTable = $optional + $optionalSize
    $sections = @()
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $at = $sectionTable + 40 * $i
        $sections += [pscustomobject]@{
            VirtualAddress = [BitConverter]::ToUInt32($bytes, $at + 12)
            Size = [Math]::Max([BitConverter]::ToUInt32($bytes, $at + 8),
                               [BitConverter]::ToUInt32($bytes, $at + 16))
            RawPointer = [BitConverter]::ToUInt32($bytes, $at + 20)
        }
    }
    $toOffset = {
        param([uint32]$Rva)
        foreach ($section in $sections) {
            if ($Rva -ge $section.VirtualAddress -and $Rva -lt $section.VirtualAddress + $section.Size) {
                return [int64]($Rva - $section.VirtualAddress + $section.RawPointer)
            }
        }
        return [int64]-1
    }
    $readName = {
        param([int64]$Offset)
        $end = $Offset
        while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
        return [Text.Encoding]::ASCII.GetString($bytes, $Offset, $end - $Offset)
    }
    $names = New-Object System.Collections.Generic.List[string]
    # (directory index, descriptor size, offset of the name RVA in a descriptor)
    foreach ($table in @(@(1, 20, 12), @(13, 32, 4))) {
        $entry = $directories + 8 * $table[0]
        if ($entry + 8 -gt $optional + $optionalSize) { continue }
        $rva = [BitConverter]::ToUInt32($bytes, $entry)
        if ($rva -eq 0) { continue }
        $offset = & $toOffset $rva
        if ($offset -lt 0) { continue }
        while ($offset + $table[1] -le $bytes.Length) {
            $nameRva = [BitConverter]::ToUInt32($bytes, $offset + $table[2])
            if ($nameRva -eq 0) { break }
            $nameOffset = & $toOffset $nameRva
            if ($nameOffset -ge 0) { $names.Add((& $readName $nameOffset)) }
            $offset += $table[1]
        }
    }
    return , $names.ToArray()
}

function Assert-InsideFolder([string]$Path, [string]$Folder, [string]$What) {
    # A folder the scripts delete must be one they own: inside the repository's out folder.
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $root = [IO.Path]::GetFullPath($Folder).TrimEnd('\') + '\'
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$What must be inside $root, not $full"
    }
    return $full
}
