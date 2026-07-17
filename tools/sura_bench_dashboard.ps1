param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Out = "bench_dashboard.html",
    [string]$JsonOut = "",
    [string]$SummaryOut = "",
    [string]$ReleaseNotesOut = "",
    [string]$HistoryIn = "",
    [string]$HistoryOut = "",
    [string]$NativePerfIn = "",
    [int]$HistoryLimit = 100
)

$root = Split-Path -Parent $PSScriptRoot
$benches = @(
    "bench_jit.sura",
    "bench_fib.sura",
    "bench_agent_scoring.sura",
    "bench_ai_schema.sura",
    "bench_api_log_etl.sura",
    "bench_rag_vector.sura",
    "bench_tool_routing.sura",
    "bench_policy_gate.sura",
    "bench_guardrail.sura",
    "bench_dependency_resolver.sura",
    "bench_workflow_scheduler.sura",
    "bench_order_etl.sura",
    "bench_telemetry_window.sura",
    "bench_log_regex.sura",
    "bench_fraud_scoring.sura",
    "bench_feature_flags.sura",
    "bench_physics.sura",
    "bench_physics_inplace.sura",
    "bench_physics3d.sura",
    "bench_market.sura"
)
$rows = New-Object System.Collections.Generic.List[string]
$pythonRows = New-Object System.Collections.Generic.List[string]
$benchRecords = New-Object System.Collections.Generic.List[object]
$pythonRecords = New-Object System.Collections.Generic.List[object]

function Find-Python {
    $candidates = @("python", "py", "C:\msys64\mingw64\bin\python.exe")
    foreach ($candidate in $candidates) {
        try {
            $probe = & $candidate -c "import sys; print('Python ' + sys.version.split()[0])" 2>&1 | Out-String
            if ($LASTEXITCODE -eq 0 -and $probe -match "Python\s+\d") {
                return $candidate
            }
        } catch {
        }
    }
    return $null
}

function Extract-AvgMs {
    param([string]$Text)
    $ascii = [regex]::Match($Text, 'avg \(\d+ runs\):\s+([0-9.]+) ms')
    if ($ascii.Success) { return $ascii.Groups[1].Value }
    $legacy = [regex]::Match($Text, '\(\d+[^\)]*\):\s+([0-9.]+) ms')
    if ($legacy.Success) { return $legacy.Groups[1].Value }
    $execute = [regex]::Match($Text, 'Execute:\s+([0-9.]+) ms')
    if ($execute.Success) { return $execute.Groups[1].Value }
    return ""
}

function Maybe-Number {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    return [double]$Text
}

function Ensure-ParentDirectory {
    param([string]$Path)
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path $parent)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }
}

function Project-Path {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return "" }
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $root $Path
}

function Read-HistoryEntries {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) { return @() }
    try {
        $parsed = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
        if ($parsed.entries) { return @($parsed.entries) }
        return @($parsed)
    } catch {
        Write-Warning "Ignoring unreadable benchmark history: $Path ($($_.Exception.Message))"
        return @()
    }
}

function Read-NativePerfReport {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) { return $null }
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    } catch {
        Write-Warning "Ignoring unreadable native performance report: $Path"
        return $null
    }
}

function Optional-Double {
    param($Value)
    if ($null -eq $Value) { return $null }
    if ([string]::IsNullOrWhiteSpace([string]$Value)) { return $null }
    return [double]$Value
}

function Average-Value {
    param($Items, [string]$Field)
    $sum = 0.0
    $count = 0
    foreach ($item in (As-Array $Items)) {
        if ($null -ne $item.$Field) {
            $sum += [double]$item.$Field
            $count += 1
        }
    }
    if ($count -eq 0) { return $null }
    return $sum / $count
}

function Html-Escape {
    param([string]$Text)
    if ($null -eq $Text) { return "" }
    return [System.Net.WebUtility]::HtmlEncode($Text)
}

function Format-ChartNumber {
    param($Value)
    if ($null -eq $Value) { return "" }
    return "{0:N2}" -f ([double]$Value)
}

function Markdown-Escape {
    param([string]$Text)
    if ($null -eq $Text) { return "" }
    return ($Text -replace "\\", "\\\\" -replace "\|", "\|").Replace("`r", " ").Replace("`n", " ")
}

function Format-Ratio {
    param($Value)
    if ($null -eq $Value) { return "n/a" }
    return "{0:N2}x" -f ([double]$Value)
}

function Format-Ms {
    param($Value)
    if ($null -eq $Value) { return "n/a" }
    return "{0:N3} ms" -f ([double]$Value)
}

