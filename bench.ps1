param(
    [string]$Engine = (Join-Path $PSScriptRoot "SuraLanguage.exe"),
    [string]$ArtifactsDir = "artifacts",
    [int]$SuraRuns = 5,
    [double]$MaxRegressionPercent = 35.0,
    [switch]$SkipNative,
    [switch]$SkipGate,
    [switch]$NoOpen
)

$benchNow = Join-Path $PSScriptRoot "tools\sura_bench_now.ps1"
$params = @{
    Engine = $Engine
    ArtifactsDir = $ArtifactsDir
    SuraRuns = $SuraRuns
    MaxRegressionPercent = $MaxRegressionPercent
}
if ($SkipNative) { $params["SkipNative"] = $true }
if ($SkipGate) { $params["SkipGate"] = $true }
if (-not $NoOpen) { $params["OpenDashboard"] = $true }

& $benchNow @params
