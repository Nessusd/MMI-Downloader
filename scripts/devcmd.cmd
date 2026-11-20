@echo off
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if /I not "%ARCH%"=="x64" if /I not "%ARCH%"=="x86" (
    echo Usage: scripts\devcmd.cmd [x64^|x86]
    exit /b 2
)

call "%~dp0find-vs.cmd"
if errorlevel 1 exit /b %errorlevel%

call "%VSDEVCMD%" -arch=%ARCH% -host_arch=x64
