param(
    [string]$Surapkg = ".\surapkg.exe",
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$SurapkgPath = (Resolve-Path $Surapkg).Path
$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_bench_dashboard_cli_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-JsonText {
    param([string]$Path, [string]$Text)
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

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $history = Join-Path $temp "bench_history.json"
    $html = Join-Path $temp "bench_dashboard.html"
    $json = Join-Path $temp "bench_dashboard.json"
    $summary = Join-Path $temp "bench_summary.md"
    $releaseNotes = Join-Path $temp "bench_release_notes.md"
    $nativePerf = Join-Path $temp "native_perf.json"

    Write-JsonText $history @"
{
  "updated_utc": "2026-01-01T00:00:00Z",
  "entries": [
    {
      "generated_utc": "2026-01-01T00:00:00Z",
      "benchmarks": [
        {"benchmark": "bench_jit.sura", "speedup": 2.0}
      ],
      "python_comparisons": [
        {"label": "AI agent task scoring", "sura_faster_by": 4.0}
      ]
    }
  ]
}
"@

    Write-JsonText $nativePerf @"
{
  "schema": "sura.native.performance.v1",
  "generated_utc": "2026-01-02T00:00:00Z",
  "passed": true,
  "compiler": "g++",
  "benchmark": "game physics Vec2 loop",
  "sura_file": "bench_physics.sura",
  "native_file": "bench_physics_native.cpp",
  "sura_jit_ms": 0.052,
  "sura_jit_time_source": "script_loop",
  "sura_jit_run_count": 5,
  "native_ms": 0.048,
  "native_time_source": "native_avg",
  "sura_native_ratio": 1.0833333333,
  "native_faster_by": 1.0833333333,
  "baseline_count": 2,
  "baselines": [
    {
      "id": "vec2",
      "benchmark": "game physics Vec2 loop",
      "sura_file": "bench_physics.sura",
      "native_file": "bench_physics_native.cpp",
      "dimension": "vec2",
      "measurement_scope": {"timed_region": "inner physics loop only", "steps": 100000, "sura_runs": 5, "native_runs": 5},
      "fair_scope_passed": true,
      "sura_jit_ms": 0.052,
      "sura_jit_time_source": "script_loop",
      "sura_jit_run_count": 5,
      "native_ms": 0.048,
      "native_time_source": "native_avg",
      "sura_native_ratio": 1.0833333333,
      "warnings": []
    },
    {
      "id": "vec3",
      "benchmark": "game physics Vec3 loop",
      "sura_file": "bench_physics3d.sura",
      "native_file": "bench_physics_native.cpp",
      "dimension": "vec3",
      "measurement_scope": {"timed_region": "inner physics loop only", "steps": 100000, "sura_runs": 5, "native_runs": 5},
      "fair_scope_passed": true,
      "sura_jit_ms": 41.0,
      "sura_jit_time_source": "script_loop",
      "sura_jit_run_count": 5,
      "native_ms": 0.06,
      "native_time_source": "native_loop",
      "sura_native_ratio": 683.3333333333,
      "warnings": ["vec3 evidence"]
    }
  ],
  "warnings": []
}
"@

    $dash = Run-Pkg -PkgArgs @(
        "bench-dashboard",
        "--engine", $EnginePath,
        "--out", $html,
        "--json", $json,
        "--summary", $summary,
        "--release-notes", $releaseNotes,
        "--history-in", $history,
        "--history-out", $history,
        "--native-perf", $nativePerf,
        "--history-limit", "10"
    )
    if ($dash.Code -ne 0 -or
        $dash.Output -notmatch "benchmark dashboard:" -or
        $dash.Output -notmatch "benchmark dashboard JSON:" -or
        $dash.Output -notmatch "benchmark dashboard summary:" -or
        $dash.Output -notmatch "benchmark dashboard release notes:" -or
        $dash.Output -notmatch "benchmark dashboard history:" -or
        $dash.Output -notmatch "benchmark dashboard native performance:") {
        Write-Output $dash.Output
        throw "expected surapkg bench-dashboard to generate all artifacts"
    }
    foreach ($path in @($html, $json, $summary, $releaseNotes, $history)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "expected bench-dashboard artifact: $path"
        }
    }

    $htmlText = Get-Content -Raw -LiteralPath $html
    if ($htmlText -notmatch "Benchmark Summary" -or
        $htmlText -notmatch "JIT Speedup Chart" -or
        $htmlText -notmatch "Native C\+\+ Baseline" -or
        $htmlText -notmatch "game physics Vec3 loop" -or
        $htmlText -notmatch "<svg") {
        throw "expected bench-dashboard HTML charts and summary"
    }

    $report = Get-Content -Raw -LiteralPath $json | ConvertFrom-Json
    if (-not $report.generated_utc -or
        -not $report.summary -or
        $report.summary.benchmark_count -lt 1 -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_api_log_etl.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_guardrail.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_dependency_resolver.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_workflow_scheduler.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_telemetry_window.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_log_regex.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_fraud_scoring.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_feature_flags.sura" }) -or
        -not ($report.benchmarks | Where-Object { $_.benchmark -eq "bench_physics3d.sura" }) -or
        -not $report.native_performance -or [double]$report.native_performance.sura_native_ratio -le 0 -or
        -not $report.native_performance.native_3d_available -or
        -not ($report.native_performance.baselines | Where-Object { $_.id -eq "vec3" -and [double]$_.sura_native_ratio -gt 0 })) {
        throw "expected bench-dashboard JSON summary and benchmark records"
    }

    $summaryText = Get-Content -Raw -LiteralPath $summary
    if ($summaryText -notmatch "# Sura Benchmark Summary" -or
        $summaryText -notmatch "Average JIT speedup" -or
        $summaryText -notmatch "Native C\+\+ Baseline" -or
        $summaryText -notmatch "Native C\+\+ 3D comparison") {
        throw "expected bench-dashboard Markdown summary"
    }
    $releaseNotesText = Get-Content -Raw -LiteralPath $releaseNotes
    if ($releaseNotesText -notmatch "# Sura Benchmark Release Notes" -or
        $releaseNotesText -notmatch "Faster-than-Python cases" -or
        $releaseNotesText -notmatch "Python Comparison Highlights" -or
        $releaseNotesText -notmatch "Native C\+\+ Baseline" -or
        $releaseNotesText -notmatch "Native C\+\+ 3D baseline" -or
        $releaseNotesText -notmatch "Dashboard JSON includes") {
        throw "expected bench-dashboard release notes evidence"
    }

    $historyReport = Get-Content -Raw -LiteralPath $history | ConvertFrom-Json
    if (-not $historyReport.entries -or @($historyReport.entries).Count -lt 2) {
        throw "expected bench-dashboard history append"
    }

    $help = Run-Pkg -PkgArgs @("bench-dashboard", "--help")
    if ($help.Code -ne 0 -or $help.Output -notmatch "bench-dashboard" -or $help.Output -notmatch "--native-perf") {
        Write-Output $help.Output
        throw "expected bench-dashboard help to pass"
    }

    "bench_dashboard_cli_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
