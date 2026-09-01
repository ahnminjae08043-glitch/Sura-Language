@echo off
setlocal EnableExtensions
chcp 65001 > nul

if not exist "%~dp0SuraLanguage.exe" (
    echo [SURA] SuraLanguage.exe is missing. Run build.bat first.
    exit /b 2
)

"%~dp0SuraLanguage.exe" %*
set "RUN_RC=%errorlevel%"
pause
exit /b %RUN_RC%
