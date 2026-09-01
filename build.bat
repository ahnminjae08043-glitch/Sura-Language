@echo off
setlocal EnableExtensions

set "BUILD_MODE=%~1"
if not defined BUILD_MODE set "BUILD_MODE=portable"

rem Zero the PE header timestamp. Without this, two builds of identical source
rem differ in exactly the 4 timestamp bytes, so the SHA-256 that release
rem provenance records cannot be reproduced by anyone rebuilding the tree.
set "REPRO_FLAGS=-Wl,--no-insert-timestamp"

set "CPU_FLAGS="
set "OPT_FLAGS=-O3"
set "LINK_FLAGS=%REPRO_FLAGS%"
set "SKIP_PKG="
if /I "%BUILD_MODE%"=="portable" (
    set "LINK_FLAGS=-static %REPRO_FLAGS%"
    goto build_mode_ready
)
if /I "%BUILD_MODE%"=="native" (
    set "CPU_FLAGS=-march=native"
    goto build_mode_ready
)
rem Iteration build. Measured on this tree with g++ 15.2.0: main.cpp alone is
rem 89s at -O3 and 53s at -O1, and the engine is one dominant translation unit,
rem so the optimizer is most of what a correctness-only rebuild pays for.
rem Skipping the package manager saves a further 35s.
rem
rem -O0 is deliberately not used even though it compiles fastest (40s): this
rem toolchain then fails to link, with undefined __emutls_t references for the
rem thread_local statics inside SuraStd's inline accessors. -O1 is the fastest
rem level that actually produces a binary here.
rem
rem The remaining floor is the ~16k-line stdlib.hpp itself, which no flag
rem avoids - splitting it is the only way past roughly 50s.
if /I "%BUILD_MODE%"=="dev" (
    set "OPT_FLAGS=-O1"
    set "SKIP_PKG=1"
    goto build_mode_ready
)

echo Usage: build.bat [portable^|native^|dev]
echo   portable  Build a redistributable binary without host-specific CPU instructions. ^(default^)
echo   native    Optimize for this machine with -march=native. Do not redistribute this build.
echo   dev       Lightly optimized engine only, for fast iteration. Roughly 55s versus 145s.
echo             Do not use for benchmarking, release, or any timing measurement.
exit /b 2

:build_mode_ready
if defined SURA_CXX (
    set "CXX=%SURA_CXX%"
) else (
    set "CXX=C:\msys64\mingw64\bin\g++.exe"
)

echo Sura build mode: %BUILD_MODE%
echo C++ compiler: %CXX%

call "%CXX%" -std=c++17 %OPT_FLAGS% -DNDEBUG %CPU_FLAGS% -Wall main.cpp gc.cpp platform.cpp -o SuraLanguage.exe %LINK_FLAGS% -lgdi32 > build_output.txt 2>&1
set ENGINE_RC=%errorlevel%
type build_output.txt
if "%ENGINE_RC%"=="0" (
    copy /Y SuraLanguage.exe SuraEngine.exe > nul
)

if defined SKIP_PKG (
    echo Skipping surapkg in dev mode.
    set PKG_RC=0
    goto build_done
)

call "%CXX%" -std=c++17 -O2 -Wall surapkg.cpp -o surapkg.exe %LINK_FLAGS% > surapkg_build_output.txt 2>&1
set PKG_RC=%errorlevel%
type surapkg_build_output.txt

:build_done

if not "%ENGINE_RC%"=="0" exit /b %ENGINE_RC%
if not "%PKG_RC%"=="0" exit /b %PKG_RC%
exit /b 0
