@echo off
setlocal
chcp 65001 > nul

echo [SURA] SuraEngine 실행 중...
set "MSYS_BIN=C:\msys64\mingw64\bin"
if exist "%MSYS_BIN%\libsfml-window-3.dll" (
    set "PATH=%MSYS_BIN%;%PATH%"
) else if not exist "%~dp0libsfml-window-3.dll" (
    echo [경고] libsfml-window-3.dll을 찾을 수 없습니다.
    echo        MSYS 설치 경로가 다르거나 SFML DLL이 복사되지 않았습니다.
)
echo ------------------------------------------
"%~dp0SuraEngine.exe"

pause