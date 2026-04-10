@echo off
chcp 65001 > nul
cd /d %~dp0
echo [SURA] Building SuraEngine...
C:\msys64\mingw64\bin\g++.exe -std=c++17 -O2 -Wall main.cpp gc.cpp -o SuraEngine.exe 2>build_errors.txt
if %ERRORLEVEL% EQU 0 (
    echo [OK] Build successful! SuraEngine.exe
    if not "%1"=="" (
        SuraEngine.exe %*
    )
) else (
    echo [FAIL] Build failed:
    type build_errors.txt
    pause
)