function New-BarChartSvg {
    param(
        $Items,
        [string]$LabelField,
        [string]$ValueField,
        [string]$Unit = "x",
        [string]$Color = "#2563eb",
        [int]$Limit = 10
    )
    $rows = @()
    foreach ($item in (As-Array $Items)) {
        if ($null -eq $item.$ValueField) { continue }
        $rows += [pscustomobject]@{
            Label = [string]$item.$LabelField
            Value = [double]$item.$ValueField
        }
    }
    $rows = @($rows | Sort-Object -Property Value -Descending | Select-Object -First $Limit)
    if ($rows.Count -eq 0) { return "<p class=""empty"">No chart data available.</p>" }
    $max = ($rows | Measure-Object -Property Value -Maximum).Maximum
    if ($max -le 0) { $max = 1 }
    $barHeight = 28
    $gap = 10
    $left = 210
    $right = 70
    $width = 860
    $plotWidth = $width - $left - $right
    $height = 28 + ($rows.Count * ($barHeight + $gap))
    $svg = New-Object System.Collections.Generic.List[string]
    $svg.Add("<svg class=""chart"" viewBox=""0 0 $width $height"" role=""img"">")
    $svg.Add("<line x1=""$left"" y1=""10"" x2=""$left"" y2=""$($height - 10)"" stroke=""#d4d4d8""/>")
    for ($i = 0; $i -lt $rows.Count; ++$i) {
        $row = $rows[$i]
        $y = 20 + ($i * ($barHeight + $gap))
        $barWidth = [Math]::Max(2, [Math]::Round(($row.Value / $max) * $plotWidth, 2))
        $label = Html-Escape $row.Label
        $valueText = Html-Escape ((Format-ChartNumber $row.Value) + $Unit)
        $svg.Add("<text x=""0"" y=""$($y + 19)"" class=""chart-label"">$label</text>")
        $svg.Add("<rect x=""$left"" y=""$y"" width=""$barWidth"" height=""$barHeight"" rx=""4"" fill=""$Color""></rect>")
        $svg.Add("<text x=""$($left + $barWidth + 8)"" y=""$($y + 19)"" class=""chart-value"">$valueText</text>")
    }
    $svg.Add("</svg>")
    return ($svg -join "`n")
}

function New-HistoryChartSvg {
    param($Entries)
    $points = @()
    foreach ($entry in @((As-Array $Entries) | Select-Object -Last 24)) {
        $jit = Average-Value $entry.benchmarks "speedup"
        $python = Average-Value $entry.python_comparisons "sura_faster_by"
        if ($null -eq $jit -and $null -eq $python) { continue }
        $points += [pscustomobject]@{
            Label = [string]$entry.generated_utc
            Jit = $jit
            Python = $python
        }
    }
    if ($points.Count -lt 2) { return "<p class=""empty"">Add more benchmark history entries to show trend lines.</p>" }

    $width = 860
    $height = 280
    $left = 62
    $right = 24
    $top = 24
    $bottom = 54
    $plotWidth = $width - $left - $right
    $plotHeight = $height - $top - $bottom
    $values = @()
    foreach ($point in $points) {
        if ($null -ne $point.Jit) { $values += [double]$point.Jit }
        if ($null -ne $point.Python) { $values += [double]$point.Python }
    }
    $max = ($values | Measure-Object -Maximum).Maximum
    if ($max -le 0) { $max = 1 }
    $max = [Math]::Ceiling($max * 10) / 10
    $denom = [Math]::Max(1, $points.Count - 1)
    $scaleX = { param($i) $left + (($i / $denom) * $plotWidth) }
    $scaleY = { param($v) $top + ($plotHeight - (($v / $max) * $plotHeight)) }

    $jitPoints = New-Object System.Collections.Generic.List[string]
    $pythonPoints = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $points.Count; ++$i) {
        $x = & $scaleX $i
        if ($null -ne $points[$i].Jit) {
            $jitPoints.Add(("{0:N2},{1:N2}" -f $x, (& $scaleY ([double]$points[$i].Jit))))
        }
        if ($null -ne $points[$i].Python) {
            $pythonPoints.Add(("{0:N2},{1:N2}" -f $x, (& $scaleY ([double]$points[$i].Python))))
        }
    }

    $svg = New-Object System.Collections.Generic.List[string]
    $svg.Add("<svg class=""chart trend"" viewBox=""0 0 $width $height"" role=""img"">")
    $svg.Add("<line x1=""$left"" y1=""$top"" x2=""$left"" y2=""$($top + $plotHeight)"" stroke=""#d4d4d8""/>")
    $svg.Add("<line x1=""$left"" y1=""$($top + $plotHeight)"" x2=""$($left + $plotWidth)"" y2=""$($top + $plotHeight)"" stroke=""#d4d4d8""/>")
    for ($tick = 0; $tick -le 4; ++$tick) {
        $value = $max * $tick / 4
        $y = & $scaleY $value
        $svg.Add("<line x1=""$left"" y1=""$y"" x2=""$($left + $plotWidth)"" y2=""$y"" stroke=""#f1f5f9""/>")
        $svg.Add("<text x=""6"" y=""$($y + 4)"" class=""chart-value"">$((Format-ChartNumber $value))x</text>")
    }
    if ($jitPoints.Count -gt 1) {
        $svg.Add("<polyline points=""$($jitPoints -join ' ')"" fill=""none"" stroke=""#2563eb"" stroke-width=""3""/>")
    }
    if ($pythonPoints.Count -gt 1) {
        $svg.Add("<polyline points=""$($pythonPoints -join ' ')"" fill=""none"" stroke=""#16a34a"" stroke-width=""3""/>")
    }
    $firstLabel = Html-Escape $points[0].Label
    $lastLabel = Html-Escape $points[-1].Label
    $svg.Add("<text x=""$left"" y=""$($height - 18)"" class=""chart-label"">$firstLabel</text>")
    $svg.Add("<text x=""$($left + $plotWidth - 220)"" y=""$($height - 18)"" class=""chart-label"">$lastLabel</text>")
    $svg.Add("<rect x=""$($width - 270)"" y=""16"" width=""16"" height=""10"" fill=""#2563eb""/><text x=""$($width - 248)"" y=""26"" class=""chart-label"">Avg JIT speedup</text>")
    $svg.Add("<rect x=""$($width - 270)"" y=""38"" width=""16"" height=""10"" fill=""#16a34a""/><text x=""$($width - 248)"" y=""48"" class=""chart-label"">Avg Sura/Python ratio</text>")
    $svg.Add("</svg>")
    return ($svg -join "`n")
}

