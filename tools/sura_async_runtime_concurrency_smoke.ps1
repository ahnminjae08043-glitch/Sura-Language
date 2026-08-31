param(
    [string]$Cxx = "",
    [int]$Repeat = 1,
    [switch]$ThreadSanitize
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$root = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($Cxx)) {
    $candidates = @()
    if ($env:OS -eq "Windows_NT") {
        $candidates += "C:\msys64\mingw64\bin\g++.exe"
    }
    $candidates += @("c++", "g++", "clang++")
    foreach ($candidate in $candidates) {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) {
            $Cxx = $candidate
            break
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Cxx)) {
    throw "No C++17 compiler found; pass -Cxx <path>"
}
if ($Repeat -lt 1 -or $Repeat -gt 1000) {
    throw "Repeat must be in 1..1000"
}

$binaryBase = if ($ThreadSanitize) {
    "async_runtime_concurrency_test_tsan"
} else {
    "async_runtime_concurrency_test"
}
$exeName = if ($env:OS -eq "Windows_NT") { "$binaryBase.exe" } else { $binaryBase }
$output = Join-Path $root "build\$exeName"
New-Item -ItemType Directory -Force (Split-Path -Parent $output) | Out-Null

$arguments = @(
    "-std=c++17",
    "-pthread",
    "-I.",
    $(if ($ThreadSanitize) { "-O1" } else { "-O2" })
)
if ($ThreadSanitize) {
    $arguments += @(
        "-g",
        "-fno-omit-frame-pointer",
        "-fsanitize=thread"
    )
}
# Forward slashes: g++ accepts them on Windows, and backslash relative paths
# are not path separators for Unix compilers, so this is the portable spelling.
$arguments += @(
    "tests/async_runtime_concurrency_test.cpp",
    "gc.cpp",
    "-o",
    "build/$exeName"
)
Push-Location $root
try {
    & $Cxx @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Async runtime concurrency harness compilation failed with exit code $LASTEXITCODE"
    }

    for ($round = 1; $round -le $Repeat; $round++) {
        Write-Host "async concurrency round $round/$Repeat"
        & $output
        if ($LASTEXITCODE -ne 0) {
            throw "Async runtime concurrency harness failed in round $round with exit code $LASTEXITCODE"
        }
    }
} finally {
    Pop-Location
}

Write-Host "async runtime concurrency smoke: PASS ($Repeat round(s))"
