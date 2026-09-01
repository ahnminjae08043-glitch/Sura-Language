param(
    [string]$Cxx = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot

function Resolve-Cxx {
    param([string]$Requested)
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Requested)) { $candidates += $Requested }
    if ($IsWindows -or $env:OS -eq "Windows_NT") { $candidates += "C:\msys64\mingw64\bin\g++.exe" }
    $candidates += @("c++", "g++", "clang++")
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    throw "C++ compiler not found. Pass -Cxx or install c++/g++/clang++."
}

$CxxPath = Resolve-Cxx $Cxx
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_embed_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $hostFile = Join-Path $temp "embed_host.cpp"
    $exeName = if ($IsWindows -or $env:OS -eq "Windows_NT") { "embed_host.exe" } else { "embed_host" }
    $exe = Join-Path $temp $exeName
    $compileLog = Join-Path $temp "compile.log"
    $runLog = Join-Path $temp "run.log"

    @'
#include "sura_ffi.hpp"
#include <cmath>
#include <cstring>
#include <iostream>

int main() {
    if (sura_abi_version() != SURA_FFI_ABI_VERSION) return 1;

    SuraHandle h = sura_new();
    if (!h) return 2;

    sura_set_number(h, "base_price", 100);
    sura_set_number(h, "demand", 0.8);
    sura_set_string(h, "item_name", "sura");
    sura_set_bool(h, "vip", 1);

    const char* src =
        "price is base_price * (1 + demand * 0.5)\n"
        "label is item_name + \":\" + to_str(price)\n"
        "allowed is vip and price > 100\n";
    int rc = sura_run(h, src);
    if (rc != SURA_OK) {
        std::cerr << sura_last_error(h) << "\n";
        return 3;
    }
    if (!sura_has(h, "price")) return 4;
    if (std::fabs(sura_get_number(h, "price") - 140.0) > 0.0001) return 5;
    if (std::strcmp(sura_get_string(h, "label"), "sura:140") != 0) return 6;
    if (!sura_get_bool(h, "allowed")) return 7;

    rc = sura_run(h, "total is price + 2\n");
    if (rc != SURA_OK) {
        std::cerr << sura_last_error(h) << "\n";
        return 8;
    }
    if (std::fabs(sura_get_number(h, "total") - 142.0) > 0.0001) return 9;

    sura_clear_global(h, "vip");
    rc = sura_run(h, "vip_seen is vip\n");
    if (rc != SURA_OK || sura_get_bool(h, "vip_seen")) return 10;

    rc = sura_run(h, "x is 1 ++ 2");
    if (rc == SURA_OK || std::strlen(sura_last_error(h)) == 0) return 11;

    sura_free(h);
    std::cout << "embed_smoke: PASS\n";
    return 0;
}
'@ | Set-Content -LiteralPath $hostFile -Encoding ASCII

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $extraArgs = @()
    if ($IsWindows -or $env:OS -eq "Windows_NT") { $extraArgs += "-lgdi32" }
    elseif ($IsLinux) { $extraArgs += "-ldl" }
    & $CxxPath -std=c++17 -O2 -I $root $hostFile (Join-Path $root "sura_ffi.cpp") `
        (Join-Path $root "gc.cpp") (Join-Path $root "platform.cpp") -o $exe @extraArgs > $compileLog 2>&1
    $compileCode = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    if ($compileCode -ne 0) {
        Get-Content -Raw -Path $compileLog | Write-Output
        throw "embedding host compile failed with exit code $compileCode"
    }

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $exe > $runLog 2>&1
    $runCode = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    if ($runCode -ne 0) {
        Get-Content -Raw -Path $runLog | Write-Output
        throw "embedding host failed with exit code $runCode"
    }
    $runText = Get-Content -Raw -Path $runLog
    if ($runText -notmatch "embed_smoke:\s+PASS") {
        Write-Output $runText
        throw "embedding host did not report PASS"
    }

    "embed_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
