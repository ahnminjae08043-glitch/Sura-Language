param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_engine_test_" + [System.Guid]::NewGuid().ToString("N"))

try {
    $testsDir = Join-Path $root "tests"
    New-Item -ItemType Directory -Force -Path $testsDir | Out-Null

    Set-Content -LiteralPath (Join-Path $testsDir "pass_test.sura") -Encoding UTF8 @"
print "hidden pass output"
assert(true)
"@

    $report = Join-Path $root "report.json"
    $passOutput = & $enginePath --test-report $report $root 2>&1
    $passCode = $LASTEXITCODE
    if ($passCode -ne 0 -or ($passOutput -join "`n") -notmatch "Sura tests: 1 passed, 0 failed") {
        $passOutput | Write-Host
        throw "expected passing --test-report run"
    }
    if (-not (Test-Path -LiteralPath $report)) {
        throw "expected test report to be written"
    }
    $json = Get-Content -Raw -Encoding UTF8 -LiteralPath $report | ConvertFrom-Json
    if ($json.passed -ne 1 -or $json.failed -ne 0 -or $json.tests.Count -ne 1) {
        throw "unexpected test report counts"
    }

    Set-Content -LiteralPath (Join-Path $testsDir "fail_test.sura") -Encoding UTF8 @"
print "visible fail output"
assert(false)
"@

    $failOutput = & $enginePath --test $root 2>&1
    $failCode = $LASTEXITCODE
    $failText = $failOutput -join "`n"
    if ($failCode -eq 0 -or $failText -notmatch "\[FAIL\]" -or $failText -notmatch "visible fail output") {
        $failOutput | Write-Host
        throw "expected failing --test run to expose failure output"
    }

    Write-Host "[PASS] engine test runner smoke"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

# The last check above is a negative test, so this script printed PASS while
# inheriting its nonzero exit code. State the verdict explicitly.
exit 0
