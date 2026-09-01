param(
    [string]$Report = "artifacts\bench_dashboard.json",
    [string]$History = "artifacts\bench_history.json",
    [double]$MaxRegressionPercent = 35.0,
    [double]$MinJitSpeedup = 0.0,
    [double]$MinPythonFasterBy = 0.0,
    [string[]]$RequiredBenchmarks = @(),
    [string[]]$RequiredPythonComparisons = @(),
    [switch]$RequireBaseline
)

$ErrorActionPreference = "Stop"

function Read-JsonFile {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "JSON file not found: $Path"
    }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
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

function Get-Field {
    param($Object, [string]$Field)
    if ($null -eq $Object) { return $null }
    $prop = $Object.PSObject.Properties[$Field]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function Build-Map {
    param($Items, [string]$Key)
    $map = @{}
    foreach ($item in (As-Array $Items)) {
        $value = Get-Field $item $Key
        if ($null -ne $value) {
            $map[[string]$value] = $item
        }
    }
    return $map
}

function Number-Or-Null {
    param($Value)
    if ($null -eq $Value) { return $null }
    try { return [double]$Value } catch { return $null }
}

function Expand-RequiredList {
    param([string[]]$Items)
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($item in (As-Array $Items)) {
        if ($null -eq $item) { continue }
        foreach ($part in ([string]$item -split ",")) {
            $trimmed = $part.Trim()
            if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
                $out.Add($trimmed)
            }
        }
    }
    return @($out)
}

function Fail-Gate {
    param([System.Collections.Generic.List[string]]$Findings)
    foreach ($finding in $Findings) {
        [Console]::Error.WriteLine("[bench gate] $finding")
    }
    throw "Benchmark regression gate failed with $($Findings.Count) finding(s)"
}

if ($MaxRegressionPercent -lt 0) {
    throw "MaxRegressionPercent must be non-negative"
}

$current = Read-JsonFile $Report
$failures = New-Object System.Collections.Generic.List[string]
$currentBenches = Build-Map $current.benchmarks "benchmark"
$currentPy = Build-Map $current.python_comparisons "label"

foreach ($required in (Expand-RequiredList $RequiredBenchmarks)) {
    if (-not $currentBenches.ContainsKey($required)) {
        $failures.Add("required benchmark missing from current report: $required")
    }
}

foreach ($required in (Expand-RequiredList $RequiredPythonComparisons)) {
    if (-not $currentPy.ContainsKey($required)) {
        $failures.Add("required Python comparison missing from current report: $required")
    }
}

foreach ($name in $currentBenches.Keys) {
    $speedup = Number-Or-Null (Get-Field $currentBenches[$name] "speedup")
    if ($MinJitSpeedup -gt 0 -and $null -ne $speedup -and $speedup -lt $MinJitSpeedup) {
        $failures.Add("$name JIT speedup {0:N2}x is below required {1:N2}x" -f $speedup, $MinJitSpeedup)
    }
}

foreach ($label in $currentPy.Keys) {
    $ratio = Number-Or-Null (Get-Field $currentPy[$label] "sura_faster_by")
    if ($MinPythonFasterBy -gt 0 -and $null -ne $ratio -and $ratio -lt $MinPythonFasterBy) {
        $failures.Add("$label Sura/Python ratio {0:N2}x is below required {1:N2}x" -f $ratio, $MinPythonFasterBy)
    }
}

$historyEntries = @()
if (Test-Path $History) {
    try {
        $historyJson = Read-JsonFile $History
        if ($historyJson.entries) { $historyEntries = As-Array $historyJson.entries }
        else { $historyEntries = @($historyJson) }
    } catch {
        Write-Warning "Ignoring unreadable benchmark history: $History"
        $historyEntries = @()
    }
}

$currentTime = [string](Get-Field $current "generated_utc")
$currentContract = [string](Get-Field $current "benchmark_contract")
$baselineCandidates = @($historyEntries | Where-Object {
    [string](Get-Field $_ "generated_utc") -ne $currentTime -and
    [string](Get-Field $_ "benchmark_contract") -eq $currentContract
})
$baseline = $baselineCandidates | Select-Object -Last 1

if (-not $baseline) {
    if ($RequireBaseline) {
        $failures.Add("No previous benchmark baseline found in $History")
    }
    if ($failures.Count -gt 0) {
        Fail-Gate $failures
    }
    $contractLabel = if ([string]::IsNullOrWhiteSpace($currentContract)) { "legacy" } else { $currentContract }
    Write-Host "[OK] no previous benchmark baseline for contract '$contractLabel'; regression gate skipped"
    exit 0
}

$baselineBenches = Build-Map $baseline.benchmarks "benchmark"

foreach ($name in $currentBenches.Keys) {
    if (-not $baselineBenches.ContainsKey($name)) { continue }
    $now = Number-Or-Null (Get-Field $currentBenches[$name] "jit_ms")
    $then = Number-Or-Null (Get-Field $baselineBenches[$name] "jit_ms")
    if ($null -ne $now -and $null -ne $then -and $then -gt 0) {
        $regression = (($now - $then) / $then) * 100.0
        if ($regression -gt $MaxRegressionPercent) {
            $failures.Add(("$name JIT regressed by {0:N1}% ({1:N3} ms -> {2:N3} ms)" -f
                $regression, $then, $now))
        }
    }
}

$baselinePy = Build-Map $baseline.python_comparisons "label"
foreach ($label in $currentPy.Keys) {
    if ($baselinePy.ContainsKey($label)) {
        $now = Number-Or-Null (Get-Field $currentPy[$label] "sura_jit_ms")
        $then = Number-Or-Null (Get-Field $baselinePy[$label] "sura_jit_ms")
        if ($null -ne $now -and $null -ne $then -and $then -gt 0) {
            $regression = (($now - $then) / $then) * 100.0
            if ($regression -gt $MaxRegressionPercent) {
                $failures.Add(("$label Sura-vs-Python JIT run regressed by {0:N1}% ({1:N3} ms -> {2:N3} ms)" -f
                    $regression, $then, $now))
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Fail-Gate $failures
}

Write-Host "[OK] benchmark regression gate passed against baseline $(Get-Field $baseline "generated_utc")"
