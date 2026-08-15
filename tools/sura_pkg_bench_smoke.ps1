param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_bench_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $Text = $Text -replace "`r`n", "`n"
    if (-not $Text.EndsWith("`n")) { $Text += "`n" }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

function Find-Python {
    $candidates = @(
        $env:SURA_PYTHON,
        "C:\msys64\mingw64\bin\python.exe",
        "C:\msys64\ucrt64\bin\python.exe",
        "python",
        "python3"
    )
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        try {
            $version = & $candidate --version 2>&1 | Out-String
            if ($LASTEXITCODE -eq 0 -and $version -match "Python\s+\d+\.\d+") {
                return $candidate
            }
        } catch {
        }
    }
    return $null
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $pkg = Join-Path $temp "bench_pkg"
    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "bench_pkg",
  "version": "1.0.0",
  "main": "src/bench_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/bench_pkg.sura") @"
func mix(a, b) do
  return (a * 3 + b) % 9973
end

total is 0
i is 0
while i < 12000 do
  total is mix(total, i)
  if total > 5000 then
    total is total - 17
  else
    total is total + 11
  end
  i is i + 1
end

print total
"@
    Write-Text (Join-Path $pkg "bench_pkg.py") @"
import time

def mix(a, b):
    return (a * 3 + b) % 9973

runs = 5
total_ms = 0.0
answer = 0
for _ in range(runs):
    start = time.perf_counter()
    total = 0
    i = 0
    while i < 12000:
        total = mix(total, i)
        if total > 5000:
            total = total - 17
        else:
            total = total + 11
        i = i + 1
    total_ms += (time.perf_counter() - start) * 1000
    answer = total

print(answer)
print(f"avg ({runs} runs): {total_ms / runs:.3f} ms")
"@

    $json = Join-Path $temp "bench.json"
    $summary = Join-Path $temp "bench.md"
    $bench = Run-Pkg -PkgArgs @("bench", $pkg, "--json", $json, "--summary", $summary, "--min-speedup", "0.001", "--", "--mode=fast")
    if ($bench.Code -ne 0 -or
        $bench.Output -notmatch "(?m)^\[info\] interpreter benchmark" -or
        $bench.Output -notmatch "(?m)^\[info\] JIT benchmark" -or
        $bench.Output -notmatch "benchmarked package main" -or
        $bench.Output -notmatch "JIT speedup" -or
        $bench.Output -notmatch "bench summary" -or
        -not (Test-Path -LiteralPath $json) -or
        -not (Test-Path -LiteralPath $summary)) {
        Write-Output $bench.Output
        throw "expected surapkg bench to run interpreter/JIT and write JSON plus Markdown summary"
    }

    $report = Get-Content -Raw -LiteralPath $json | ConvertFrom-Json
    if ($report.schema -ne "sura.package.bench.v1") {
        throw "expected package bench schema"
    }
    if ($report.package -ne "bench_pkg" -or $report.main -ne "src/bench_pkg.sura") {
        throw "expected package metadata in bench report"
    }
    if ($report.interpreter.execute_ms -le 0 -or $report.jit.execute_ms -le 0) {
        throw "expected interpreter and JIT execute timings"
    }
    if ($report.speedup -le 0) {
        throw "expected positive JIT speedup ratio"
    }
    $summaryText = Get-Content -Raw -LiteralPath $summary
    if ($summaryText -notmatch "# Sura Package Benchmark Summary" -or
        $summaryText -notmatch "\| Package \| bench_pkg \|" -or
        $summaryText -notmatch "\| JIT speedup \| [0-9.]+x \|" -or
        $summaryText -notmatch "\| Interpreter \|") {
        throw "expected Markdown benchmark summary with package metadata and timings"
    }

    $textOnly = Run-Pkg -PkgArgs @("bench", $pkg, "--no-jit")
    if ($textOnly.Code -ne 0 -or
        $textOnly.Output -notmatch "(?m)^\[info\] interpreter benchmark" -or
        $textOnly.Output -match "(?m)^\[info\] JIT benchmark" -or
        $textOnly.Output -notmatch "benchmarked package main") {
        Write-Output $textOnly.Output
        throw "expected no-JIT package bench to run only interpreter benchmark"
    }

    $gateFail = Run-Pkg -PkgArgs @("bench", $pkg, "--min-speedup", "999")
    if ($gateFail.Code -eq 0 -or $gateFail.Output -notmatch "below minimum") {
        Write-Output $gateFail.Output
        throw "expected min-speedup gate to fail"
    }

    $python = Find-Python
    if ($python) {
        $oldPython = $env:SURA_PYTHON
        $env:SURA_PYTHON = $python
        try {
            $pythonJson = Join-Path $temp "bench_python.json"
            $pythonSummary = Join-Path $temp "bench_python.md"
            $pyBench = Run-Pkg -PkgArgs @("bench", $pkg, "--json", $pythonJson, "--summary", $pythonSummary, "--python", "bench_pkg.py")
            if ($pyBench.Code -ne 0 -or
                $pyBench.Output -notmatch "(?m)^\[info\] Python comparison benchmark" -or
                $pyBench.Output -notmatch "Sura faster than Python" -or
                -not (Test-Path -LiteralPath $pythonJson) -or
                -not (Test-Path -LiteralPath $pythonSummary)) {
                Write-Output $pyBench.Output
                throw "expected surapkg bench Python comparison to pass"
            }
            $pyReport = Get-Content -Raw -LiteralPath $pythonJson | ConvertFrom-Json
            if (-not $pyReport.python -or
                $pyReport.python.script -ne "bench_pkg.py" -or
                $pyReport.python.ms -le 0 -or
                $pyReport.sura_faster_by_python -le 0) {
                throw "expected Python comparison fields in bench report"
            }
            $pySummaryText = Get-Content -Raw -LiteralPath $pythonSummary
            if ($pySummaryText -notmatch "## Python Comparison" -or
                $pySummaryText -notmatch '\| `bench_pkg\.py` \|' -or
                $pySummaryText -notmatch "\| JIT speedup \| [0-9.]+x \|") {
                throw "expected Markdown benchmark summary with Python comparison"
            }
        }
        finally {
            $env:SURA_PYTHON = $oldPython
        }
    }

    "pkg_bench_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# Verified passing; state the exit code rather than inheriting it.
exit 0
