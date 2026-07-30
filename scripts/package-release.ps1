[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$X64BuildDirectory = 'build',
    [string]$X86BuildDirectory = 'build-x86'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

function Resolve-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $root $Path))
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'dist'
}
$OutputDirectory = Resolve-ProjectPath $OutputDirectory
$X64BuildDirectory = Resolve-ProjectPath $X64BuildDirectory
$X86BuildDirectory = Resolve-ProjectPath $X86BuildDirectory

$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$versionMatch = [regex]::Match($cmake, 'project\(MMIDownloader VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) {
    throw 'Could not determine the project version from CMakeLists.txt'
}
$version = $versionMatch.Groups[1].Value

function Read-BuildNumber([string]$Header) {
    if (-not (Test-Path -LiteralPath $Header -PathType Leaf)) {
        throw "Missing generated version header: $Header"
    }
    $match = [regex]::Match((Get-Content -LiteralPath $Header -Raw), '#define MMID_BUILD_NUMBER "([^"]+)"')
    if (-not $match.Success) {
        throw "Could not read build number from $Header"
    }
    return $match.Groups[1].Value
}

function Read-PeMachine([string]$Binary) {
    $stream = [IO.File]::OpenRead($Binary)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a PE image: $Binary"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset -gt ($stream.Length - 6)) {
            throw "Invalid PE header offset: $Binary"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Missing PE signature: $Binary"
        }
        return $reader.ReadUInt16()
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Assert-ReleaseBuild([string]$BuildDirectory, [string]$Architecture) {
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "Missing CMake cache: $cachePath"
    }
    $cache = Get-Content -LiteralPath $cachePath -Raw
    if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') {
        throw "Build directory is not configured for Release: $BuildDirectory"
    }

    $binary = Join-Path $BuildDirectory 'MMIDownloader.exe'
    $coreLibrary = Join-Path $BuildDirectory 'mmid_core.lib'
    $guiObject = Join-Path $BuildDirectory 'CMakeFiles\MMIDownloader.dir\src\gui.cpp.obj'
    $versionHeader = Join-Path $BuildDirectory 'generated\version.h'
    foreach ($artifact in @($binary, $coreLibrary, $guiObject, $versionHeader)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
            throw "Missing successful-build artifact: $artifact"
        }
        if ((Get-Item -LiteralPath $artifact).Length -eq 0) {
            throw "Empty successful-build artifact: $artifact"
        }
    }

    $binaryInfo = Get-Item -LiteralPath $binary
    foreach ($inputArtifact in @($coreLibrary, $guiObject, $versionHeader)) {
        if ($binaryInfo.LastWriteTimeUtc -lt (Get-Item -LiteralPath $inputArtifact).LastWriteTimeUtc) {
            throw "Release binary is older than its build inputs: $binary"
        }
    }

    $expectedMachine = if ($Architecture -eq 'x64') { 0x8664 } else { 0x014C }
    $actualMachine = Read-PeMachine $binary
    if ($actualMachine -ne $expectedMachine) {
        throw ('Unexpected PE machine for {0}: expected 0x{1:X4}, got 0x{2:X4}' -f
            $binary, $expectedMachine, $actualMachine)
    }

    $bytes = [IO.File]::ReadAllBytes($binary)
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $unicode = [Text.Encoding]::Unicode.GetString($bytes)
    foreach ($diagnosticSwitch in @('--check-source', '--check-verify-policy')) {
        if ($ascii.Contains($diagnosticSwitch) -or $unicode.Contains($diagnosticSwitch)) {
            throw "Production binary contains diagnostic command '$diagnosticSwitch': $binary"
        }
    }

    return $binary
}

$x64 = Assert-ReleaseBuild $X64BuildDirectory 'x64'
$x86 = Assert-ReleaseBuild $X86BuildDirectory 'x86'
$x64Build = Read-BuildNumber (Join-Path $X64BuildDirectory 'generated\version.h')
$x86Build = Read-BuildNumber (Join-Path $X86BuildDirectory 'generated\version.h')
if ($x64Build -ne $x86Build) {
    throw "Architecture build numbers differ: x64=$x64Build, x86=$x86Build"
}

