param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$ArtifactsDir = "artifacts",
    [int]$SuraRuns = 5,
    [double]$MaxRegressionPercent = 35.0,
    [switch]$SkipNative,
    [switch]$SkipGate,
    [switch]$OpenDashboard
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot
$enginePath = if ([System.IO.Path]::IsPathRooted($Engine)) { $Engine } else { Join-Path $root $Engine }
$enginePath = (Resolve-Path -LiteralPath $enginePath).Path
$artifactRoot = if ([System.IO.Path]::IsPathRooted($ArtifactsDir)) { $ArtifactsDir } else { Join-Path $root $ArtifactsDir }
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

function Join-Artifact {
    param([string]$Name)
    return Join-Path $artifactRoot $Name
}

function Invoke-Step {
    param([string]$Name, [scriptblock]$Block)
    Write-Host ""
    Write-Host "== $Name =="
    & $Block
}

$nativeJson = Join-Artifact "native_perf.json"
$nativeMd = Join-Artifact "native_perf.md"
$dashboardHtml = Join-Artifact "bench_dashboard.html"
$dashboardJson = Join-Artifact "bench_dashboard.json"
$summaryMd = Join-Artifact "bench_summary.md"
$releaseNotesMd = Join-Artifact "bench_release_notes.md"
$historyJson = Join-Artifact "bench_history.json"
$historyIn = $historyJson

if (Test-Path -LiteralPath $historyJson) {
    try {
        [System.IO.File]::ReadAllText($historyJson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json | Out-Null
    } catch {
        $stamp = [DateTime]::UtcNow.ToString("yyyyMMddHHmmss")
        $backup = "$historyJson.invalid.$stamp"
        Move-Item -LiteralPath $historyJson -Destination $backup -Force
        Write-Warning "Ignored unreadable benchmark history and moved it to $backup"
        $historyIn = ""
    }
}

if (-not $SkipNative) {
    Invoke-Step "native C++ fair-scope baseline" {
        & (Join-Path $PSScriptRoot "sura_native_perf_baseline.ps1") `
            -Engine $enginePath `
            -JsonOut $nativeJson `
            -SummaryOut $nativeMd `
            -SuraRuns $SuraRuns
    }
}

Invoke-Step "benchmark dashboard" {
    $dashboardParams = @{
        Engine = $enginePath
        Out = $dashboardHtml
        JsonOut = $dashboardJson
        SummaryOut = $summaryMd
        ReleaseNotesOut = $releaseNotesMd
        HistoryOut = $historyJson
    }
    if (-not [string]::IsNullOrWhiteSpace($historyIn)) {
        $dashboardParams["HistoryIn"] = $historyIn
    }
    if (Test-Path -LiteralPath $nativeJson) {
        $dashboardParams["NativePerfIn"] = $nativeJson
    }
    & (Join-Path $PSScriptRoot "sura_bench_dashboard.ps1") @dashboardParams
}

if (-not $SkipGate) {
    Invoke-Step "benchmark regression gate" {
        & (Join-Path $PSScriptRoot "sura_bench_gate.ps1") `
            -Report $dashboardJson `
            -History $historyJson `
            -MaxRegressionPercent $MaxRegressionPercent `
            -RequiredBenchmarks "bench_fib.sura,bench_ai_schema.sura,bench_rag_vector.sura,bench_tool_routing.sura,bench_policy_gate.sura,bench_guardrail.sura,bench_dependency_resolver.sura,bench_workflow_scheduler.sura,bench_telemetry_window.sura,bench_fraud_scoring.sura,bench_feature_flags.sura,bench_physics.sura,bench_physics_inplace.sura,bench_physics3d.sura,bench_market.sura" `
            -RequiredPythonComparisons "fib(30),AI JSON/schema validation,RAG vector ranking,AI tool policy gate,AI guardrail event scoring,dependency resolver hot loop,automation workflow scheduler,telemetry rolling window,payment fraud scoring,feature flag rollout,game physics Vec2 loop,game physics in-place Vec2 loop,game physics Vec3 loop,market simulation objects"
    }
}

Write-Host ""
Write-Host "Benchmark artifacts:"
Write-Host "  HTML: $dashboardHtml"
Write-Host "  JSON: $dashboardJson"
Write-Host "  Summary: $summaryMd"
Write-Host "  Native: $nativeJson"

if ($OpenDashboard) {
    Start-Process -FilePath $dashboardHtml
}