function As-Array {
    param($Items)
    if ($null -eq $Items) { return @() }
    if ($Items -is [string]) { return @($Items) }
    if ($Items -is [System.Management.Automation.PSCustomObject]) { return @($Items) }
    if ($Items -is [System.Collections.IDictionary]) { return @($Items) }
    if ($Items -is [System.Collections.IEnumerable]) {
        $out = @()
        foreach ($item in $Items) { $out += $item }
        return $out
    }
    return @($Items)
}

$benchIndex = 0
foreach ($bench in $benches) {
    $path = Join-Path $root $bench
    if (-not (Test-Path $path)) {
        throw "Benchmark file not found: $path"
    }
    $benchIndex += 1
    Write-Host ("[bench {0}/{1}] {2}" -f $benchIndex, $benches.Count, $bench)
    $interp = & $Engine --bench $path 2>&1 | Out-String
    $jit = & $Engine --jit --bench $path 2>&1 | Out-String
    $interpExec = [regex]::Match($interp, 'Execute:\s+([0-9.]+) ms').Groups[1].Value
    $jitExec = [regex]::Match($jit, 'Execute:\s+([0-9.]+) ms').Groups[1].Value
    $speedup = ""
    if ($interpExec -and $jitExec -and [double]$jitExec -gt 0) {
        $speedup = "{0:N2}x" -f ([double]$interpExec / [double]$jitExec)
    }
    Write-Host ("  interpreter {0} ms, JIT {1} ms, speedup {2}" -f $interpExec, $jitExec, $(if ($speedup) { $speedup } else { "n/a" }))
    $rows.Add("<tr><td>$bench</td><td>$interpExec ms</td><td>$jitExec ms</td><td>$speedup</td></tr>")
    $benchRecords.Add([pscustomobject]@{
        benchmark = $bench
        interpreter_ms = Maybe-Number $interpExec
        jit_ms = Maybe-Number $jitExec
        speedup = if ($interpExec -and $jitExec -and [double]$jitExec -gt 0) { [double]$interpExec / [double]$jitExec } else { $null }
    })
}

$python = Find-Python
$pythonPairs = @(
    @{ Label = "fib(30)"; Python = "bench_python.py"; Sura = "bench_fib.sura" },
    @{ Label = "AI agent task scoring"; Python = "bench_agent_scoring.py"; Sura = "bench_agent_scoring.sura" },
    @{ Label = "AI JSON/schema validation"; Python = "bench_ai_schema.py"; Sura = "bench_ai_schema.sura" },
    @{ Label = "API log ETL aggregation"; Python = "bench_api_log_etl.py"; Sura = "bench_api_log_etl.sura" },
    @{ Label = "RAG vector ranking"; Python = "bench_rag_vector.py"; Sura = "bench_rag_vector.sura" },
    @{ Label = "AI tool routing scheduler"; Python = "bench_tool_routing.py"; Sura = "bench_tool_routing.sura" },
    @{ Label = "AI tool policy gate"; Python = "bench_policy_gate.py"; Sura = "bench_policy_gate.sura" },
    @{ Label = "AI guardrail event scoring"; Python = "bench_guardrail.py"; Sura = "bench_guardrail.sura" },
    @{ Label = "dependency resolver hot loop"; Python = "bench_dependency_resolver.py"; Sura = "bench_dependency_resolver.sura" },
    @{ Label = "automation workflow scheduler"; Python = "bench_workflow_scheduler.py"; Sura = "bench_workflow_scheduler.sura" },
    @{ Label = "order CSV normalization ETL"; Python = "bench_order_etl.py"; Sura = "bench_order_etl.sura" },
    @{ Label = "telemetry rolling window"; Python = "bench_telemetry_window.py"; Sura = "bench_telemetry_window.sura" },
    @{ Label = "regex log summarization"; Python = "bench_log_regex.py"; Sura = "bench_log_regex.sura" },
    @{ Label = "payment fraud scoring"; Python = "bench_fraud_scoring.py"; Sura = "bench_fraud_scoring.sura" },
    @{ Label = "feature flag rollout"; Python = "bench_feature_flags.py"; Sura = "bench_feature_flags.sura" },
    @{ Label = "game physics Vec2 loop"; Python = "bench_physics.py"; Sura = "bench_physics.sura" },
    @{ Label = "game physics in-place Vec2 loop"; Python = "bench_physics_inplace.py"; Sura = "bench_physics_inplace.sura" },
    @{ Label = "game physics Vec3 loop"; Python = "bench_physics3d.py"; Sura = "bench_physics3d.sura" },
    @{ Label = "market simulation objects"; Python = "bench_market.py"; Sura = "bench_market.sura" }
)
if ($python) {
    $pythonIndex = 0
    foreach ($pair in $pythonPairs) {
        $pythonBench = Join-Path $root $pair.Python
        $suraBench = Join-Path $root $pair.Sura
        if (-not ((Test-Path $pythonBench) -and (Test-Path $suraBench))) { continue }
        $pythonIndex += 1
        Write-Host ("[python {0}/{1}] {2}" -f $pythonIndex, $pythonPairs.Count, $pair.Label)
        $pyOut = & $python $pythonBench 2>&1 | Out-String
        $suraOut = & $Engine --jit --bench $suraBench 2>&1 | Out-String
        $pyAvg = Extract-AvgMs $pyOut
        $suraAvg = Extract-AvgMs $suraOut
        $ratio = ""
        if ($pyAvg -and $suraAvg -and [double]$suraAvg -gt 0) {
            $ratio = "{0:N2}x" -f ([double]$pyAvg / [double]$suraAvg)
        }
        Write-Host ("  Python {0} ms, Sura JIT {1} ms, Sura faster {2}" -f $(if ($pyAvg) { $pyAvg } else { "n/a" }), $(if ($suraAvg) { $suraAvg } else { "n/a" }), $(if ($ratio) { $ratio } else { "n/a" }))
        if ($pyAvg -and $suraAvg) {
            $pythonRows.Add("<tr><td>$($pair.Label)</td><td>$pyAvg ms</td><td>$suraAvg ms</td><td>$ratio</td></tr>")
            $pythonRecords.Add([pscustomobject]@{
                label = $pair.Label
                python_file = $pair.Python
                sura_file = $pair.Sura
                python_ms = Maybe-Number $pyAvg
                sura_jit_ms = Maybe-Number $suraAvg
                sura_faster_by = if ([double]$suraAvg -gt 0) { [double]$pyAvg / [double]$suraAvg } else { $null }
            })
        }
    }
}

