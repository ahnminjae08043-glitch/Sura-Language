param(
    [string]$Cxx = "",
    [switch]$Sanitize
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot

function Resolve-Cxx([string]$Requested) {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Requested)) { $candidates += $Requested }
    if ($IsWindows -or $env:OS -eq "Windows_NT") { $candidates += "C:\msys64\mingw64\bin\g++.exe" }
    $candidates += @("c++", "g++", "clang++")
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    throw "C++ compiler not found. Pass -Cxx or install c++/g++/clang++."
}

$compiler = Resolve-Cxx $Cxx
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_ffi_safety_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $common = @("-std=c++17", "-O1", "-g", "-Wall", "-DSURA_FFI_TESTING", "-I", $root)
    $links = @()
    if ($IsWindows -or $env:OS -eq "Windows_NT") { $links += "-lgdi32" }
    elseif ($IsLinux) { $links += @("-ldl", "-pthread") }
    else { $links += "-pthread" }
    if ($Sanitize) {
        $common += @("-fno-omit-frame-pointer", "-fsanitize=address,undefined")
        $links += "-fsanitize=address,undefined"
    }

    # The public ABI header must be consumable by a C compiler, not only by C++.
    $headerSmoke = Join-Path $temp "sura_ffi_header_smoke.c"
    [IO.File]::WriteAllText($headerSmoke, @'
#include "sura_ffi.hpp"
int main(void) {
    SuraHandle handle = 0;
    return handle == 0 && SURA_ERR_BUSY == -5 ? 0 : 1;
}
'@, (New-Object Text.UTF8Encoding($false)))
    & $compiler -x c -fsyntax-only -I $root $headerSmoke
    if ($LASTEXITCODE -ne 0) {
        throw "sura_ffi.hpp C compatibility check failed with exit code $LASTEXITCODE"
    }

    # Compile the very large FFI/runtime translation unit once. Compiling it
    # again for every harness causes avoidable peak memory and has made the
    # Windows GCC driver terminate without diagnostics on constrained hosts.
    $runtimeObjects = @()
    foreach ($runtimeSource in @("sura_ffi.cpp", "gc.cpp", "platform.cpp")) {
        $source = Join-Path $root $runtimeSource
        $object = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($source) + ".o")
        $compileLog = Join-Path $temp ([IO.Path]::GetFileNameWithoutExtension($source) + ".compile.log")
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $compiler @common -c $source -o $object > $compileLog 2>&1
        $compileCode = $LASTEXITCODE
        $ErrorActionPreference = $oldPreference
        if ($compileCode -ne 0) {
            Get-Content -Raw -Path $compileLog | Write-Output
            throw "$runtimeSource compilation failed with exit code $compileCode"
        }
        $runtimeObjects += $object
    }

    $tests = @(
        "tests/sura_ffi_gc_isolation.cpp",
        "tests/sura_ffi_safety_test.cpp",
        "tests/ffi_uncaught_upvalue_test.cpp"
    )
    foreach ($relative in $tests) {
        $source = Join-Path $root $relative
        $name = [IO.Path]::GetFileNameWithoutExtension($source)
        $suffix = if ($IsWindows -or $env:OS -eq "Windows_NT") { ".exe" } else { "" }
        $binary = Join-Path $temp ($name + $suffix)
        $testObject = Join-Path $temp ($name + ".o")
        $compileLog = Join-Path $temp ($name + ".compile.log")

        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $compiler @common -c $source -o $testObject > $compileLog 2>&1
        $compileCode = $LASTEXITCODE
        if ($compileCode -eq 0) {
            & $compiler $testObject @runtimeObjects -o $binary @links >> $compileLog 2>&1
            $compileCode = $LASTEXITCODE
        }
        $ErrorActionPreference = $oldPreference
        if ($compileCode -ne 0) {
            Get-Content -Raw -Path $compileLog | Write-Output
            throw "$name compilation failed with exit code $compileCode"
        }

        $oldAsan = $env:ASAN_OPTIONS
        $oldUbsan = $env:UBSAN_OPTIONS
        if ($Sanitize) {
            $env:ASAN_OPTIONS = "detect_leaks=1:halt_on_error=1:strict_string_checks=1"
            $env:UBSAN_OPTIONS = "halt_on_error=1:print_stacktrace=1"
        }
        try {
            $startInfo = New-Object Diagnostics.ProcessStartInfo
            $startInfo.FileName = $binary
            $startInfo.UseShellExecute = $false
            $startInfo.CreateNoWindow = $true
            $startInfo.RedirectStandardOutput = $true
            $startInfo.RedirectStandardError = $true
            $process = New-Object Diagnostics.Process
            $process.StartInfo = $startInfo
            if (-not $process.Start()) { throw "failed to start $name" }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(60000)) {
                try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
                throw "$name timed out after 60 seconds (possible FFI deadlock)"
            }
            $runCode = $process.ExitCode
            $output = @($stdout.Result, $stderr.Result) | Where-Object { -not [string]::IsNullOrEmpty($_) }
            $process.Dispose()
        } finally {
            $env:ASAN_OPTIONS = $oldAsan
            $env:UBSAN_OPTIONS = $oldUbsan
        }
        $passMarker = if ($name -eq "sura_ffi_safety_test") { "sura_ffi_safety" } else { $name }
        if ($runCode -ne 0 -or ($output -join "`n") -notmatch ([regex]::Escape($passMarker) + ": PASS")) {
            $output | Write-Output
            throw "$name failed with exit code $runCode"
        }
        $output | Write-Output
    }

    "sura_ffi_safety_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
