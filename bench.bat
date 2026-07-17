@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\sura_bench_now.ps1" -OpenDashboard %*
set "BENCH_EXIT=%ERRORLEVEL%"
echo.
if "%BENCH_EXIT%"=="0" (
  echo Benchmark complete.
  echo Dashboard: %~dp0artifacts\bench_dashboard.html
) else (
  echo Benchmark failed with exit code %BENCH_EXIT%.
)
if not "%SURA_BENCH_NO_PAUSE%"=="1" pause
exit /b %BENCH_EXIT%
