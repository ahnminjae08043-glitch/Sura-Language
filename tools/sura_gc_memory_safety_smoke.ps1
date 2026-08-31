param(
    [string]$Cxx = "",
    [switch]$Sanitize
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot
$candidates = @()
if (-not [string]::IsNullOrWhiteSpace($Cxx)) { $candidates += $Cxx }
if (-not [string]::IsNullOrWhiteSpace($env:SURA_CXX)) { $candidates += $env:SURA_CXX }
$candidates += @("c++", "g++", "clang++", "C:\msys64\mingw64\bin\g++.exe")

$compiler = $null
foreach ($candidate in $candidates) {
    try {
        & $candidate --version *> $null
        if ($LASTEXITCODE -eq 0) { $compiler = $candidate; break }
    } catch {
    }
}
if (-not $compiler) { throw "C++ compiler not found for GC memory-safety smoke" }

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_gc_safety_" + [System.Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $suffix = if ($IsWindows -or $env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $output = Join-Path $temp ("gc_memory_safety_test" + $suffix)
    $source = Join-Path $root "tests\gc_memory_safety_test.cpp"
    $gcSource = Join-Path $root "gc.cpp"
    $compileArgs = @(
        "-std=c++17",
        "-DNDEBUG",
        "-DSURA_GC_TEST_HOOKS"
    )
    if ($Sanitize) {
        $compileArgs += @(
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
            "-fsanitize=address,undefined"
        )
    } else {
        $compileArgs += "-O2"
    }
    $compileArgs += @($source, $gcSource, "-o", $output)

    & $compiler @compileArgs
    if ($LASTEXITCODE -ne 0) { throw "GC memory-safety harness compilation failed" }

    $result = & $output 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $text = $result -join "`n"
    if ($code -ne 0 -or $text -notmatch "gc memory safety: PASS") {
        Write-Output $text
        throw "GC memory-safety harness failed (exit=$code)"
    }
    "sura_gc_memory_safety_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
