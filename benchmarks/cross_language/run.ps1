param(
    [string]$Engine = "",
    [string]$Cxx = "",
    [int]$Repeat = 1,
    [switch]$SkipBuild
)

# Runs the same eight workloads in every language that is installed on this
# machine and prints one table. A language that is not installed is skipped
# rather than failing, so the script is useful with any subset.
#
# Every implementation prints a checksum built from the results it computed.
# The checksums must agree across languages; if they do not, the programs are
# not doing the same work and the timings are not comparable. Two harmless
# differences are expected and documented in README.md.

$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here "..\..")).Path
Set-Location $here

if ([string]::IsNullOrWhiteSpace($Engine)) {
    $candidate = Join-Path $root $(if ($env:OS -eq "Windows_NT") { "SuraLanguage.exe" } else { "SuraLanguage" })
    if (Test-Path -LiteralPath $candidate) { $Engine = (Resolve-Path -LiteralPath $candidate).Path }
}
if ([string]::IsNullOrWhiteSpace($Cxx)) {
    if ($env:SURA_CXX) { $Cxx = $env:SURA_CXX }
    elseif ($env:OS -eq "Windows_NT" -and (Test-Path "C:\msys64\mingw64\bin\g++.exe")) { $Cxx = "C:\msys64\mingw64\bin\g++.exe" }
    else { $Cxx = "c++" }
}

function Have([string]$name) {
    return $null -ne (Get-Command $name -ErrorAction SilentlyContinue)
}

$exe = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }

if (-not $SkipBuild) {
    Write-Host "building..."
    if (Have $Cxx) { & $Cxx -std=c++17 -O2 bench.cpp -o ("bench_cpp" + $exe) 2>&1 | Out-Null }
    if (Have "rustc") {
        & rustc -O -o ("bench_rs" + $exe) bench.rs 2>&1 | Out-Null
        # On Windows the default MSVC toolchain can fail to link when a GNU
        # "link" from MSYS2 shadows MSVC's link.exe. Fall back to the GNU
        # toolchain when it is installed.
        if (-not (Test-Path ("bench_rs" + $exe)) -and (Have "rustup")) {
            & rustup run stable-x86_64-pc-windows-gnu rustc -O -o ("bench_rs" + $exe) bench.rs 2>&1 | Out-Null
        }
    }
    if (Have "go") { & go build -o ("bench_go" + $exe) bench.go 2>&1 | Out-Null }
    if (Have "javac") { & javac Bench.java 2>&1 | Out-Null }
    if (Have "dotnet") { & dotnet build csharp -c Release --nologo -v quiet 2>&1 | Out-Null }
}

$runs = @()
if (Test-Path ("bench_cpp" + $exe)) { $runs += @{ n = "C++ -O2";      c = (Join-Path $here ("bench_cpp" + $exe)); a = @() } }
if (Test-Path ("bench_rs" + $exe))  { $runs += @{ n = "Rust -O";      c = (Join-Path $here ("bench_rs" + $exe));  a = @() } }
if (Test-Path ("bench_go" + $exe))  { $runs += @{ n = "Go";           c = (Join-Path $here ("bench_go" + $exe));  a = @() } }
$cs = Join-Path $here "csharp\bin\Release\net10.0\csbench$exe"
if (Test-Path $cs)                  { $runs += @{ n = "C# .NET";      c = $cs;      a = @() } }
if (Test-Path "Bench.class")        { $runs += @{ n = "Java";         c = "java";   a = @("Bench") } }
if (Have "node")                    { $runs += @{ n = "Node";         c = "node";   a = @("bench.js") } }
if ($Engine -and (Test-Path -LiteralPath $Engine)) {
    $runs += @{ n = "Sura JIT"; c = $Engine; a = @("--jit", "bench.sura") }
    $runs += @{ n = "Sura VM";  c = $Engine; a = @("bench.sura") }
}
if (Have "python")                  { $runs += @{ n = "Python";       c = "python"; a = @("bench.py") } }

if ($runs.Count -eq 0) { throw "no runnable language found" }

$benches = @("fib","numeric","array","string","dict","sort","object","matmul")
$all = [ordered]@{}
foreach ($r in $runs) {
    $best = @{}
    for ($pass = 0; $pass -lt [Math]::Max(1, $Repeat); $pass++) {
        $out = ""
        try { $out = (& $r.c @($r.a) 2>&1 | Out-String) } catch { $out = "" }
        foreach ($line in ($out -split "`r?`n")) {
            if ($line -match '^([a-z]+)=([0-9.eE+-]+)\s*$') {
                $k = $matches[1]; $v = [double]$matches[2]
                if (-not $best.ContainsKey($k) -or $v -lt $best[$k]) { $best[$k] = $v }
            }
        }
    }
    $all[$r.n] = $best
    $missing = @($benches | Where-Object { -not $best.ContainsKey($_) })
    if ($missing.Count -gt 0) { Write-Host ("  " + $r.n + " missing: " + ($missing -join ",")) }
}

Write-Host ""
$header = "{0,-12}" -f "language"
foreach ($b in $benches) { $header += ("{0,9}" -f $b) }
Write-Host $header
foreach ($r in $runs) {
    $line = "{0,-12}" -f $r.n
    foreach ($b in $benches) {
        $v = $all[$r.n][$b]
        if ($null -eq $v) { $line += ("{0,9}" -f "-") } else { $line += ("{0,9:N1}" -f $v) }
    }
    Write-Host $line
}
Write-Host ""
Write-Host "checksums (must agree; see README for the two documented exceptions):"
foreach ($r in $runs) {
    $c = $all[$r.n]["checksum"]
    Write-Host ("  {0,-12} {1}" -f $r.n, $(if ($null -eq $c) { "-" } else { $c }))
}
