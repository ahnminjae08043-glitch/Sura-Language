$ErrorActionPreference = "Stop"

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_bench_gate_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-JsonText {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Gate {
    param(
        [string]$Report,
        [string]$History,
        [double]$MaxRegressionPercent = 25.0,
        [string[]]$ExtraArgs = @()
    )
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $cmdArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $PSScriptRoot "sura_bench_gate.ps1"),
        "-Report", $Report,
        "-History", $History,
        "-MaxRegressionPercent", $MaxRegressionPercent
    )
    $cmdArgs += $ExtraArgs
    $out = & powershell @cmdArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $historyPath = Join-Path $temp "bench_history.json"
    $okPath = Join-Path $temp "bench_ok.json"
    $badPath = Join-Path $temp "bench_bad.json"
    $newContractPath = Join-Path $temp "bench_new_contract.json"
    $noHistoryPath = Join-Path $temp "missing_history.json"

    Write-JsonText $historyPath @"
{
  "updated_utc": "2026-01-01T00:00:00Z",
  "entries": [
    {
      "generated_utc": "2026-01-01T00:00:00Z",
      "benchmarks": [
        {"benchmark": "bench_jit.sura", "jit_ms": 100.0, "speedup": 2.0}
      ],
      "python_comparisons": [
        {"label": "AI agent task scoring", "sura_jit_ms": 20.0, "sura_faster_by": 4.0}
      ]
    }
  ]
}
"@

    Write-JsonText $newContractPath @"
{
  "benchmark_contract": "sura.semantic-jit.v3",
  "generated_utc": "2026-01-04T00:00:00Z",
  "benchmarks": [
    {"benchmark": "bench_jit.sura", "jit_ms": 10000.0, "speedup": 0.1}
  ],
  "python_comparisons": [
    {"label": "AI agent task scoring", "sura_jit_ms": 10000.0, "sura_faster_by": 0.1}
  ]
}
"@

    Write-JsonText $okPath @"
{
  "generated_utc": "2026-01-02T00:00:00Z",
  "benchmarks": [
    {"benchmark": "bench_jit.sura", "jit_ms": 110.0, "speedup": 1.8}
  ],
  "python_comparisons": [
    {"label": "AI agent task scoring", "sura_jit_ms": 22.0, "sura_faster_by": 3.6}
  ]
}
"@

    Write-JsonText $badPath @"
{
  "generated_utc": "2026-01-03T00:00:00Z",
  "benchmarks": [
    {"benchmark": "bench_jit.sura", "jit_ms": 160.0, "speedup": 1.1}
  ],
  "python_comparisons": [
    {"label": "AI agent task scoring", "sura_jit_ms": 40.0, "sura_faster_by": 1.2}
  ]
}
"@

    $ok = Run-Gate $okPath $historyPath
    if ($ok.Code -ne 0) {
        Write-Output $ok.Output
        throw "expected non-regressed benchmark gate to pass"
    }

    $bad = Run-Gate $badPath $historyPath
    if ($bad.Code -eq 0 -or $bad.Output -notmatch "regressed") {
        Write-Output $bad.Output
        throw "expected regressed benchmark gate to fail"
    }

    $newContract = Run-Gate $newContractPath $historyPath
    if ($newContract.Code -ne 0 -or $newContract.Output -notmatch "no previous benchmark baseline") {
        Write-Output $newContract.Output
        throw "expected a new benchmark contract to establish a fresh baseline"
    }

    $coverageBad = Run-Gate $okPath $historyPath -ExtraArgs @(
        "-RequiredBenchmarks", "bench_jit.sura,bench_ai_schema.sura",
        "-RequiredPythonComparisons", "AI agent task scoring,RAG vector ranking"
    )
    if ($coverageBad.Code -eq 0 -or
        $coverageBad.Output -notmatch "required benchmark missing" -or
        $coverageBad.Output -notmatch "required Python comparison missing") {
        Write-Output $coverageBad.Output
        throw "expected missing required benchmark coverage to fail"
    }

    $coverageOk = Run-Gate $okPath $historyPath -ExtraArgs @(
        "-RequiredBenchmarks", "bench_jit.sura",
        "-RequiredPythonComparisons", "AI agent task scoring"
    )
    if ($coverageOk.Code -ne 0) {
        Write-Output $coverageOk.Output
        throw "expected required benchmark coverage to pass"
    }

    $thresholdNoBaseline = Run-Gate $badPath $noHistoryPath -ExtraArgs @(
        "-MinJitSpeedup", "1.5",
        "-MinPythonFasterBy", "2.0"
    )
    if ($thresholdNoBaseline.Code -eq 0 -or $thresholdNoBaseline.Output -notmatch "below required") {
        Write-Output $thresholdNoBaseline.Output
        throw "expected minimum thresholds to fail without a baseline"
    }

    "bench_gate_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
