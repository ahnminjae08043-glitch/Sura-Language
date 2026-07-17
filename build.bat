@echo off
setlocal EnableExtensions

set "BUILD_MODE=%~1"
if not defined BUILD_MODE set "BUILD_MODE=portable"

set "CPU_FLAGS="
set "LINK_FLAGS="
if /I "%BUILD_MODE%"=="portable" (
    set "LINK_FLAGS=-static"
    goto build_mode_ready
)
if /I "%BUILD_MODE%"=="native" (
    set "CPU_FLAGS=-march=native"
    goto build_mode_ready
)

echo Usage: build.bat [portable^|native]
echo   portable  Build a redistributable binary without host-specific CPU instructions. ^(default^)
echo   native    Optimize for this machine with -march=native. Do not redistribute this build.
exit /b 2

:build_mode_ready
if defined SURA_CXX (
    set "CXX=%SURA_CXX%"
) else (
    set "CXX=C:\msys64\mingw64\bin\g++.exe"
)

echo Sura build mode: %BUILD_MODE%
echo C++ compiler: %CXX%

call "%CXX%" -std=c++17 -O3 -DNDEBUG %CPU_FLAGS% -Wall main.cpp gc.cpp platform.cpp -o SuraLanguage.exe %LINK_FLAGS% -lgdi32 > build_output.txt 2>&1
set ENGINE_RC=%errorlevel%
type build_output.txt
if "%ENGINE_RC%"=="0" (
    copy /Y SuraLanguage.exe SuraEngine.exe > nul
)

call "%CXX%" -std=c++17 -O2 -Wall surapkg.cpp -o surapkg.exe %LINK_FLAGS% > surapkg_build_output.txt 2>&1
set PKG_RC=%errorlevel%
type surapkg_build_output.txt

if not "%ENGINE_RC%"=="0" exit /b %ENGINE_RC%
if not "%PKG_RC%"=="0" exit /b %PKG_RC%
exit /b 0
