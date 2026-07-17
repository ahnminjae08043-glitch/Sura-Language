param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_native_perf_smoke_" + [System.Guid]::NewGuid().ToString("N"))
$gate = Join-Path $PSScriptRoot "sura_native_perf_baseline.ps1"
$powerShellExe = (Get-Command pwsh -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if (-not $powerShellExe) {
    $powerShellExe = (Get-Command powershell -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source)
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $jsonOut = Join-Path $temp "native_perf.json"
    $summaryOut = Join-Path $temp "native_perf.md"
    $out = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -Engine $Engine -JsonOut $jsonOut -SummaryOut $summaryOut -SuraRuns 2 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $out -notmatch "native_perf_baseline:\s+PASS") {
        Write-Output $out
        throw "expected native performance baseline to pass"
    }

    if (-not (Test-Path -LiteralPath $jsonOut) -or -not (Test-Path -LiteralPath $summaryOut)) {
        throw "expected native performance baseline artifacts"
    }
    $report = [System.IO.File]::ReadAllText($jsonOut, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $summary = [System.IO.File]::ReadAllText($summaryOut, [System.Text.Encoding]::UTF8)
    if ($report.schema -ne "sura.native.performance.v1" -or
        $report.passed -ne $true -or
        -not $report.compiler -or
        $report.sura_jit_ms -le 0 -or
        $report.sura_jit_time_source -ne "script_loop" -or
        $report.sura_jit_run_count -lt 1 -or
        $report.native_ms -le 0 -or
        $report.native_time_source -ne "native_loop" -or
        $report.fair_scope_passed -ne $true -or
        $report.baseline_count -lt 2 -or
        -not ($report.baselines | Where-Object { $_.id -eq "vec3" -and $_.fair_scope_passed -eq $true -and [double]$_.sura_jit_ms -gt 0 -and [double]$_.native_ms -gt 0 -and [double]$_.sura_native_ratio -gt 0 }) -or
        $report.native_3d_fair_scope_passed -ne $true -or
        $report.measurement_scope.steps -ne 100000 -or
        $report.measurement_scope.timed_region -ne "inner physics loop only" -or
        $report.native_loop_scope -ne "100k" -or
        $report.sura_loop_scope -ne "100k" -or
        $report.sura_native_ratio -le 0 -or
        $summary -notmatch "# Sura Native Performance Baseline" -or
        $summary -notmatch "3D Sura/native ratio" -or
        $summary -notmatch "Timed region" -or
        $summary -notmatch "C\+\+ flags" -or
        $summary -notmatch "Sura time source" -or
        $summary -notmatch "Sura/native ratio") {
        $report | ConvertTo-Json -Depth 6
        throw "unexpected native performance baseline report"
    }

    $badJson = Join-Path $temp "bad_native_perf.json"
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $badOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -Engine $Engine -JsonOut $badJson -SummaryOut (Join-Path $temp "bad_native_perf.md") -SuraRuns 2 -MaxSuraNativeRatio 0.001 2>&1) | Out-String
    $badCode = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($badCode -eq 0 -or $badOut -notmatch "Native performance baseline failed") {
        Write-Output $badOut
        throw "expected strict native ratio gate to fail"
    }
    $badReport = [System.IO.File]::ReadAllText($badJson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($badReport.passed -ne $false -or -not ($badReport.warnings | Where-Object { $_ -match "MaxSuraNativeRatio" })) {
        $badReport | ConvertTo-Json -Depth 6
        throw "unexpected strict native ratio failure report"
    }

    "native_perf_baseline_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