$pythonSection = ""
if ($pythonRows.Count -gt 0) {
    $pythonChart = New-BarChartSvg -Items $pythonRecords -LabelField "label" -ValueField "sura_faster_by" -Unit "x" -Color "#16a34a"
    $pythonSection = @"
<h2>Python Comparison</h2>
$pythonChart
<table>
<tr><th>Benchmark</th><th>Python</th><th>Sura JIT</th><th>Sura Faster By</th></tr>
$($pythonRows -join "`n")
</table>
"@
}

$nativePerfRecord = $null
$nativePerfCandidates = if ($NativePerfIn) { @($NativePerfIn) } else { @("artifacts\native_perf.json", "native_perf.json") }
foreach ($candidate in $nativePerfCandidates) {
    $nativePath = Project-Path $candidate
    if (-not (Test-Path -LiteralPath $nativePath)) { continue }
    $nativeReport = Read-NativePerfReport $nativePath
    if ($null -eq $nativeReport) { continue }
    $rawBaselines = @(As-Array $nativeReport.baselines)
    if ($rawBaselines.Count -eq 0) { $rawBaselines = @($nativeReport) }
    $baselineRecords = New-Object System.Collections.Generic.List[object]
    foreach ($baseline in $rawBaselines) {
        $baselineId = [string]$baseline.id
        if ([string]::IsNullOrWhiteSpace($baselineId)) { $baselineId = "vec2" }
        $baselineRecords.Add([pscustomobject]@{
            id = $baselineId
            benchmark = [string]$baseline.benchmark
            sura_file = [string]$baseline.sura_file
            native_file = [string]$baseline.native_file
            dimension = [string]$baseline.dimension
            measurement_scope = $baseline.measurement_scope
            fair_scope_passed = if ($null -ne $baseline.fair_scope_passed) { [bool]$baseline.fair_scope_passed } else { $false }
            sura_jit_ms = Optional-Double -Value $baseline.sura_jit_ms
            sura_jit_time_source = [string]$baseline.sura_jit_time_source
            sura_loop_scope = [string]$baseline.sura_loop_scope
            sura_jit_run_count = if ($null -ne $baseline.sura_jit_run_count) { [int]$baseline.sura_jit_run_count } else { 0 }
            native_ms = Optional-Double -Value $baseline.native_ms
            native_time_source = [string]$baseline.native_time_source
            native_loop_scope = [string]$baseline.native_loop_scope
            sura_native_ratio = Optional-Double -Value $baseline.sura_native_ratio
            native_faster_by = Optional-Double -Value $baseline.native_faster_by
            warnings = @(As-Array $baseline.warnings)
        })
    }
    $native3dBaseline = $baselineRecords | Where-Object {
        $_.id -eq "vec3" -or $_.dimension -eq "vec3" -or $_.benchmark -match "Vec3|3D"
    } | Select-Object -First 1
    $nativePerfRecord = [pscustomobject]@{
        source = $nativePath
        schema = [string]$nativeReport.schema
        passed = if ($null -ne $nativeReport.passed) { [bool]$nativeReport.passed } else { $false }
        compiler = [string]$nativeReport.compiler
        baseline_count = $baselineRecords.Count
        baselines = $baselineRecords
        benchmark = [string]$nativeReport.benchmark
        sura_file = [string]$nativeReport.sura_file
        native_file = [string]$nativeReport.native_file
        measurement_scope = $nativeReport.measurement_scope
        fair_scope_passed = if ($null -ne $nativeReport.fair_scope_passed) { [bool]$nativeReport.fair_scope_passed } else { $false }
        sura_jit_ms = Optional-Double -Value $nativeReport.sura_jit_ms
        sura_jit_time_source = [string]$nativeReport.sura_jit_time_source
        sura_loop_scope = [string]$nativeReport.sura_loop_scope
        sura_jit_run_count = if ($null -ne $nativeReport.sura_jit_run_count) { [int]$nativeReport.sura_jit_run_count } else { 0 }
        native_ms = Optional-Double -Value $nativeReport.native_ms
        native_time_source = [string]$nativeReport.native_time_source
        native_loop_scope = [string]$nativeReport.native_loop_scope
        sura_native_ratio = Optional-Double -Value $nativeReport.sura_native_ratio
        native_faster_by = Optional-Double -Value $nativeReport.native_faster_by
        native_3d_available = ($null -ne $native3dBaseline)
        native_3d_sura_native_ratio = if ($native3dBaseline) { Optional-Double -Value $native3dBaseline.sura_native_ratio } else { $null }
        native_3d_sura_jit_ms = if ($native3dBaseline) { Optional-Double -Value $native3dBaseline.sura_jit_ms } else { $null }
        native_3d_ms = if ($native3dBaseline) { Optional-Double -Value $native3dBaseline.native_ms } else { $null }
        warnings = @(As-Array $nativeReport.warnings)
    }
    break
}

