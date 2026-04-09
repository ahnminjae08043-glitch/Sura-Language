Set-Location "C:\Users\user\Documents\Sura"
$output = & "C:\msys64\mingw64\bin\g++.exe" -std=c++17 -Wall main2.cpp -o SuraEngine2.exe -lsfml-graphics -lsfml-window -lsfml-system 2>&1
$output | Out-File -FilePath "build_errors.txt" -Encoding UTF8
$exitCode = $LASTEXITCODE
if ($exitCode -eq 0) {
    Write-Host "[OK] 빌드 성공!"
} else {
    Write-Host "[FAIL] 빌드 실패 (Exit: $exitCode)"
    Get-Content "build_errors.txt"
}
