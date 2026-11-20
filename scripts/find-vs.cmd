@echo off

rem Return VSDEVCMD to the caller. Do not use setlocal here.
if defined VSDEVCMD if exist "%VSDEVCMD%" exit /b 0
set "VSDEVCMD="
set "VSINSTALL="
set "VSWHERE="

if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
) else if exist "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" (
    set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
)

if defined VSWHERE (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
)

if defined VSINSTALL if exist "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" (
    set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
)

rem Environment variables cover custom and older Visual Studio installations.
if not defined VSDEVCMD if defined VS170COMNTOOLS if exist "%VS170COMNTOOLS%VsDevCmd.bat" set "VSDEVCMD=%VS170COMNTOOLS%VsDevCmd.bat"
if not defined VSDEVCMD if defined VS160COMNTOOLS if exist "%VS160COMNTOOLS%VsDevCmd.bat" set "VSDEVCMD=%VS160COMNTOOLS%VsDevCmd.bat"
if not defined VSDEVCMD if defined VS150COMNTOOLS if exist "%VS150COMNTOOLS%VsDevCmd.bat" set "VSDEVCMD=%VS150COMNTOOLS%VsDevCmd.bat"

rem Fallbacks are intentionally limited to known Visual Studio layouts and
rem ordered newest-first across all editions.
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VSDEVCMD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat"
    if not defined VSDEVCMD if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat"
)
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VSDEVCMD if exist "%ProgramFiles%\Microsoft Visual Studio\2019\%%E\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2019\%%E\Common7\Tools\VsDevCmd.bat"
    if not defined VSDEVCMD if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%E\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%E\Common7\Tools\VsDevCmd.bat"
)
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VSDEVCMD if exist "%ProgramFiles%\Microsoft Visual Studio\2017\%%E\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2017\%%E\Common7\Tools\VsDevCmd.bat"
    if not defined VSDEVCMD if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\%%E\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\%%E\Common7\Tools\VsDevCmd.bat"
)

if not defined VSDEVCMD (
    echo Visual Studio with the C++ x86/x64 tools was not found. 1>&2
    echo Install Visual Studio Build Tools and the Desktop development with C++ workload. 1>&2
    exit /b 1
)

exit /b 0
