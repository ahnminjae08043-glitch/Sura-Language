param(
    [string]$RepoRoot = ".",
    [string]$Engine = "",
    [string]$Cxx = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
if ([string]::IsNullOrWhiteSpace($Engine)) {
    $Engine = Join-Path $root $(if ($env:OS -eq "Windows_NT") { "SuraLanguage.exe" } else { "SuraLanguage" })
}
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
if ([string]::IsNullOrWhiteSpace($Cxx)) {
    $Cxx = if ($env:OS -eq "Windows_NT") { "C:\msys64\mingw64\bin\g++.exe" } else { "c++" }
}

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
$temp = Join-Path $tempRoot ("sura_jit_arm64_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $suffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $binary = Join-Path $temp ("jit_arm64_baseline_test" + $suffix)
    $compileArgs = @(
        "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-pedantic",
        "-I.", "tests/jit_arm64_baseline_test.cpp", "platform.cpp", "gc.cpp",
        "-o", $binary
    )
    $isLinux = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Linux)
    if ($isLinux) { $compileArgs += "-ldl" }

    Push-Location $root
    try {
        & $Cxx @compileArgs
        if ($LASTEXITCODE -ne 0) { throw "ARM64 baseline C++ test compilation failed" }
    } finally {
        Pop-Location
    }

    $testOutput = & $binary 2>&1 | ForEach-Object { "$_" }
    $testCode = $LASTEXITCODE
    $testText = $testOutput -join "`n"
    $testOutput | Write-Host
    if ($testCode -ne 0 -or $testText -notmatch "jit arm64 baseline: PASS") {
        throw "ARM64 baseline C++ test failed (exit=$testCode)"
    }

    $infoText = (& $enginePath --jit-info-json 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "--jit-info-json failed: $infoText" }
    $info = $infoText | ConvertFrom-Json
    $architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    if ($architecture -eq "Arm64") {
        if (-not $info.native_supported -or $info.backend -ne "arm64-aapcs-baseline") {
            throw "ARM64 runtime did not enable its AAPCS64 baseline: $infoText"
        }
        if ($testText -notmatch "native execution") {
            throw "ARM64 test did not execute generated machine code"
        }
    } elseif ($testText -notmatch "encoder") {
        throw "non-ARM64 test did not stay in encoder-only mode"
    }

    Write-Host "sura_jit_arm64_baseline_smoke: PASS ($($info.os), $($info.architecture), $($info.backend))"
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    $leaf = [System.IO.Path]::GetFileName($resolvedTemp)
    $parent = [System.IO.Path]::GetDirectoryName($resolvedTemp).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    if ($parent -eq $tempRoot -and $leaf -match '^sura_jit_arm64_[0-9a-f]{32}$' -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
