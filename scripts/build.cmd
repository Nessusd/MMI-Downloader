@echo off
setlocal

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
set "BUILD_DIR=%~2"
if not defined MMID_CMAKE_GENERATOR set "MMID_CMAKE_GENERATOR=Ninja"

if /I "%ARCH%"=="x64" (
    set "ARCH=x64"
    if "%BUILD_DIR%"=="" set "BUILD_DIR=build"
) else if /I "%ARCH%"=="x86" (
    set "ARCH=x86"
    if "%BUILD_DIR%"=="" set "BUILD_DIR=build-x86"
) else (
    echo Usage: scripts\build.cmd [x64^|x86] [build-directory]
    exit /b 2
)

call "%~dp0find-vs.cmd"
if errorlevel 1 exit /b %errorlevel%

call "%VSDEVCMD%" -arch=%ARCH% -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

cd /d "%~dp0\.."

for %%I in ("%BUILD_DIR%") do set "BUILD_DIR=%%~fI"
set "BUILD_TEMP=%BUILD_DIR%\tmp"
if not exist "%BUILD_TEMP%" mkdir "%BUILD_TEMP%"
if errorlevel 1 (
    echo Could not create the build temp directory: "%BUILD_TEMP%" 1>&2
    exit /b 1
)
set "TMP=%BUILD_TEMP%"
set "TEMP=%BUILD_TEMP%"

where cl >nul 2>nul
if errorlevel 1 (
    echo MSVC C/C++ compiler was not found after Visual Studio environment setup. 1>&2
    exit /b 1
)
where cmake >nul 2>nul
if errorlevel 1 (
    echo cmake was not found after Visual Studio environment setup. 1>&2
    exit /b 1
)
if /I "%MMID_CMAKE_GENERATOR%"=="Ninja" (
    where ninja >nul 2>nul
    if errorlevel 1 (
        echo ninja was not found after Visual Studio environment setup. 1>&2
        exit /b 1
    )
)

cmake -S . -B "%BUILD_DIR%" -G "%MMID_CMAKE_GENERATOR%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%

rem The two first-party translation units are intentionally compiled
rem sequentially to keep peak memory usage predictable on service laptops.
if /I "%MMID_CMAKE_GENERATOR%"=="NMake Makefiles" (
    cmake --build "%BUILD_DIR%" --config Release
) else (
    cmake --build "%BUILD_DIR%" --config Release --parallel 1
)
if errorlevel 1 exit /b %errorlevel%

dir /-c "%BUILD_DIR%\MMIDownloader.exe"
