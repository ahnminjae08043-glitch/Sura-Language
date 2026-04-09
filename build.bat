@echo off
C:\msys64\mingw64\bin\g++.exe -std=c++17 -Wall main.cpp -o SuraFinal.exe > build_output.txt 2>&1
echo Exit Code: %errorlevel%
type build_output.txt
