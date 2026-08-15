param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_ci_" + [System.Guid]::NewGuid().ToString("N"))
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

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $good = Join-Path $temp "ci_pkg"
    Write-Text (Join-Path $good "sura.pkg.json") @"
{
  "name": "ci_pkg",
  "version": "1.0.0",
  "main": "src/ci_pkg.sura",
  "bench": true,
  "bench_min_speedup": "0.001",
  "bench_report": "artifacts/ci-bench.json",
  "protect_report": "artifacts/ci-protect.json",
  "protect_verify_report": "artifacts/ci-protect-verify.json",
  "protect_require_closed_source": true,
  "protect_require_key": true,
  "protect_require_license": true,
  "protect_require_expires": true,
  "protect_require_target": "package",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $good "src/ci_pkg.sura") @"
func add do
  return 2 + 3
end
"@
    Write-Text (Join-Path $good "tests/ci_pkg_test.sura") @"
assert(2 + 3 == 5)
"@
    Write-Text (Join-Path $good "README.md") "# ci_pkg`n"
    Write-Text (Join-Path $good "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_request"],
  "url_prefixes": ["file://"],
  "http_methods": ["GET"],
  "allowed_headers": ["X-Agent"],
  "required_headers": {"X-Agent": "sura-ci-smoke"},
  "max_timeout": 30,
  "max_body_bytes": 0,
  "approval": false,
  "allow_shell": false,
  "command_prefixes": []
}
"@
    $keyFile = Join-Path $temp "ci-release.key"
    $licenseFile = Join-Path $temp "ci-release.license"
    Write-Text $keyFile "ci-release-key"
    Write-Text $licenseFile "ci-release-license"
    $protect = Run-Pkg -PkgArgs @(
        "protect", $good,
        "--out", (Join-Path $good "dist/ci_pkg.sura.srp"),
        "--closed-source",
        "--key-file", $keyFile,
        "--license-file", $licenseFile,
        "--expires", "2999-12-31",
        "--json", (Join-Path $good "artifacts/ci-protect.json")
    )
    if ($protect.Code -ne 0) {
        Write-Output $protect.Output
        throw "expected protect setup for ci smoke to pass"
    }

    $unsignedCi = Run-Pkg -PkgArgs @("ci", $good)
    if ($unsignedCi.Code -ne 0 -or
        $unsignedCi.Output -notmatch "Sura CI" -or
        $unsignedCi.Output -notmatch "ci docs generated" -or
        $unsignedCi.Output -notmatch "Sura tests: 1 passed, 0 failed" -or
        $unsignedCi.Output -notmatch "ci bench passed" -or
        $unsignedCi.Output -notmatch "ci protect verification passed" -or
        $unsignedCi.Output -notmatch "ci package signature not present; skipping verify" -or
        $unsignedCi.Output -notmatch "quality:\s+PASS" -or
        $unsignedCi.Output -notmatch "ci completed ci_pkg@1\.0\.0") {
        Write-Output $unsignedCi.Output
        throw "expected unsigned ci to pass and skip absent signatures"
    }
    if (-not (Test-Path (Join-Path $good "docs/index.html")) -or
        -not (Test-Path (Join-Path $good "docs/api.json")) -or
        -not (Test-Path (Join-Path $good "docs/search-index.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/ci-bench.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/ci-protect-verify.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/ci-test.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/ci-audit.json")) -or
        -not (Test-Path (Join-Path $good "artifacts/ci-quality.json"))) {
        throw "expected ci to generate searchable package docs, bench report, protect-verify report, test report, audit report, and quality report"
    }
    $benchReport = Get-Content -Raw -Path (Join-Path $good "artifacts/ci-bench.json") | ConvertFrom-Json
    if ($benchReport.schema -ne "sura.package.bench.v1" -or
        $benchReport.package -ne "ci_pkg" -or
        $benchReport.speedup -le 0) {
        throw "expected ci bench report to include package timing data"
    }
    $protectVerifyReport = Get-Content -Raw -Path (Join-Path $good "artifacts/ci-protect-verify.json") | ConvertFrom-Json
    if ($protectVerifyReport.schema -ne "sura.package.protect_verify.v1" -or
        $protectVerifyReport.passed -ne $true -or
        $protectVerifyReport.mode -ne "closed-source" -or
        $protectVerifyReport.keyed -ne $true -or
        $protectVerifyReport.licensed -ne $true -or
        $protectVerifyReport.expires -ne "2999-12-31" -or
        $protectVerifyReport.failure_count -ne 0) {
        $protectVerifyReport | ConvertTo-Json -Depth 6
        throw "expected ci protect-verify report to include protected release evidence"
    }
    $testReport = Get-Content -Raw -Path (Join-Path $good "artifacts/ci-test.json") | ConvertFrom-Json
    if ($testReport.schema -ne "sura.package.test.v1" -or
        $testReport.ok -ne $true -or
        $testReport.total -ne 1 -or
        $testReport.passed -ne 1 -or
        $testReport.failed -ne 0) {
        throw "expected ci test report to include package test result data"
    }
    $auditReport = Get-Content -Raw -Path (Join-Path $good "artifacts/ci-audit.json") | ConvertFrom-Json
    if ($auditReport.version -ne 1 -or
        $auditReport.passed -ne $true -or
        $auditReport.finding_count -ne 0) {
        throw "expected ci audit report to include security gate data"
    }
    $qualityReport = Get-Content -Raw -Path (Join-Path $good "artifacts/ci-quality.json") | ConvertFrom-Json
    if ($qualityReport.schema -ne "sura.package.quality.v1" -or
        $qualityReport.package -ne "ci_pkg" -or
        $qualityReport.passed -ne $true -or
        $qualityReport.score -lt 80) {
        throw "expected ci quality report to include package readiness data"
    }

    $ciReportPath = Join-Path $temp "ci-report.json"
    $jsonCi = Run-Pkg -PkgArgs @("ci", $good, "--json", $ciReportPath)
    if ($jsonCi.Code -ne 0 -or -not (Test-Path $ciReportPath)) {
        Write-Output $jsonCi.Output
        throw "expected ci --json to pass and write a report"
    }
    $ciReport = [System.IO.File]::ReadAllText($ciReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($ciReport.schema -ne "sura.package.ci.v1" -or
        $ciReport.package -ne "ci_pkg" -or
        $ciReport.version -ne "1.0.0" -or
        $ciReport.passed -ne $true -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "docs" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "tests" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "test_report" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "bench" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "protect_verify" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "audit" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "docs_audit_refresh" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "docs_quality_refresh" -and $_.status -eq "pass" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "verify_package_signature" -and $_.status -eq "skip" }) -or
        -not ($ciReport.stages | Where-Object { $_.name -eq "quality" -and $_.status -eq "pass" }) -or
        @($ciReport.next_actions).Count -ne 0) {
        $ciReport | ConvertTo-Json -Depth 6
        throw "unexpected ci JSON report"
    }

    $signPolicy = Run-Pkg -PkgArgs @("sign-policy", $good)
    if ($signPolicy.Code -ne 0) {
        Write-Output $signPolicy.Output
        throw "expected tool policy sign to pass"
    }
    $sign = Run-Pkg -PkgArgs @("sign", $good)
    if ($sign.Code -ne 0) {
        Write-Output $sign.Output
        throw "expected package sign to pass"
    }

    $signedCi = Run-Pkg -PkgArgs @("ci", $good)
    if ($signedCi.Code -ne 0 -or
        $signedCi.Output -notmatch "ci package signature verified" -or
        $signedCi.Output -notmatch "ci tool policy verified" -or
        $signedCi.Output -notmatch "ci bench passed" -or
        $signedCi.Output -notmatch "ci protect verification passed" -or
        $signedCi.Output -notmatch "quality:\s+PASS" -or
        $signedCi.Output -notmatch "ci completed ci_pkg@1\.0\.0") {
        Write-Output $signedCi.Output
        throw "expected signed ci to verify package and tool policy signatures"
    }

    $docs = Get-Content -Raw -Path (Join-Path $good "docs/index.html")
    if ($docs -notmatch "API Reference" -or
        $docs -notmatch "Benchmark Summary" -or
        $docs -notmatch "artifacts/ci-bench\.json" -or
        $docs -notmatch "Test Summary" -or
        $docs -notmatch "artifacts/ci-test\.json" -or
        $docs -notmatch "Security Audit Summary" -or
        $docs -notmatch "artifacts/ci-audit\.json" -or
        $docs -notmatch "Quality Summary" -or
        $docs -notmatch "artifacts/ci-quality\.json" -or
        $docs -notmatch "Search Docs" -or
        $docs -notmatch "Tool Policy Summary" -or
        $docs -notmatch "http_request" -or
        $docs -notmatch "src/ci_pkg\.sura:1") {
        throw "expected ci docs to include searchable API, test, audit, quality, and tool policy summary"
    }
    $search = Get-Content -Raw -Path (Join-Path $good "docs/search-index.json")
    if ($search -notmatch '"entries"\s*:' -or
        $search -notmatch '"type"\s*:\s*"symbol"' -or
        $search -notmatch '"name"\s*:\s*"add"' -or
        $search -notmatch '"type"\s*:\s*"benchmark"' -or
        $search -notmatch '"type"\s*:\s*"test"' -or
        $search -notmatch '"type"\s*:\s*"audit"' -or
        $search -notmatch '"type"\s*:\s*"quality"' -or
        $search -notmatch '"type"\s*:\s*"tool_policy"' -or
        $search -notmatch '"name"\s*:\s*"http_request"') {
        throw "expected ci docs search-index.json to include symbols, test, audit, quality, and tool policy entries"
    }

    $bad = Join-Path $temp "bad_ci_pkg"
    Write-Text (Join-Path $bad "sura.pkg.json") @"
{
  "name": "bad_ci_pkg",
  "version": "1.0.0",
  "main": "src/bad_ci_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $bad "src/bad_ci_pkg.sura") "print `"bad`"`n"
    Write-Text (Join-Path $bad "tests/bad_ci_pkg_test.sura") "assert(false)`n"
    Write-Text (Join-Path $bad "README.md") "# bad_ci_pkg`n"

    $badReportPath = Join-Path $temp "bad-ci-report.json"
    $badCi = Run-Pkg -PkgArgs @("ci", $bad, "--json=$badReportPath")
    if ($badCi.Code -eq 0 -or $badCi.Output -notmatch "ci tests failed") {
        Write-Output $badCi.Output
        throw "expected failing tests to stop ci"
    }
    if (-not (Test-Path $badReportPath)) {
        Write-Output $badCi.Output
        throw "expected failing ci --json to write a report"
    }
    $badReport = [System.IO.File]::ReadAllText($badReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($badReport.schema -ne "sura.package.ci.v1" -or
        $badReport.package -ne "bad_ci_pkg" -or
        $badReport.passed -ne $false -or
        -not ($badReport.stages | Where-Object { $_.name -eq "tests" -and $_.status -eq "fail" }) -or
        -not ($badReport.next_actions | Where-Object { $_.stage -eq "tests" -and $_.action -match "surapkg test" -and $_.action -match "surapkg ci" }) -or
        ($badReport.stages | Where-Object { $_.name -eq "quality" })) {
        $badReport | ConvertTo-Json -Depth 6
        throw "unexpected failing ci JSON report"
    }

    "ci_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# The last check above is a negative test, so this script printed PASS while
# inheriting its nonzero exit code. State the verdict explicitly.
exit 0
