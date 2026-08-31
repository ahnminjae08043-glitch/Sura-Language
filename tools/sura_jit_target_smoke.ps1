param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Cxx = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
if (-not $Cxx) {
    $Cxx = if ($env:OS -eq "Windows_NT") { "C:\msys64\mingw64\bin\g++.exe" } else { "c++" }
}

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
$temp = Join-Path $tempRoot ("sura_jit_target_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $suffix = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $binary = Join-Path $temp ("jit_target_info_test" + $suffix)
    & $Cxx -std=c++17 -O2 -I $root (Join-Path $root "tests/jit_target_info_test.cpp") -o $binary
    if ($LASTEXITCODE -ne 0) { throw "failed to compile JIT target contract test" }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "JIT target contract test failed" }

    $jsonText = (& $enginePath --jit-info-json 2>&1 | ForEach-Object { "$_" }) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "--jit-info-json failed: $jsonText" }
    $info = $jsonText | ConvertFrom-Json
    if ($info.schema -ne "sura.jit_target.v1" -or
        -not $info.os -or -not $info.architecture -or -not $info.abi -or
        -not $info.backend -or $info.fallback -ne "register-vm" -or -not $info.reason) {
        throw "--jit-info-json returned incomplete target metadata: $jsonText"
    }

    $architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    $isLinux = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Linux)
    $isMacOS = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::OSX)
    if ($env:OS -eq "Windows_NT" -and $architecture -eq "X64") {
        if (-not $info.native_supported -or $info.backend -ne "x64-win64") {
            throw "Windows x86-64 engine did not report its native backend"
        }
    } elseif ($isLinux -and $architecture -eq "X64") {
        if (-not $info.native_supported -or $info.backend -ne "x64-sysv-baseline") {
            throw "Linux x86-64 engine did not report its System V baseline backend"
        }
    } elseif ($architecture -eq "Arm64" -and
              ($env:OS -eq "Windows_NT" -or $isLinux -or $isMacOS)) {
        if (-not $info.native_supported -or $info.backend -ne "arm64-aapcs-baseline") {
            throw "ARM64 engine did not report its AAPCS64 baseline backend"
        }
    } elseif ($info.native_supported) {
        throw "unsupported platform unexpectedly reported native JIT support"
    }

    "sura_jit_target_smoke: PASS ($($info.os), $($info.architecture), $($info.abi), $($info.backend))"
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    $leaf = [System.IO.Path]::GetFileName($resolvedTemp)
    $parent = [System.IO.Path]::GetDirectoryName($resolvedTemp).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    if ($parent -eq $tempRoot -and $leaf -match '^sura_jit_target_[0-9a-f]{32}$' -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