$generatedUtc = [DateTime]::UtcNow.ToString("o")
$avgJitSpeedup = Average-Value $benchRecords "speedup"
$avgPythonRatio = Average-Value $pythonRecords "sura_faster_by"
$fasterThanPythonCount = 0
$bestPythonComparison = $null
foreach ($record in (As-Array $pythonRecords)) {
    if ($null -ne $record.sura_faster_by) {
        if ([double]$record.sura_faster_by -gt 1.0) { $fasterThanPythonCount += 1 }
        if ($null -eq $bestPythonComparison -or [double]$record.sura_faster_by -gt [double]$bestPythonComparison.sura_faster_by) {
            $bestPythonComparison = $record
        }
    }
}
$summary = [pscustomobject]@{
    benchmark_count = (As-Array $benchRecords).Count
    python_comparison_count = (As-Array $pythonRecords).Count
    faster_than_python_count = $fasterThanPythonCount
    average_jit_speedup = $avgJitSpeedup
    average_sura_python_ratio = $avgPythonRatio
    best_python_comparison = $bestPythonComparison
    native_performance_available = ($null -ne $nativePerfRecord)
    native_performance = $nativePerfRecord
}
$topPythonComparisons = @((As-Array $pythonRecords) |
    Where-Object { $null -ne $_.sura_faster_by } |
    Sort-Object -Property sura_faster_by -Descending |
    Select-Object -First 5)
$releaseNoteHighlights = New-Object System.Collections.Generic.List[object]
foreach ($record in $topPythonComparisons) {
    $releaseNoteHighlights.Add([pscustomobject]@{
        label = $record.label
        sura_faster_by = $record.sura_faster_by
        python_ms = $record.python_ms
        sura_jit_ms = $record.sura_jit_ms
        sura_file = $record.sura_file
        python_file = $record.python_file
    })
}
$releaseNotesEvidence = [pscustomobject]@{
    headline = "$($summary.benchmark_count) benchmark cases, $($summary.python_comparison_count) Python comparisons, $($summary.faster_than_python_count) faster-than-Python cases"
    average_jit_speedup = $summary.average_jit_speedup
    average_sura_python_ratio = $summary.average_sura_python_ratio
    best_python_comparison = $summary.best_python_comparison
    native_performance = $nativePerfRecord
    highlights = $releaseNoteHighlights
}
$report = [pscustomobject]@{
    # Bump this whenever benchmark semantics or the optimizer contract changes.
    # Regression gates compare only reports produced under the same contract.
    benchmark_contract = "sura.semantic-jit.v3"
    generated_utc = $generatedUtc
    engine = $Engine
    python = $python
    summary = $summary
    release_notes = $releaseNotesEvidence
    native_performance = $nativePerfRecord
    benchmarks = $benchRecords
    python_comparisons = $pythonRecords
}

$historyEntries = @()
$historySource = if ($HistoryIn) { Project-Path $HistoryIn } elseif ($HistoryOut) { Project-Path $HistoryOut } else { "" }
if ($historySource) {
    $historyEntries = @(Read-HistoryEntries $historySource)
}
if ($HistoryOut) {
    $historyEntries += $report
    if ($HistoryLimit -gt 0 -and $historyEntries.Count -gt $HistoryLimit) {
        $historyEntries = @($historyEntries | Select-Object -Last $HistoryLimit)
    }
}

$historySection = ""
if ($historyEntries.Count -gt 0) {
    $historyRows = New-Object System.Collections.Generic.List[string]
    foreach ($entry in @($historyEntries | Select-Object -Last 12)) {
        $avgJitSpeedup = Average-Value $entry.benchmarks "speedup"
        $avgPythonRatio = Average-Value $entry.python_comparisons "sura_faster_by"
        $benchCount = (As-Array $entry.benchmarks).Count
        $pythonCount = (As-Array $entry.python_comparisons).Count
        $avgJitText = if ($null -ne $avgJitSpeedup) { "{0:N2}x" -f $avgJitSpeedup } else { "" }
        $avgPythonText = if ($null -ne $avgPythonRatio) { "{0:N2}x" -f $avgPythonRatio } else { "" }
        $historyRows.Add("<tr><td>$($entry.generated_utc)</td><td>$benchCount</td><td>$pythonCount</td><td>$avgJitText</td><td>$avgPythonText</td></tr>")
    }
    $historySection = @"
<h2>Recent Benchmark History</h2>
$(New-HistoryChartSvg $historyEntries)
<table>
<tr><th>Generated UTC</th><th>Sura Benches</th><th>Python Comparisons</th><th>Avg JIT Speedup</th><th>Avg Sura/Python Ratio</th></tr>
$($historyRows -join "`n")
</table>
"@
}

