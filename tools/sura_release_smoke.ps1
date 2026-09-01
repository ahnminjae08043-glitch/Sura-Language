param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_release_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $Text = $Text -replace "`r`n", "`n"
    if (-not $Text.EndsWith("`n")) { $Text += "`n" }
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

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $null

    $good = Join-Path $temp "release_pkg"
    Write-Text (Join-Path $good "sura.pkg.json") @"
{
  "name": "release_pkg",
  "version": "1.0.0",
  "main": "src/release_pkg.sura",
  "bench": true,
  "bench_min_speedup": "0.001",
  "bench_report": "artifacts/release-bench.json",
  "protect_report": "artifacts/release-protect.json",
  "protect_verify_report": "artifacts/release-protect-verify.json",
  "protect_require_closed_source": true,
  "protect_require_key": true,
  "protect_require_license": true,
  "protect_require_expires": true,
  "protect_require_target": "package",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $good "src/release_pkg.sura") @"
func add do
  return 2 + 3
end
"@
    Write-Text (Join-Path $good "tests/release_pkg_test.sura") @"
assert(2 + 3 == 5)
"@
    $keyFile = Join-Path $temp "release.key"
    $licenseFile = Join-Path $temp "release.license"
    Write-Text $keyFile "release-key"
    Write-Text $licenseFile "release-license"
    $protect = Run-Pkg -PkgArgs @(
        "protect", $good,
        "--out", (Join-Path $good "dist/release_pkg.sura.srp"),
        "--closed-source",
        "--key-file", $keyFile,
        "--license-file", $licenseFile,
        "--expires", "2999-12-31",
        "--json", (Join-Path $good "artifacts/release-protect.json")
    )
    if ($protect.Code -ne 0) {
        Write-Output $protect.Output
        throw "expected protect setup for release smoke to pass"
    }

    $dryRunReportPath = Join-Path $temp "release-dry-run-report.json"
    $dryRun = Run-Pkg -PkgArgs @("release", $good, "--dry-run", "--json", $dryRunReportPath)
    if ($dryRun.Code -ne 0 -or
        $dryRun.Output -notmatch "release dry-run completed release_pkg@1\.0\.0" -or
        $dryRun.Output -notmatch "release protect verification passed" -or
        $dryRun.Output -notmatch "dry-run publish passed") {
        Write-Output $dryRun.Output
        throw "expected release --dry-run to pass"
    }
    if (Test-Path (Join-Path $registry "release_pkg")) {
        Write-Output $dryRun.Output
        throw "expected release --dry-run not to write registry package"
    }
    if (-not (Test-Path $dryRunReportPath)) {
        Write-Output $dryRun.Output
        throw "expected release --dry-run --json to write a report"
    }
    $dryRunReport = [System.IO.File]::ReadAllText($dryRunReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($dryRunReport.schema -ne "sura.package.release.v1" -or
        $dryRunReport.package -ne "release_pkg" -or
        $dryRunReport.version -ne "1.0.0" -or
        $dryRunReport.passed -ne $true -or
        $dryRunReport.dry_run -ne $true -or
        -not ($dryRunReport.stages | Where-Object { $_.name -eq "protect_verify" -and $_.status -eq "pass" }) -or
        -not ($dryRunReport.stages | Where-Object { $_.name -eq "publish" -and $_.status -eq "pass" -and $_.message -match "dry-run" }) -or
        @($dryRunReport.next_actions).Count -ne 0) {
        $dryRunReport | ConvertTo-Json -Depth 6
        throw "unexpected release --dry-run JSON report"
    }

    $release = Run-Pkg -PkgArgs @("release", $good)
    if ($release.Code -ne 0 -or
        $release.Output -notmatch "release bench passed" -or
        $release.Output -notmatch "release protect verification passed" -or
        $release.Output -notmatch "quality:\s+PASS" -or
        $release.Output -notmatch "release completed release_pkg@1\.0\.0") {
        Write-Output $release.Output
        throw "expected release to pass"
    }
    if (-not (Test-Path (Join-Path $good "docs/index.html")) -or
        -not (Test-Path (Join-Path $good "docs/api.json")) -or
        -not (Test-Path (Join-Path $good "docs/search-index.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/release-bench.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/release-protect-verify.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/release-test.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/release-audit.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/release-quality.json")) -or
        -not (Test-Path (Join-Path $good "sura.pkg.sig")) -or
        -not (Test-Path (Join-Path $registry "release_pkg/1.0.0/package.surabundle.json"))) {
        throw "expected release to generate docs, bench report, protect-verify report, test report, audit report, quality report, signature, and registry bundle"
    }
    $benchReport = Get-Content -Raw -Path (Join-Path $good "artifacts/release-bench.json") | ConvertFrom-Json
    if ($benchReport.schema -ne "sura.package.bench.v1" -or
        $benchReport.package -ne "release_pkg" -or
        $benchReport.speedup -le 0) {
        throw "expected release bench report to include package timing data"
    }
    $protectVerifyReport = Get-Content -Raw -Path (Join-Path $good "artifacts/release-protect-verify.json") | ConvertFrom-Json
    if ($protectVerifyReport.schema -ne "sura.package.protect_verify.v1" -or
        $protectVerifyReport.passed -ne $true -or
        $protectVerifyReport.mode -ne "closed-source" -or
        $protectVerifyReport.keyed -ne $true -or
        $protectVerifyReport.licensed -ne $true -or
        $protectVerifyReport.expires -ne "2999-12-31" -or
        $protectVerifyReport.failure_count -ne 0) {
        $protectVerifyReport | ConvertTo-Json -Depth 6
        throw "expected release protect-verify report to include protected release evidence"
    }
    $testReport = Get-Content -Raw -Path (Join-Path $good "artifacts/release-test.json") | ConvertFrom-Json
    if ($testReport.schema -ne "sura.package.test.v1" -or
        $testReport.ok -ne $true -or
        $testReport.total -ne 1 -or
        $testReport.passed -ne 1 -or
        $testReport.failed -ne 0) {
        throw "expected release test report to include package test result data"
    }
    $auditReport = Get-Content -Raw -Path (Join-Path $good "artifacts/release-audit.json") | ConvertFrom-Json
    if ($auditReport.version -ne 1 -or
        $auditReport.passed -ne $true -or
        $auditReport.finding_count -ne 0) {
        throw "expected release audit report to include security gate data"
    }
    $qualityReport = Get-Content -Raw -Path (Join-Path $good "artifacts/release-quality.json") | ConvertFrom-Json
    if ($qualityReport.schema -ne "sura.package.quality.v1" -or
        $qualityReport.package -ne "release_pkg" -or
        $qualityReport.passed -ne $true -or
        $qualityReport.score -lt 80) {
        throw "expected release quality report to include package readiness data"
    }
    $docs = Get-Content -Raw -Path (Join-Path $good "docs/index.html")
    if ($docs -notmatch "API Reference" -or
        $docs -notmatch "Benchmark Summary" -or
        $docs -notmatch "artifacts/release-bench\.json" -or
        $docs -notmatch "Test Summary" -or
        $docs -notmatch "artifacts/release-test\.json" -or
        $docs -notmatch "Security Audit Summary" -or
        $docs -notmatch "artifacts/release-audit\.json" -or
        $docs -notmatch "Quality Summary" -or
        $docs -notmatch "artifacts/release-quality\.json" -or
        $docs -notmatch "Search Docs" -or
        $docs -notmatch "search-index\.json" -or
        $docs -notmatch "func add do" -or
        $docs -notmatch "src/release_pkg\.sura:1") {
        throw "expected release docs to include search, test summary, function signature, and source location"
    }
    $api = Get-Content -Raw -Path (Join-Path $good "docs/api.json")
    if ($api -notmatch '"name"\s*:\s*"release_pkg"' -or
        $api -notmatch '"symbols"\s*:' -or
        $api -notmatch '"kind"\s*:\s*"function"' -or
        $api -notmatch '"name"\s*:\s*"add"' -or
        $api -notmatch '"source"\s*:\s*"src/release_pkg\.sura"' -or
        $api -notmatch '"line"\s*:\s*1' -or
        $api -notmatch '"benchmark"\s*:' -or
        $api -notmatch '"source"\s*:\s*"artifacts/release-bench\.json"' -or
        $api -notmatch '"tests"\s*:' -or
        $api -notmatch '"source"\s*:\s*"artifacts/release-test\.json"' -or
        $api -notmatch '"audit"\s*:' -or
        $api -notmatch '"source"\s*:\s*"artifacts/release-audit\.json"' -or
        $api -notmatch '"quality"\s*:' -or
        $api -notmatch '"source"\s*:\s*"artifacts/release-quality\.json"') {
        throw "expected release docs api.json to include machine-readable function symbol, benchmark summary, test summary, audit summary, and quality summary"
    }
    $search = Get-Content -Raw -Path (Join-Path $good "docs/search-index.json")
    if ($search -notmatch '"entries"\s*:' -or
        $search -notmatch '"type"\s*:\s*"symbol"' -or
        $search -notmatch '"kind"\s*:\s*"function"' -or
        $search -notmatch '"name"\s*:\s*"add"' -or
        $search -notmatch '"type"\s*:\s*"benchmark"' -or
        $search -notmatch '"type"\s*:\s*"test"' -or
        $search -notmatch '"type"\s*:\s*"audit"' -or
        $search -notmatch '"type"\s*:\s*"quality"' -or
        $search -notmatch '"text"\s*:\s*"[^"]*func add do') {
        throw "expected release docs search-index.json to include searchable function symbol, benchmark summary, test summary, audit summary, and quality summary"
    }
    $index = Get-Content -Raw -Path (Join-Path $registry "index.json")
    if ($index -notmatch '"name"\s*:\s*"release_pkg"' -or $index -notmatch '"version"\s*:\s*"1\.0\.0"') {
        throw "expected registry index to include released package"
    }

    $releaseReportPath = Join-Path $temp "release-report.json"
    $jsonRelease = Run-Pkg -PkgArgs @("release", $good, "--json", $releaseReportPath)
    if ($jsonRelease.Code -ne 0 -or -not (Test-Path $releaseReportPath)) {
        Write-Output $jsonRelease.Output
        throw "expected release --json to pass and write a report"
    }
    $releaseReport = [System.IO.File]::ReadAllText($releaseReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($releaseReport.schema -ne "sura.package.release.v1" -or
        $releaseReport.package -ne "release_pkg" -or
        $releaseReport.version -ne "1.0.0" -or
        $releaseReport.passed -ne $true -or
        $releaseReport.dry_run -ne $false -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "docs" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "tests" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "test_report" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "bench" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "protect_verify" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "audit" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "docs_audit_refresh" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "sign" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "quality" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "docs_quality_refresh" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "sign_refresh" -and $_.status -eq "pass" }) -or
        -not ($releaseReport.stages | Where-Object { $_.name -eq "publish" -and $_.status -eq "pass" }) -or
        @($releaseReport.next_actions).Count -ne 0) {
        $releaseReport | ConvertTo-Json -Depth 6
        throw "unexpected release JSON report"
    }

    $bad = Join-Path $temp "bad_release_pkg"
    Write-Text (Join-Path $bad "sura.pkg.json") @"
{
  "name": "bad_release_pkg",
  "version": "1.0.0",
  "main": "src/bad_release_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $bad "src/bad_release_pkg.sura") "print `"bad`"`n"
    Write-Text (Join-Path $bad "tests/bad_release_pkg_test.sura") "assert(false)`n"

    $badReportPath = Join-Path $temp "bad-release-report.json"
    $badRelease = Run-Pkg -PkgArgs @("release", $bad, "--json=$badReportPath")
    if ($badRelease.Code -eq 0 -or $badRelease.Output -notmatch "release tests failed") {
        Write-Output $badRelease.Output
        throw "expected failing tests to stop release"
    }
    if (-not (Test-Path $badReportPath)) {
        Write-Output $badRelease.Output
        throw "expected failing release --json to write a report"
    }
    $badReport = [System.IO.File]::ReadAllText($badReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($badReport.schema -ne "sura.package.release.v1" -or
        $badReport.package -ne "bad_release_pkg" -or
        $badReport.passed -ne $false -or
        -not ($badReport.stages | Where-Object { $_.name -eq "tests" -and $_.status -eq "fail" }) -or
        -not ($badReport.next_actions | Where-Object { $_.stage -eq "tests" -and $_.action -match "surapkg test" -and $_.action -match "surapkg release" }) -or
        ($badReport.stages | Where-Object { $_.name -eq "publish" })) {
        $badReport | ConvertTo-Json -Depth 6
        throw "unexpected failing release JSON report"
    }
    if (Test-Path (Join-Path $registry "bad_release_pkg")) {
        throw "expected failed release not to publish package"
    }

    "release_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
