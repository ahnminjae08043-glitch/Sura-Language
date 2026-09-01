@echo off
setlocal EnableExtensions
chcp 65001 > nul
cd /d "%~dp0"

echo [SURA] Building the canonical portable runtime...
call "%~dp0build.bat" portable
set "BUILD_RC=%errorlevel%"
if not "%BUILD_RC%"=="0" (
    echo [FAIL] Build failed with exit code %BUILD_RC%.
    exit /b %BUILD_RC%
)

echo [OK] Built SuraLanguage.exe
"%~dp0SuraLanguage.exe" %*
exit /b %errorlevel%