$bestPythonText = if ($summary.best_python_comparison) {
    (Html-Escape $summary.best_python_comparison.label) + " (" + (Format-Ratio $summary.best_python_comparison.sura_faster_by) + ")"
} else {
    "n/a"
}
$bestPythonMarkdownText = if ($summary.best_python_comparison) {
    (Markdown-Escape $summary.best_python_comparison.label) + " (" + (Format-Ratio $summary.best_python_comparison.sura_faster_by) + ")"
} else {
    "n/a"
}
$nativeSummaryRow = ""
if ($nativePerfRecord) {
    $nativeSummaryRow = "<tr><td>Native C++ comparison</td><td>Sura/native $(Format-Ratio $nativePerfRecord.sura_native_ratio)</td></tr>"
    if ($nativePerfRecord.native_3d_available) {
        $nativeSummaryRow += "`n<tr><td>Native C++ 3D comparison</td><td>Sura/native $(Format-Ratio $nativePerfRecord.native_3d_sura_native_ratio)</td></tr>"
    }
}
$summarySection = @"
<h2>Benchmark Summary</h2>
<table>
<tr><th>Metric</th><th>Value</th></tr>
<tr><td>Benchmarks</td><td>$($summary.benchmark_count)</td></tr>
<tr><td>Average JIT speedup</td><td>$(Format-Ratio $summary.average_jit_speedup)</td></tr>
<tr><td>Python comparison cases</td><td>$($summary.python_comparison_count)</td></tr>
<tr><td>Cases faster than Python</td><td>$($summary.faster_than_python_count)</td></tr>
<tr><td>Average Sura/Python ratio</td><td>$(Format-Ratio $summary.average_sura_python_ratio)</td></tr>
<tr><td>Best Python comparison</td><td>$bestPythonText</td></tr>
$nativeSummaryRow
</table>
"@

$nativeSection = ""
if ($nativePerfRecord) {
    $nativeStatus = if ($nativePerfRecord.passed) { "PASS" } else { "WARN" }
    $nativeBaselineRows = New-Object System.Collections.Generic.List[string]
    foreach ($baseline in (As-Array $nativePerfRecord.baselines)) {
        $baselineScope = $baseline.measurement_scope
        $baselineTimedRegion = if ($baselineScope) { [string]$baselineScope.timed_region } else { "" }
        $baselineSteps = if ($baselineScope) { [string]$baselineScope.steps } else { "" }
        $baselineRuns = if ($baselineScope) { "Sura $($baselineScope.sura_runs), C++ $($baselineScope.native_runs)" } else { "" }
        $baselineWarnings = if ((As-Array $baseline.warnings).Count -gt 0) {
            Html-Escape ((As-Array $baseline.warnings) -join "; ")
        } else {
            "none"
        }
        $nativeBaselineRows.Add("<tr><td>$(Html-Escape $baseline.benchmark)</td><td>$(Html-Escape $baselineTimedRegion)</td><td>$(Html-Escape $baselineSteps)</td><td>$(Html-Escape $baselineRuns)</td><td>$($baseline.fair_scope_passed)</td><td>$(Format-Ms $baseline.sura_jit_ms)</td><td>$(Format-Ms $baseline.native_ms)</td><td>$(Format-Ratio $baseline.sura_native_ratio)</td><td>$(Html-Escape ($baseline.sura_jit_time_source + ' / ' + $baseline.native_time_source))</td><td>$(Html-Escape ($baseline.sura_file + ' vs ' + $baseline.native_file))</td><td>$baselineWarnings</td></tr>")
    }
    $nativeSection = @"
<h2>Native C++ Baseline</h2>
<p>Status: $nativeStatus</p>
<table>
<tr><th>Benchmark</th><th>Timed region</th><th>Steps</th><th>Runs</th><th>Fair scope</th><th>Sura JIT loop avg</th><th>Native C++ avg</th><th>Sura/native ratio</th><th>Timing source</th><th>Evidence</th><th>Warnings</th></tr>
$($nativeBaselineRows -join "`n")
</table>
"@
}