$archiveBase = "MMIDownloader-$version-windows-x64-x86"
$packageRootName = "MMIDownloader-$version-windows"
$staging = Join-Path $OutputDirectory ".package-staging-$PID"
$packageRoot = Join-Path $staging $packageRootName
$archive = Join-Path $OutputDirectory "$archiveBase.zip"
$temporaryArchive = Join-Path $OutputDirectory "$archiveBase.tmp-$PID.zip"
$backupArchive = Join-Path $OutputDirectory "$archiveBase.previous-$PID.zip"

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
if (Test-Path -LiteralPath $staging) {
    Remove-Item -LiteralPath $staging -Recurse -Force
}
if (Test-Path -LiteralPath $temporaryArchive) {
    Remove-Item -LiteralPath $temporaryArchive -Force
}
if (Test-Path -LiteralPath $backupArchive) {
    Remove-Item -LiteralPath $backupArchive -Force
}

try {
    New-Item -ItemType Directory -Path (Join-Path $packageRoot 'x64') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $packageRoot 'x86') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $packageRoot 'licenses') -Force | Out-Null

    Copy-Item -LiteralPath $x64 -Destination (Join-Path $packageRoot 'x64\MMIDownloader.exe')
    Copy-Item -LiteralPath $x86 -Destination (Join-Path $packageRoot 'x86\MMIDownloader.exe')
    Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $packageRoot 'README.md')
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $packageRoot 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $root 'NOTICE') -Destination (Join-Path $packageRoot 'NOTICE')
    Copy-Item -LiteralPath (Join-Path $root 'docs\third-party.md') -Destination (Join-Path $packageRoot 'THIRD-PARTY-NOTICES.md')
    Copy-Item -LiteralPath (Join-Path $root 'third_party\zlib\LICENSE') -Destination (Join-Path $packageRoot 'licenses\zlib-LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $root 'third_party\bzlib\LICENSE') -Destination (Join-Path $packageRoot 'licenses\bzip2-LICENSE.txt')

    $releaseInfo = @(
        'MMI Downloader release package'
        "Version: $version"
        "Build: $x64Build"
        'Architectures: Windows x64 and x86'
        'Signature: unsigned local build'
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $packageRoot 'RELEASE-INFO.txt'), $releaseInfo + "`r`n", [Text.UTF8Encoding]::new($false))

    $hashLines = foreach ($relative in @('x64\MMIDownloader.exe', 'x86\MMIDownloader.exe')) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $packageRoot $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $($relative.Replace('\', '/'))"
    }
    [IO.File]::WriteAllText((Join-Path $packageRoot 'SHA256SUMS.txt'), ($hashLines -join "`r`n") + "`r`n", [Text.UTF8Encoding]::new($false))

    Compress-Archive -LiteralPath $packageRoot -DestinationPath $temporaryArchive -CompressionLevel Optimal
    if (Test-Path -LiteralPath $archive) {
        Move-Item -LiteralPath $archive -Destination $backupArchive
    }
    try {
        Move-Item -LiteralPath $temporaryArchive -Destination $archive
        if (Test-Path -LiteralPath $backupArchive) {
            Remove-Item -LiteralPath $backupArchive -Force
        }
    } catch {
        if (Test-Path -LiteralPath $archive) {
            Remove-Item -LiteralPath $archive -Force
        }
        if (Test-Path -LiteralPath $backupArchive) {
            Move-Item -LiteralPath $backupArchive -Destination $archive
        }
        throw
    }
} finally {
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    if (Test-Path -LiteralPath $temporaryArchive) {
        Remove-Item -LiteralPath $temporaryArchive -Force
    }
    if (Test-Path -LiteralPath $backupArchive) {
        if (-not (Test-Path -LiteralPath $archive)) {
            Move-Item -LiteralPath $backupArchive -Destination $archive
        } else {
            Remove-Item -LiteralPath $backupArchive -Force
        }
    }
}

Get-Item -LiteralPath $archive
