param(
    [string]$RepoRoot = ".",
    [string]$Cxx = ""
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$testSource = Join-Path $root "tests/typechecker_hardening_test.cpp"
if (-not (Test-Path -LiteralPath $testSource -PathType Leaf)) {
    throw "typechecker hardening test source not found: $testSource"
}

function Resolve-Cxx {
    param([string]$Requested)

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Requested)) { $candidates += $Requested }
    if (-not [string]::IsNullOrWhiteSpace($env:SURA_CXX)) { $candidates += $env:SURA_CXX }
    if ($env:OS -eq "Windows_NT") { $candidates += "C:\msys64\mingw64\bin\g++.exe" }
    $candidates += @("c++", "g++")

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        $command = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return $command.Source }
    }
    throw "no C++ compiler found; pass -Cxx or set SURA_CXX"
}

$compiler = Resolve-Cxx $Cxx
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_typechecker_test_" + [System.Guid]::NewGuid().ToString("N"))
$onWindows = $env:OS -eq "Windows_NT"
$binaryName = if ($onWindows) { "typechecker_hardening_test.exe" } else { "typechecker_hardening_test" }
$binary = Join-Path $temp $binaryName

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $compileArgs = @(
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-pedantic",
        "-I.",
        "tests/typechecker_hardening_test.cpp",
        "platform.cpp",
        "gc.cpp",
        "-o",
        $binary
    )
    if (-not $onWindows -and [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
            [System.Runtime.InteropServices.OSPlatform]::Linux)) {
        $compileArgs += "-ldl"
    }

    Push-Location $root
    try {
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $compileOutput = (& $compiler @compileArgs 2>&1) | ForEach-Object { "$_" }
        $compileCode = $LASTEXITCODE
        $ErrorActionPreference = $oldPreference
    }
    finally {
        Pop-Location
    }
    if ($compileCode -ne 0) {
        $compileOutput | Write-Host
        throw "typechecker hardening test compilation failed with exit code $compileCode"
    }

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $testOutput = (& $binary 2>&1) | ForEach-Object { "$_" }
    $testCode = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    $testOutput | Write-Host
    if ($testCode -ne 0 -or ($testOutput -join "`n") -notmatch '\[PASS\] typechecker hardening tests') {
        throw "typechecker hardening test failed with exit code $testCode"
    }
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

Write-Host "sura_typechecker_hardening_smoke: PASS"
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