$html = @"
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sura Benchmark Dashboard</title>
<style>
:root{--bg:#f6f8fb;--panel:#fff;--panel2:#f9fbfd;--border:#d8e1ea;--text:#182230;--muted:#5d6b7a;--accent:#0f5d91;--accent2:#167a48;--shadow:0 10px 28px rgba(15,23,42,.08)}
*{box-sizing:border-box}
body{font-family:Segoe UI,Arial,sans-serif;margin:0;line-height:1.45;color:var(--text);background:var(--bg)}
.page{width:min(1180px,calc(100% - 40px));margin:0 auto;padding:34px 0 46px}
.dashboard-header{background:var(--panel);border:1px solid var(--border);border-radius:8px;box-shadow:var(--shadow);padding:26px 30px;margin-bottom:18px}
.eyebrow{color:var(--accent);font-size:12px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;margin:0 0 8px}
h1{font-size:32px;line-height:1.15;margin:0 0 8px}
.lede{color:var(--muted);max-width:780px;margin:0 0 16px}
.meta-row{display:flex;flex-wrap:wrap;gap:8px}
.pill{border:1px solid var(--border);background:var(--panel2);border-radius:999px;color:#344054;font-size:12px;font-weight:600;padding:5px 10px}
.section{background:var(--panel);border:1px solid var(--border);border-radius:8px;box-shadow:var(--shadow);padding:22px 24px;margin:18px 0;overflow-x:auto}
h2{font-size:20px;margin:0 0 14px;border-bottom:1px solid var(--border);padding-bottom:10px}
p{color:var(--muted)}
table{border-collapse:collapse;min-width:640px;width:100%;font-size:13px}
td,th{border-bottom:1px solid var(--border);padding:9px 12px;text-align:left;vertical-align:top}
th{background:#edf3f8;color:#475467;font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.04em}
tr:last-child td{border-bottom:0}
.chart{display:block;width:min(100%,920px);height:auto;margin:12px 0 24px}
.chart-label{font-size:12px;fill:#344054}
.chart-value{font-size:12px;fill:#475467}
.empty{color:#71717a;font-style:italic}
@media(max-width:760px){.page{width:min(100% - 24px,1180px);padding-top:20px}.dashboard-header,.section{padding:18px 16px}h1{font-size:26px}table{font-size:12px}}
</style>
</head>
<body>
<main class="page">
<header class="dashboard-header">
<p class="eyebrow">Official benchmark evidence</p>
<h1>Sura Benchmark Dashboard</h1>
<p class="lede">Release-facing benchmark evidence for interpreter, JIT, Python comparison, and fair-scope native C++ baselines. Native rows report the same measured loop scope so C++ comparisons stay honest.</p>
<div class="meta-row">
<span class="pill">Generated UTC: $generatedUtc</span>
<span class="pill">Fair native baseline</span>
<span class="pill">Python comparison evidence</span>
<span class="pill">CI-ready JSON + Markdown</span>
</div>
</header>
<section class="section">
$summarySection
</section>
<section class="section">
$nativeSection
</section>
<section class="section">
<h2>JIT Speedup Chart</h2>
$(New-BarChartSvg -Items $benchRecords -LabelField "benchmark" -ValueField "speedup" -Unit "x" -Color "#2563eb")
<table>
<tr><th>Benchmark</th><th>Interpreter Execute</th><th>JIT Execute</th><th>Speedup</th></tr>
$($rows -join "`n")
</table>
</section>
<section class="section">
$pythonSection
</section>
<section class="section">
$historySection
</section>
</main>
</body>
</html>
"@

$htmlPath = Project-Path $Out
Ensure-ParentDirectory $htmlPath
Set-Content -LiteralPath $htmlPath -Value $html -Encoding UTF8
Write-Host "[OK] wrote $Out"

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
if ($JsonOut) {
    $jsonPath = Project-Path $JsonOut
    Ensure-ParentDirectory $jsonPath
    [System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 6), $utf8NoBom)
    Write-Host "[OK] wrote $JsonOut"
}

if ($SummaryOut) {
    $summaryPath = Project-Path $SummaryOut
    Ensure-ParentDirectory $summaryPath
    $summaryLines = New-Object System.Collections.Generic.List[string]
    $pythonName = if ($python) { $python } else { "not available" }
    $summaryLines.Add("# Sura Benchmark Summary")
    $summaryLines.Add("")
    $summaryLines.Add("- Generated UTC: " + (Markdown-Escape $generatedUtc))
    $summaryLines.Add("- Engine: " + (Markdown-Escape $Engine))
    $summaryLines.Add("- Python: " + (Markdown-Escape $pythonName))
    $summaryLines.Add("")
    $summaryLines.Add("## Summary")
    $summaryLines.Add("")
    $summaryLines.Add("| Metric | Value |")
    $summaryLines.Add("| --- | ---: |")
    $summaryLines.Add("| Benchmarks | $($summary.benchmark_count) |")
    $summaryLines.Add("| Average JIT speedup | $(Format-Ratio $summary.average_jit_speedup) |")
    $summaryLines.Add("| Python comparison cases | $($summary.python_comparison_count) |")
    $summaryLines.Add("| Cases faster than Python | $($summary.faster_than_python_count) |")
    $summaryLines.Add("| Average Sura/Python ratio | $(Format-Ratio $summary.average_sura_python_ratio) |")
    $summaryLines.Add("| Best Python comparison | $bestPythonMarkdownText |")
    if ($nativePerfRecord) {
        $summaryLines.Add("| Native C++ comparison | Sura/native $(Format-Ratio $nativePerfRecord.sura_native_ratio) |")
        if ($nativePerfRecord.native_3d_available) {
            $summaryLines.Add("| Native C++ 3D comparison | Sura/native $(Format-Ratio $nativePerfRecord.native_3d_sura_native_ratio) |")
        }
    }
    if ($nativePerfRecord) {
        $summaryLines.Add("")
        $summaryLines.Add("## Native C++ Baseline")
        $summaryLines.Add("")
        $summaryLines.Add("| Benchmark | Timed region | Steps | Sura JIT ms | Native C++ ms | Sura/native ratio | Fair scope | Evidence |")
        $summaryLines.Add("| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |")
        foreach ($baseline in (As-Array $nativePerfRecord.baselines)) {
            $nativeScope = $baseline.measurement_scope
            $nativeScopeText = if ($nativeScope) { "$(Markdown-Escape ([string]$nativeScope.timed_region))" } else { "n/a" }
            $nativeSteps = if ($nativeScope) { $nativeScope.steps } else { "" }
            $summaryLines.Add("| $(Markdown-Escape $baseline.benchmark) | $nativeScopeText | $nativeSteps | $($baseline.sura_jit_ms) | $($baseline.native_ms) | $(Format-Ratio $baseline.sura_native_ratio) | $($baseline.fair_scope_passed) | $(Markdown-Escape $baseline.sura_file) vs $(Markdown-Escape $baseline.native_file) |")
        }
    }
    $summaryLines.Add("")
    $summaryLines.Add("## JIT Benchmarks")
    $summaryLines.Add("")
    $summaryLines.Add("| Benchmark | Interpreter ms | JIT ms | Speedup |")
    $summaryLines.Add("| --- | ---: | ---: | ---: |")
    foreach ($record in (As-Array $benchRecords)) {
        $summaryLines.Add("| $(Markdown-Escape $record.benchmark) | $($record.interpreter_ms) | $($record.jit_ms) | $(Format-Ratio $record.speedup) |")
    }
    $summaryLines.Add("")
    $summaryLines.Add("## Python Comparison")
    $summaryLines.Add("")
    if ((As-Array $pythonRecords).Count -eq 0) {
        $summaryLines.Add("Python was not available or no comparison cases ran.")
    } else {
        $summaryLines.Add("| Case | Python ms | Sura JIT ms | Sura faster by |")
        $summaryLines.Add("| --- | ---: | ---: | ---: |")
        foreach ($record in (As-Array $pythonRecords)) {
            $summaryLines.Add("| $(Markdown-Escape $record.label) | $($record.python_ms) | $($record.sura_jit_ms) | $(Format-Ratio $record.sura_faster_by) |")
        }
    }
    [System.IO.File]::WriteAllText($summaryPath, ($summaryLines -join "`n") + "`n", $utf8NoBom)
    Write-Host "[OK] wrote $SummaryOut"
}

if ($ReleaseNotesOut) {
    $releaseNotesPath = Project-Path $ReleaseNotesOut
    Ensure-ParentDirectory $releaseNotesPath
    $releaseLines = New-Object System.Collections.Generic.List[string]
    $pythonName = if ($python) { $python } else { "not available" }
    $releaseLines.Add("# Sura Benchmark Release Notes")
    $releaseLines.Add("")
    $releaseLines.Add("## Evidence")
    $releaseLines.Add("")
    $releaseLines.Add("- Generated UTC: " + (Markdown-Escape $generatedUtc))
    $releaseLines.Add("- Engine: " + (Markdown-Escape $Engine))
    $releaseLines.Add("- Python: " + (Markdown-Escape $pythonName))
    $releaseLines.Add("- Benchmark cases: $($summary.benchmark_count)")
    $releaseLines.Add("- Average JIT speedup: $(Format-Ratio $summary.average_jit_speedup)")
    $releaseLines.Add("- Python comparison cases: $($summary.python_comparison_count)")
    $releaseLines.Add("- Faster-than-Python cases: $($summary.faster_than_python_count)")
    $releaseLines.Add("- Average Sura/Python ratio: $(Format-Ratio $summary.average_sura_python_ratio)")
    $releaseLines.Add("- Best Python comparison: $bestPythonMarkdownText")
    if ($nativePerfRecord) {
        $releaseLines.Add("- Native C++ baseline: Sura/native $(Format-Ratio $nativePerfRecord.sura_native_ratio) ($(Format-Ms $nativePerfRecord.sura_jit_ms) Sura JIT vs $(Format-Ms $nativePerfRecord.native_ms) native)")
        if ($nativePerfRecord.native_3d_available) {
            $releaseLines.Add("- Native C++ 3D baseline: Sura/native $(Format-Ratio $nativePerfRecord.native_3d_sura_native_ratio) ($(Format-Ms $nativePerfRecord.native_3d_sura_jit_ms) Sura JIT vs $(Format-Ms $nativePerfRecord.native_3d_ms) native)")
        }
    }
    if ($nativePerfRecord) {
        $releaseLines.Add("")
        $releaseLines.Add("## Native C++ Baseline")
        $releaseLines.Add("")
        $releaseLines.Add("| Benchmark | Scope | Sura JIT ms | Native C++ ms | Sura/native ratio | Evidence |")
        $releaseLines.Add("| --- | --- | ---: | ---: | ---: | --- |")
        foreach ($baseline in (As-Array $nativePerfRecord.baselines)) {
            $nativeScope = $baseline.measurement_scope
            $nativeScopeText = if ($nativeScope) { "$(Markdown-Escape ([string]$nativeScope.timed_region)), $($nativeScope.steps) steps" } else { "n/a" }
            $releaseLines.Add("| $(Markdown-Escape $baseline.benchmark) | $nativeScopeText | $($baseline.sura_jit_ms) | $($baseline.native_ms) | $(Format-Ratio $baseline.sura_native_ratio) | $(Markdown-Escape $baseline.sura_file) vs $(Markdown-Escape $baseline.native_file) |")
        }
    }
    $releaseLines.Add("")
    $releaseLines.Add("## Python Comparison Highlights")
    $releaseLines.Add("")
    if ((As-Array $releaseNoteHighlights).Count -eq 0) {
        $releaseLines.Add("No Python comparison highlights were available for this run.")
    } else {
        $releaseLines.Add("| Case | Sura faster by | Python ms | Sura JIT ms | Evidence |")
        $releaseLines.Add("| --- | ---: | ---: | ---: | --- |")
        foreach ($record in (As-Array $releaseNoteHighlights)) {
            $evidence = "$(Markdown-Escape $record.sura_file) vs $(Markdown-Escape $record.python_file)"
            $releaseLines.Add("| $(Markdown-Escape $record.label) | $(Format-Ratio $record.sura_faster_by) | $($record.python_ms) | $($record.sura_jit_ms) | $evidence |")
        }
    }
    $releaseLines.Add("")
    $releaseLines.Add("## Coverage")
    $releaseLines.Add("")
    $nativeCoverage = if ($nativePerfRecord) { " plus native C++ baseline evidence" } else { "" }
    $releaseLines.Add("Dashboard JSON includes $($summary.benchmark_count) benchmark records, $($summary.python_comparison_count) Python comparison records$nativeCoverage for release evidence.")
    [System.IO.File]::WriteAllText($releaseNotesPath, ($releaseLines -join "`n") + "`n", $utf8NoBom)
    Write-Host "[OK] wrote $ReleaseNotesOut"
}

if ($HistoryOut) {
    $historyPath = Project-Path $HistoryOut
    Ensure-ParentDirectory $historyPath
    $historyReport = [pscustomobject]@{
        updated_utc = $generatedUtc
        limit = $HistoryLimit
        entries = $historyEntries
    }
    [System.IO.File]::WriteAllText($historyPath, ($historyReport | ConvertTo-Json -Depth 8), $utf8NoBom)
    Write-Host "[OK] wrote $HistoryOut"
}
