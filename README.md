# MMIDownloader

MMIDownloader is a native Windows GUI for finding an MMI software package by
part number, downloading and verifying its files, and either preparing a
removable drive or extracting the package to a folder.

The application does not include a catalog or access credentials. Configure a
local or network catalog through **File > Settings** before searching.

## Catalog sources

The GUI supports:

- a local catalog directory;
- HTTP or HTTPS;
- WebDAV;
- anonymous FTP.

Credentials are accepted only for HTTPS sources. The saved password is
protected with Windows DPAPI for the current Windows user. Runtime settings are
stored in `config.ini` next to the executable and are intentionally excluded
from Git.

## Building

Requirements:

- Windows;
- Visual Studio 2022 or Build Tools with the MSVC x86/x64 toolchain;
- CMake 3.24 or newer;
- Ninja on `PATH` (the default generator).

Build either architecture from PowerShell or Command Prompt:

```powershell
.\scripts\build.cmd x64
.\scripts\build.cmd x86
```

The default output directories are `build` and `build-x86`. Both are ignored
by Git.

## Creating a release archive

Use one build number for both architectures, build them, and run the packaging
script:

```powershell
$env:MMID_BUILD_NUMBER = Get-Date -Format 'yyyyMMddHHmm'
.\scripts\build.cmd x64
.\scripts\build.cmd x86
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

The package is written to `dist/`. It contains the x64 and x86 executables,
SHA-256 checksums, the project license, and the third-party notices. `dist/` is
excluded from the source repository; publish the ZIP as a release asset.

## Dependencies and license

MMIDownloader is licensed under the Apache License 2.0. It statically links
vendored zlib and bzip2 sources, which remain under their respective licenses.
See [NOTICE](NOTICE) and [third-party notices](docs/third-party.md).
