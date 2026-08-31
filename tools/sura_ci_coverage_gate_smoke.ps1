param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$gate = Join-Path $root "tools/sura_ci_coverage_gate.ps1"
$jsonOut = Join-Path $root "artifacts/ci_coverage_gate.json"
$powerShellExe = (Get-Command pwsh -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if (-not $powerShellExe) {
    $powerShellExe = (Get-Command powershell -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source)
}

if (Test-Path -LiteralPath $jsonOut) {
    Remove-Item -LiteralPath $jsonOut -Force
}

$out = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -RepoRoot $root -JsonOut $jsonOut 2>&1) | Out-String
if ($LASTEXITCODE -ne 0 -or $out -notmatch "ci_coverage_gate:\s+PASS") {
    Write-Output $out
    throw "expected CI coverage gate to pass"
}

if (-not (Test-Path -LiteralPath $jsonOut)) {
    throw "expected CI coverage gate JSON report"
}

$report = [System.IO.File]::ReadAllText($jsonOut, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$wasmExceptionCheck = $report.checks | Where-Object { $_.name -eq "sura_wasm_exception_smoke.ps1" } | Select-Object -First 1
$wasmMemoryCheck = $report.checks | Where-Object { $_.name -eq "sura_wasm_memory_safety_smoke.ps1" } | Select-Object -First 1
if ($report.schema -ne "sura.ci.coverage_gate.v1" -or
    $report.passed -ne $true -or
    $report.failed_count -ne 0 -or
    $report.required_count -lt 70 -or
    -not ($report.categories | Where-Object { $_.name -eq "cross_platform" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "performance" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "protected_release" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "package_ecosystem" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "registry_security" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "ai_stdlib_automation" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "goal_audit" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "release_evidence" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "runtime_safety" -and $_.status -eq "PASS" }) -or
    -not ($report.categories | Where-Object { $_.name -eq "developer_tools" -and $_.status -eq "PASS" }) -or
    $null -eq $wasmExceptionCheck -or $wasmExceptionCheck.passed -ne $true -or
    -not ($wasmExceptionCheck.referenced_in -contains "bench") -or -not ($wasmExceptionCheck.referenced_in -contains "cross") -or
    $null -eq $wasmMemoryCheck -or $wasmMemoryCheck.passed -ne $true -or
    -not ($wasmMemoryCheck.referenced_in -contains "bench") -or -not ($wasmMemoryCheck.referenced_in -contains "cross") -or
    -not ($report.checks | Where-Object { $_.name -eq "benchmark WASM exception runtime execution" -and $_.passed -eq $true }) -or
    -not ($report.checks | Where-Object { $_.name -eq "cross-platform WASM exception runtime execution" -and $_.passed -eq $true }) -or
    -not ($report.checks | Where-Object { $_.name -eq "benchmark WASM memory safety execution" -and $_.passed -eq $true }) -or
    -not ($report.checks | Where-Object { $_.name -eq "cross-platform WASM memory safety execution" -and $_.passed -eq $true })) {
    $report | ConvertTo-Json -Depth 8
    throw "unexpected CI coverage gate report"
}

$brokenRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_ci_coverage_broken_" + [System.Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path (Join-Path $brokenRoot ".github/workflows") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $brokenRoot "tools") | Out-Null
    Copy-Item -LiteralPath (Join-Path $root ".github/workflows/bench-dashboard.yml") -Destination (Join-Path $brokenRoot ".github/workflows/bench-dashboard.yml")
    Copy-Item -LiteralPath (Join-Path $root ".github/workflows/cross-platform-smoke.yml") -Destination (Join-Path $brokenRoot ".github/workflows/cross-platform-smoke.yml")
    Copy-Item -LiteralPath (Join-Path $root ".github/workflows/runtime-soak.yml") -Destination (Join-Path $brokenRoot ".github/workflows/runtime-soak.yml")
    Copy-Item -LiteralPath $gate -Destination (Join-Path $brokenRoot "tools/sura_ci_coverage_gate.ps1")

    $brokenJson = Join-Path $brokenRoot "coverage.json"
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $brokenOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $brokenRoot "tools/sura_ci_coverage_gate.ps1") -RepoRoot $brokenRoot -JsonOut $brokenJson 2>&1) | Out-String
    $brokenCode = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($brokenCode -eq 0 -or $brokenOut -notmatch "CI coverage gate failed") {
        Write-Output $brokenOut
        throw "expected CI coverage gate to fail for missing required scripts"
    }
    $brokenReport = [System.IO.File]::ReadAllText($brokenJson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($brokenReport.passed -ne $false -or
        $brokenReport.failed_count -lt 10 -or
        -not ($brokenReport.next_actions | Where-Object { $_ -match "sura_stdlib_modules_smoke" }) -or
        -not ($brokenReport.next_actions | Where-Object { $_ -match "sura_wasm_exception_smoke" }) -or
        -not ($brokenReport.next_actions | Where-Object { $_ -match "sura_wasm_memory_safety_smoke" })) {
        $brokenReport | ConvertTo-Json -Depth 8
        throw "unexpected failing CI coverage report"
    }
}
finally {
    if (Test-Path -LiteralPath $brokenRoot) {
        Remove-Item -LiteralPath $brokenRoot -Recurse -Force
    }
}

"ci_coverage_gate_smoke: PASS"
# This gate printed PASS while exiting nonzero: its last native command
# was a negative check that correctly failed, and the script inherited
# that code. CI reads the exit code, so a passing gate reported failure.
exit 0
