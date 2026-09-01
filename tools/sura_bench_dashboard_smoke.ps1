param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_bench_dashboard_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-JsonText {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
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
    },
    {
      "generated_utc": "2026-01-02T00:00:00Z",
      "benchmarks": [
        {"benchmark": "bench_jit.sura", "speedup": 2.4}
      ],
      "python_comparisons": [
        {"label": "AI agent task scoring", "sura_faster_by": 4.5}
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

    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "sura_bench_dashboard.ps1") `
        -Engine $EnginePath `
        -Out $html `
        -JsonOut $json `
        -SummaryOut $summary `
        -ReleaseNotesOut $releaseNotes `
        -HistoryIn $history `
        -HistoryOut $history `
        -NativePerfIn $nativePerf 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Write-Output ($out -join "`n")
        throw "expected benchmark dashboard generation to pass"
    }

    $htmlText = Get-Content -Raw -Path $html
    if ($htmlText -notmatch "<svg" -or
        $htmlText -notmatch "JIT Speedup Chart" -or
        $htmlText -notmatch "Recent Benchmark History" -or
        $htmlText -notmatch "polyline") {
        throw "expected benchmark dashboard HTML to include SVG bar and trend charts"
    }
    if ($htmlText -notmatch "Official benchmark evidence" -or
        $htmlText -notmatch "dashboard-header" -or
        $htmlText -notmatch "Fair native baseline") {
        throw "expected benchmark dashboard HTML to use official evidence layout"
    }
    if ($htmlText -notmatch "Benchmark Summary" -or
        $htmlText -notmatch "Average JIT speedup" -or
        $htmlText -notmatch "Native C\+\+ Baseline" -or
        $htmlText -notmatch "game physics Vec3 loop" -or
        $htmlText -notmatch "Sura/native ratio") {
        throw "expected benchmark dashboard HTML to include summary metrics"
    }

    $report = Get-Content -Raw -Path $json | ConvertFrom-Json
    if ($report.benchmark_contract -ne "sura.semantic-jit.v3" -or
        -not $report.generated_utc -or -not $report.benchmarks -or $report.benchmarks.Count -ne 23 -or
        -not $report.summary -or $report.summary.benchmark_count -lt 1 -or
        -not $report.release_notes -or -not $report.release_notes.headline -or
        -not $report.native_performance -or [double]$report.native_performance.sura_native_ratio -le 0 -or
        -not $report.native_performance.native_3d_available -or
        -not ($report.native_performance.baselines | Where-Object { $_.id -eq "vec3" -and [double]$_.sura_native_ratio -gt 0 })) {
        throw "expected benchmark dashboard JSON report"
    }
    $fibBenchmark = $report.benchmarks |
        Where-Object { $_.benchmark -eq "bench_fib.sura" } |
        Select-Object -First 1
    $fibComparison = $report.python_comparisons |
        Where-Object { $_.label -eq "fib(30)" } |
        Select-Object -First 1
    if (-not $fibBenchmark -or -not $fibComparison -or
        [double]$fibComparison.sura_jit_ms -le 0 -or
        [double]$fibComparison.sura_jit_ms -ge
            ([double]$fibBenchmark.jit_ms * 0.6)) {
        throw "expected fib Python comparison to use the script's per-run average, not the five-run Execute total"
    }
    $benchNames = @($report.benchmarks | ForEach-Object { $_.benchmark })
    if ($benchNames -notcontains "bench_api_log_etl.sura") {
        throw "expected benchmark dashboard JSON to include API log ETL benchmark"
    }
    if ($benchNames -notcontains "bench_ai_schema.sura") {
        throw "expected benchmark dashboard JSON to include AI schema validation benchmark"
    }
    if ($benchNames -notcontains "bench_rag_vector.sura") {
        throw "expected benchmark dashboard JSON to include RAG vector ranking benchmark"
    }
    if ($benchNames -notcontains "bench_policy_gate.sura") {
        throw "expected benchmark dashboard JSON to include AI tool policy gate benchmark"
    }
    if ($benchNames -notcontains "bench_guardrail.sura") {
        throw "expected benchmark dashboard JSON to include AI guardrail benchmark"
    }
    if ($benchNames -notcontains "bench_dependency_resolver.sura") {
        throw "expected benchmark dashboard JSON to include dependency resolver benchmark"
    }
    if ($benchNames -notcontains "bench_workflow_scheduler.sura") {
        throw "expected benchmark dashboard JSON to include workflow scheduler benchmark"
    }
    if ($benchNames -notcontains "bench_order_etl.sura") {
        throw "expected benchmark dashboard JSON to include order ETL benchmark"
    }
    if ($benchNames -notcontains "bench_telemetry_window.sura") {
        throw "expected benchmark dashboard JSON to include telemetry window benchmark"
    }
    if ($benchNames -notcontains "bench_log_regex.sura") {
        throw "expected benchmark dashboard JSON to include log regex benchmark"
    }
    if ($benchNames -notcontains "bench_fraud_scoring.sura") {
        throw "expected benchmark dashboard JSON to include fraud scoring benchmark"
    }
    if ($benchNames -notcontains "bench_feature_flags.sura") {
        throw "expected benchmark dashboard JSON to include feature flag benchmark"
    }
    if ($benchNames -notcontains "bench_physics_inplace.sura") {
        throw "expected benchmark dashboard JSON to include in-place physics benchmark"
    }
    if ($benchNames -notcontains "bench_physics3d.sura") {
        throw "expected benchmark dashboard JSON to include 3D physics benchmark"
    }
    if ($report.python_comparisons -and $report.python_comparisons.Count -gt 0) {
        $pythonLabels = @($report.python_comparisons | ForEach-Object { $_.label })
        if ($pythonLabels -notcontains "API log ETL aggregation") {
            throw "expected benchmark dashboard JSON to include API log ETL Python comparison"
        }
        if ($pythonLabels -notcontains "AI JSON/schema validation") {
            throw "expected benchmark dashboard JSON to include AI schema validation Python comparison"
        }
        if ($pythonLabels -notcontains "RAG vector ranking") {
            throw "expected benchmark dashboard JSON to include RAG vector ranking Python comparison"
        }
        if ($pythonLabels -notcontains "AI tool policy gate") {
            throw "expected benchmark dashboard JSON to include AI tool policy gate Python comparison"
        }
        if ($pythonLabels -notcontains "AI guardrail event scoring") {
            throw "expected benchmark dashboard JSON to include AI guardrail Python comparison"
        }
        if ($pythonLabels -notcontains "dependency resolver hot loop") {
            throw "expected benchmark dashboard JSON to include dependency resolver Python comparison"
        }
        if ($pythonLabels -notcontains "automation workflow scheduler") {
            throw "expected benchmark dashboard JSON to include workflow scheduler Python comparison"
        }
        if ($pythonLabels -notcontains "order CSV normalization ETL") {
            throw "expected benchmark dashboard JSON to include order ETL Python comparison"
        }
        if ($pythonLabels -notcontains "telemetry rolling window") {
            throw "expected benchmark dashboard JSON to include telemetry window Python comparison"
        }
        if ($pythonLabels -notcontains "regex log summarization") {
            throw "expected benchmark dashboard JSON to include log regex Python comparison"
        }
        if ($pythonLabels -notcontains "payment fraud scoring") {
            throw "expected benchmark dashboard JSON to include fraud scoring Python comparison"
        }
        if ($pythonLabels -notcontains "feature flag rollout") {
            throw "expected benchmark dashboard JSON to include feature flag Python comparison"
        }
        if ($pythonLabels -notcontains "game physics in-place Vec2 loop") {
            throw "expected benchmark dashboard JSON to include in-place physics Python comparison"
        }
        if ($pythonLabels -notcontains "game physics Vec3 loop") {
            throw "expected benchmark dashboard JSON to include 3D physics Python comparison"
        }
    }

    $summaryText = Get-Content -Raw -Path $summary
    if ($summaryText -notmatch "# Sura Benchmark Summary" -or
        $summaryText -notmatch "Average JIT speedup" -or
        $summaryText -notmatch "Python Comparison" -or
        $summaryText -notmatch "Native C\+\+ Baseline" -or
        $summaryText -notmatch "Native C\+\+ 3D comparison") {
        throw "expected benchmark markdown summary"
    }
    $releaseNotesText = Get-Content -Raw -Path $releaseNotes
    if ($releaseNotesText -notmatch "# Sura Benchmark Release Notes" -or
        $releaseNotesText -notmatch "Faster-than-Python cases" -or
        $releaseNotesText -notmatch "Python Comparison Highlights" -or
        $releaseNotesText -notmatch "Native C\+\+ Baseline" -or
        $releaseNotesText -notmatch "Native C\+\+ 3D baseline" -or
        $releaseNotesText -notmatch "Dashboard JSON includes") {
        throw "expected benchmark release notes evidence"
    }

    $historyReport = Get-Content -Raw -Path $history | ConvertFrom-Json
    if (-not $historyReport.entries -or $historyReport.entries.Count -lt 3) {
        throw "expected benchmark history to append current report"
    }

    "bench_dashboard_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# A gate that prints PASS must also exit 0 rather than inheriting
# whatever the last command happened to return.
exit 0
