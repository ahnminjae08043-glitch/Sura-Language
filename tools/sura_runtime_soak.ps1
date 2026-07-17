param(
    [string]$Engine = "",
    [ValidateRange(1, 86400)][int]$DurationSeconds = 300,
    [ValidateRange(1, 600)][int]$PerRunTimeoutSeconds = 30,
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
. (Join-Path $PSScriptRoot "sura_test_process.ps1")

if ([string]::IsNullOrWhiteSpace($Engine)) {
    $Engine = if ($env:OS -eq "Windows_NT") { Join-Path $root "SuraLanguage.exe" } else { Join-Path $root "SuraLanguage" }
}

$cases = @(
    [pscustomobject]@{ Name = "allocation-vm"; Script = "tests/runtime_soak_cycle.sura"; Jit = $false; GcStats = $true; Marker = "runtime_soak_cycle: PASS" },
    [pscustomobject]@{ Name = "allocation-jit"; Script = "tests/runtime_soak_cycle.sura"; Jit = $true; GcStats = $true; Marker = "runtime_soak_cycle: PASS" },
    [pscustomobject]@{ Name = "invalid-ops-jit"; Script = "tests/runtime_safety_invalid_ops.sura"; Jit = $true; GcStats = $false; Marker = "runtime_safety_invalid_ops: PASS" },
    [pscustomobject]@{ Name = "exception-upvalue-jit"; Script = "tests/65_exception_upvalue_safety.sura"; Jit = $true; GcStats = $false; Marker = "65_exception_upvalue_safety: PASS" },
    [pscustomobject]@{ Name = "dynamic-throw-jit"; Script = "tests/jit_dynamic_throw_safety.sura"; Jit = $true; GcStats = $false; Marker = "jit_dynamic_throw_safety: PASS" },
    [pscustomobject]@{ Name = "async-structured-jit"; Script = "tests/async_structured_stress.sura"; Jit = $true; GcStats = $false; Marker = "structured async stress: PASS" },
    [pscustomobject]@{ Name = "bpe-vm"; Script = "tests/70_bpe_tokenizer.sura"; Jit = $false; GcStats = $false; Marker = "70_bpe_tokenizer: PASS" },
    [pscustomobject]@{ Name = "onnx-vm"; Script = "tests/71_onnx_execution.sura"; Jit = $false; GcStats = $false; Marker = "71_onnx_execution: PASS" }
)

$caseTotals = @{}
foreach ($case in $cases) {
    $caseTotals[$case.Name] = [ordered]@{
        name = $case.Name
        script = $case.Script
        mode = $(if ($case.Jit) { "jit" } else { "vm" })
        runs = 0
        total_duration_ms = [int64]0
        max_duration_ms = [int64]0
        max_working_set_bytes = [int64]0
        gc_collections = [int64]0
        gc_objects_reclaimed = [int64]0
    }
}

$snapshot = $null
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_runtime_soak_" + [System.Guid]::NewGuid().ToString("N"))
$startedUtc = [DateTime]::UtcNow
$timer = [System.Diagnostics.Stopwatch]::StartNew()
$failures = New-Object System.Collections.Generic.List[object]
$completedCycles = 0
$totalRuns = 0
$version = "unknown"

function Add-SoakFailure {
    param([string]$CaseName, [int]$Cycle, [string]$Message, [string]$Output = "")
    if ($Output.Length -gt 8192) { $Output = $Output.Substring(0, 8192) }
    $failures.Add([pscustomobject]@{
        case = $CaseName
        cycle = $Cycle
        message = $Message
        output = $Output
    })
}

try {
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    $snapshot = New-SuraTestEngineSnapshot -EnginePath $Engine

    $versionRun = Invoke-SuraTestProcess -EnginePath $snapshot.Path -Arguments @("--version") -TimeoutSeconds $PerRunTimeoutSeconds -WorkingDirectory $root
    if ($versionRun.ExitCode -ne 0 -or $versionRun.TimedOut) {
        throw "unable to read the Sura engine version: $($versionRun.Output)"
    }
    if (-not [string]::IsNullOrWhiteSpace($versionRun.Output)) {
        $version = ($versionRun.Output -split "`r?`n")[0].Trim()
    }

    do {
        $cycle = $completedCycles + 1
        foreach ($case in $cases) {
            $scriptPath = Join-Path $root $case.Script
            if (-not (Test-Path -LiteralPath $scriptPath -PathType Leaf)) {
                Add-SoakFailure -CaseName $case.Name -Cycle $cycle -Message "test script not found: $($case.Script)"
                continue
            }

            $arguments = @()
            $gcPath = ""
            if ($case.GcStats) {
                $gcPath = Join-Path $tempRoot ("gc-{0}-{1}.json" -f $case.Name, [System.Guid]::NewGuid().ToString("N"))
                $arguments += @("--gc-stats-json", $gcPath)
            }
            if ($case.Jit) { $arguments += "--jit" }
            $arguments += $scriptPath

            $run = Invoke-SuraTestProcess -EnginePath $snapshot.Path -Arguments $arguments -TimeoutSeconds $PerRunTimeoutSeconds -WorkingDirectory $root
            $totalRuns++
            $totals = $caseTotals[$case.Name]
            $totals.runs++
            $totals.total_duration_ms += [int64]$run.DurationMs
            if ([int64]$run.DurationMs -gt $totals.max_duration_ms) { $totals.max_duration_ms = [int64]$run.DurationMs }
            if ([int64]$run.PeakWorkingSetBytes -gt $totals.max_working_set_bytes) { $totals.max_working_set_bytes = [int64]$run.PeakWorkingSetBytes }

            if ($run.TimedOut) {
                Add-SoakFailure -CaseName $case.Name -Cycle $cycle -Message "run timed out after $PerRunTimeoutSeconds seconds" -Output $run.Output
                continue
            }
            if ($run.ExitCode -ne 0) {
                Add-SoakFailure -CaseName $case.Name -Cycle $cycle -Message "run exited with code $($run.ExitCode)" -Output $run.Output
                continue
            }
            if (-not $run.Output.Contains([string]$case.Marker)) {
                Add-SoakFailure -CaseName $case.Name -Cycle $cycle -Message "PASS marker was not emitted: $($case.Marker)" -Output $run.Output
                continue
            }

            if ($case.GcStats) {
                if (-not (Test-Path -LiteralPath $gcPath -PathType Leaf)) {
                    Add-SoakFailure -CaseName $case.Name -Cycle $cycle -Message "GC statistics JSON was not written" -Output $run.Output
                    continue
                }
                try {
                    $gc = Get-Content -LiteralPath $gcPath -Raw -Encoding UTF8 | ConvertFrom-Json
                    if ($gc.schema -ne "sura.gc_stats.v1" -or [int64]$gc.collections -le 0 -or [int64]$gc.objects_reclaimed -le 0) {
                        throw "invalid GC statistics contract"
                    }
                    $totals.gc_collections += [int64]$gc.collections
                    $totals.gc_objects_reclaimed += [int64]$gc.objects_reclaimed
                }
                catch {
                    Add-SoakFailure -CaseName $case.Name -Cycle $cycle -Message ("unable to validate GC statistics: " + $_.Exception.Message) -Output $run.Output
                }
            }
        }

        if ($failures.Count -eq 0) { $completedCycles++ }
    } while ($failures.Count -eq 0 -and ($completedCycles -lt 1 -or $timer.Elapsed.TotalSeconds -lt $DurationSeconds))
}
catch {
    Add-SoakFailure -CaseName "runner" -Cycle ($completedCycles + 1) -Message $_.Exception.Message
}
finally {
    $timer.Stop()
    if ($null -ne $snapshot) {
        try { Remove-SuraTestEngineSnapshot -Snapshot $snapshot } catch { Add-SoakFailure -CaseName "runner" -Cycle ($completedCycles + 1) -Message $_.Exception.Message }
    }
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$caseReports = @()
foreach ($case in $cases) {
    $item = $caseTotals[$case.Name]
    $average = if ($item.runs -gt 0) { [Math]::Round($item.total_duration_ms / [double]$item.runs, 2) } else { 0.0 }
    $caseReports += [pscustomobject]@{
        name = $item.name
        script = $item.script
        mode = $item.mode
        runs = $item.runs
        total_duration_ms = $item.total_duration_ms
        average_duration_ms = $average
        max_duration_ms = $item.max_duration_ms
        max_working_set_bytes = $item.max_working_set_bytes
        gc_collections = $item.gc_collections
        gc_objects_reclaimed = $item.gc_objects_reclaimed
    }
}

$engineSourcePath = [string]$Engine
$engineSha256 = ""
$engineBytes = [int64]0
if ($null -ne $snapshot) {
    $engineSourcePath = [string]$snapshot.SourcePath
    $engineSha256 = [string]$snapshot.Sha256
    $engineBytes = [int64]($snapshot.Bytes)
}
$failureReports = @()
foreach ($failure in $failures) { $failureReports += $failure }

$report = [ordered]@{
    schema = "sura.runtime.soak.v1"
    passed = ($failures.Count -eq 0)
    started_utc = $startedUtc.ToString("yyyy-MM-ddTHH:mm:ssZ")
    ended_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    requested_duration_seconds = $DurationSeconds
    elapsed_seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3)
    completed_cycles = $completedCycles
    total_runs = $totalRuns
    engine = [ordered]@{
        source_path = $engineSourcePath
        version = $version
        sha256 = $engineSha256
        bytes = $engineBytes
    }
    totals = [ordered]@{
        max_working_set_bytes = [int64](($caseReports | Measure-Object -Property max_working_set_bytes -Maximum).Maximum)
        gc_collections = [int64](($caseReports | Measure-Object -Property gc_collections -Sum).Sum)
        gc_objects_reclaimed = [int64](($caseReports | Measure-Object -Property gc_objects_reclaimed -Sum).Sum)
    }
    cases = $caseReports
    failures = $failureReports
}

if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $jsonPath = if ([System.IO.Path]::IsPathRooted($JsonOut)) { $JsonOut } else { Join-Path $root $JsonOut }
    $jsonParent = Split-Path -Parent $jsonPath
    if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
    [System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
}

if (-not $report.passed) {
    foreach ($failure in $failures) {
        [Console]::Error.WriteLine("[runtime soak] $($failure.case), cycle $($failure.cycle): $($failure.message)")
    }
    throw "Sura runtime soak failed with $($failures.Count) failure(s)"
}

Write-Host ("sura_runtime_soak: PASS ({0} cycles, {1} runs, {2}s, {3} GC collections, {4} objects reclaimed)" -f $completedCycles, $totalRuns, $report.elapsed_seconds, $report.totals.gc_collections, $report.totals.gc_objects_reclaimed)
