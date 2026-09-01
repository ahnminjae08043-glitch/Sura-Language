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
    # SURA_CXX wins: CI installs MSYS2 under the runner temp directory.
    $Cxx = if ($env:SURA_CXX) { $env:SURA_CXX } elseif ($env:OS -eq "Windows_NT") { if (Test-Path -LiteralPath "C:\msys64\mingw64\bin\g++.exe") { "C:\msys64\mingw64\bin\g++.exe" } else { "g++" } } else { "c++" }
}

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
$temp = Join-Path $tempRoot ("sura_jit_portable_" + [Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $suffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $binary = Join-Path $temp ("jit_sysv_baseline_test" + $suffix)
    $compileArgs = @(
        "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-pedantic",
        "-I.", "tests/jit_sysv_baseline_test.cpp", "platform.cpp", "gc.cpp",
        "-o", $binary
    )
    $onLinux = [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::Linux)
    if ($onLinux) { $compileArgs += "-ldl" }

    Push-Location $root
    try {
        & $Cxx @compileArgs
        if ($LASTEXITCODE -ne 0) { throw "System V baseline C++ test compilation failed" }
    } finally {
        Pop-Location
    }

    $nativeOutput = & $binary 2>&1 | ForEach-Object { "$_" }
    $nativeCode = $LASTEXITCODE
    $nativeText = $nativeOutput -join "`n"
    $nativeOutput | Write-Host
    if ($nativeCode -ne 0 -or $nativeText -notmatch "jit sysv baseline: PASS") {
        throw "System V baseline C++ test failed (exit=$nativeCode)"
    }

    $infoText = (& $enginePath --jit-info-json 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "--jit-info-json failed: $infoText" }
    $info = $infoText | ConvertFrom-Json

    $source = Join-Path $root "tests/jit_portable_baseline.sura"
    $sourceOutput = & $enginePath --jit $source 2>&1 | ForEach-Object { "$_" }
    $sourceCode = $LASTEXITCODE
    $sourceText = $sourceOutput -join "`n"
    $sourceOutput | Write-Host
    if ($sourceCode -ne 0 -or $sourceText -notmatch "jit portable baseline source: PASS") {
        throw "portable baseline source test failed (exit=$sourceCode)"
    }
    if ($info.native_supported -and $sourceText -notmatch "1 function\(s\).+compiled") {
        throw "native target did not compile constant_math"
    }
    if (-not $info.native_supported -and $sourceText -notmatch "0 function\(s\).+compiled") {
        throw "unsupported target did not use register-VM fallback"
    }

    Write-Host "sura_jit_portable_baseline_smoke: PASS ($($info.os), $($info.architecture), $($info.backend))"
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    $leaf = [System.IO.Path]::GetFileName($resolvedTemp)
    $parent = [System.IO.Path]::GetDirectoryName($resolvedTemp).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    if ($parent -eq $tempRoot -and $leaf -match '^sura_jit_portable_[0-9a-f]{32}$' -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
