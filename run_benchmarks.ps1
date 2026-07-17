$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$engine = Join-Path $root "SuraLanguage.exe"

if (-not (Test-Path $engine)) {
    Write-Error "SuraLanguage.exe not found. Run build.bat first."
    exit 1
}

$benchmarks = @(
    "bench_jit.sura",
    "bench_fib.sura",
    "bench_agent_scoring.sura",
    "bench_api_log_etl.sura",
    "bench_tool_routing.sura",
    "bench_physics.sura",
    "bench_physics_inplace.sura",
    "bench_physics3d.sura",
    "bench_market.sura"
)

foreach ($bench in $benchmarks) {
    $path = Join-Path $root $bench
    if (-not (Test-Path $path)) {
        Write-Error "Benchmark file not found: $path"
        exit 1
    }
    Write-Host ""
    Write-Host "== $bench / interpreter =="
    & $engine --bench $path
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host ""
    Write-Host "== $bench / native JIT =="
    & $engine --jit --bench $path
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
