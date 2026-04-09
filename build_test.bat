@echo off
chcp 65001 > nul
cd /d %~dp0
echo [SURA] SuraEngine2 빌드 중...
C:\msys64\mingw64\bin\g++.exe -std=c++17 -O2 -IC:\msys64\mingw64\include main2.cpp -o SuraEngine2.exe -LC:\msys64\mingw64\lib -lsfml-graphics -lsfml-window -lsfml-system 2>build_errors.txt
if %ERRORLEVEL% EQU 0 (
    echo [OK] 빌드 성공! SuraEngine2.exe
    if not "%1"=="" (
        SuraEngine2.exe %1
    ) else (
        SuraEngine2.exe
    )
) else (
    echo [FAIL] 빌드 실패 - 오류 내용:
    type build_errors.txt
    pause
)
