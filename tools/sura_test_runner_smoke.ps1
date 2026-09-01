param(
    [string]$Surapkg = ".\surapkg.exe",
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$EnginePath = (Resolve-Path $Engine).Path
$root = Split-Path -Parent $PSScriptRoot
$testScript = Join-Path $root "tools\sura_test.ps1"
$stableTestScript = Join-Path $root "run_stable_tests.ps1"
$processHelper = Join-Path $root "tools\sura_test_process.ps1"
. $processHelper
$timeoutFixture = Join-Path $root "tools\fixtures\test_runner_timeout.sura"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_test_runner_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$engineSnapshotUnderTest = $null

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Native {
    param([scriptblock]$Block)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Block 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $korean = [string]::Concat(
        [char]0xD55C, [char]0xAD6D, [char]0xC5B4, [char]0x0020,
        [char]0xD14C, [char]0xC2A4, [char]0xD2B8, [char]0x0020,
        [char]0xCD9C, [char]0xB825
    )
    $koreanPattern = [regex]::Escape($korean)

    $mutableEngineRoot = Join-Path $temp "mutable engine"
    New-Item -ItemType Directory -Force -Path $mutableEngineRoot | Out-Null
    $mutableEngine = Join-Path $mutableEngineRoot ([System.IO.Path]::GetFileName($EnginePath))
    Copy-Item -LiteralPath $EnginePath -Destination $mutableEngine -Force
    foreach ($runtimeFile in @(Get-ChildItem -LiteralPath (Split-Path -Parent $EnginePath) -File | Where-Object {
        $_.Name -match '(?i)\.dll$|\.dylib$|\.so(?:\.|$)'
    })) {
        Copy-Item -LiteralPath $runtimeFile.FullName -Destination (Join-Path $mutableEngineRoot $runtimeFile.Name) -Force
    }
    $engineSnapshotUnderTest = New-SuraTestEngineSnapshot -EnginePath $mutableEngine
    [System.IO.File]::WriteAllText($mutableEngine, "not an executable", $utf8NoBom)
    $snapshotProbe = Join-Path $temp "snapshot_probe.sura"
    Write-Text $snapshotProbe "print `"snapshot-ok`"`n"
    $snapshotRun = Invoke-SuraTestProcess -EnginePath $engineSnapshotUnderTest.Path -Arguments @($snapshotProbe) -TimeoutSeconds 10
    if ($snapshotRun.ExitCode -ne 0 -or
        $snapshotRun.Output -notmatch 'snapshot-ok' -or
        -not (Test-SuraTestEngineSnapshot -Snapshot $engineSnapshotUnderTest) -or
        (Test-SuraTestEngineSourceUnchanged -Snapshot $engineSnapshotUnderTest)) {
        throw "expected the immutable engine snapshot to survive source engine replacement"
    }
    Remove-SuraTestEngineSnapshot -Snapshot $engineSnapshotUnderTest
    $engineSnapshotUnderTest = $null

    # Keep a space in the package path so the Windows PowerShell 5 argument
    # quoting fallback is exercised as well as ProcessStartInfo.ArgumentList.
    $good = Join-Path $temp "good package"
    Write-Text (Join-Path $good "sura.pkg.json") @"
{
  "name": "good_pkg",
  "version": "1.0.0",
  "main": "src/good_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $good "src/good_pkg.sura") "print `"good`"`n"
    Write-Text (Join-Path $good "tests/good_pkg_test.sura") ("print `"$korean`"`nassert(2 + 3 == 5)`n")

    $json = Join-Path $temp "good-report.json"
    $junit = Join-Path $temp "good-junit.xml"
    $pkgResult = Run-Native { & $SurapkgPath test $good --json $json --junit $junit }
    if ($pkgResult.Code -ne 0 -or
        $pkgResult.Output -notmatch "Sura tests: 1 passed, 0 failed" -or
        -not (Test-Path -LiteralPath $json) -or
        -not (Test-Path -LiteralPath $junit)) {
        Write-Output $pkgResult.Output
        throw "expected surapkg test to write JSON and JUnit reports"
    }
    $jsonText = [System.IO.File]::ReadAllText($json, [System.Text.Encoding]::UTF8)
    $junitText = [System.IO.File]::ReadAllText($junit, [System.Text.Encoding]::UTF8)
    if ($jsonText -notmatch '"schema"\s*:\s*"sura\.package\.test\.v1"' -or
        $jsonText -notmatch '"ok"\s*:\s*true' -or
        $jsonText -notmatch '"total"\s*:\s*1' -or
        $jsonText -notmatch '"passed"\s*:\s*1' -or
        $jsonText -notmatch $koreanPattern -or
        $junitText -notmatch '<testsuite[^>]+tests="1"' -or
        $junitText -notmatch '<property name="jit" value="true"' -or
        $junitText -notmatch $koreanPattern) {
        throw "expected surapkg test reports to include pass counts, JIT mode, and UTF-8 output"
    }

    $psJson = Join-Path $temp "ps-report.json"
    $psJUnit = Join-Path $temp "ps-junit.xml"
    $psResult = Run-Native { powershell -NoProfile -ExecutionPolicy Bypass -File $testScript -Path $good -Engine $EnginePath -Report $psJson -JUnit $psJUnit }
    if ($psResult.Code -ne 0 -or
        $psResult.Output -notmatch "Sura tests: 1 passed, 0 skipped, 0 failed" -or
        -not (Test-Path -LiteralPath $psJson) -or
        -not (Test-Path -LiteralPath $psJUnit)) {
        Write-Output $psResult.Output
        throw "expected sura_test.ps1 to write JSON and JUnit reports"
    }
    $psJsonText = [System.IO.File]::ReadAllText($psJson, [System.Text.Encoding]::UTF8)
    $psJUnitText = [System.IO.File]::ReadAllText($psJUnit, [System.Text.Encoding]::UTF8)
    if ($psJsonText -notmatch '"engineSha256"\s*:\s*"[0-9a-f]{64}"' -or
        $psJsonText -notmatch '"engineSnapshot"\s*:\s*true' -or
        $psJUnitText -notmatch '<property name="engineSha256" value="[0-9a-f]{64}"') {
        throw "expected PowerShell test reports to identify the immutable engine snapshot"
    }

    $global:LASTEXITCODE = 77
    & $testScript -Path $good -Engine $EnginePath `
        -Report (Join-Path $temp "in-process-report.json") -NoJit | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "expected successful in-process sura_test.ps1 to reset LASTEXITCODE"
    }

    $global:LASTEXITCODE = 77
    & $stableTestScript -Engine $EnginePath `
        -TestPath (Join-Path $good "tests\good_pkg_test.sura") -NoJit | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "expected successful in-process run_stable_tests.ps1 to reset LASTEXITCODE"
    }

    $timeoutJson = Join-Path $temp "timeout-report.json"
    $timeoutJUnit = Join-Path $temp "timeout-junit.xml"
    $timeoutWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $timeoutResult = Run-Native {
        powershell -NoProfile -ExecutionPolicy Bypass -File $testScript `
            -Path $timeoutFixture -Engine $EnginePath -Report $timeoutJson `
            -JUnit $timeoutJUnit -NoJit -TimeoutSeconds 1
    }
    $timeoutWatch.Stop()
    if ($timeoutResult.Code -eq 0 -or
        $timeoutWatch.Elapsed.TotalSeconds -gt 8 -or
        $timeoutResult.Output -notmatch '\[TIMEOUT\].*exceeded 1s' -or
        -not (Test-Path -LiteralPath $timeoutJson) -or
        -not (Test-Path -LiteralPath $timeoutJUnit)) {
        Write-Output $timeoutResult.Output
        throw "expected sura_test.ps1 to stop and report a timed-out test"
    }
    $timeoutJsonText = [System.IO.File]::ReadAllText($timeoutJson, [System.Text.Encoding]::UTF8)
    $timeoutJUnitText = [System.IO.File]::ReadAllText($timeoutJUnit, [System.Text.Encoding]::UTF8)
    if ($timeoutJsonText -notmatch '"timedOut"\s*:\s*true' -or
        $timeoutJsonText -notmatch '"exitCode"\s*:\s*124' -or
        $timeoutJsonText -notmatch '"timeoutSeconds"\s*:\s*1' -or
        $timeoutJUnitText -notmatch 'failure message="timed out after 1s"') {
        throw "expected timeout JSON/JUnit reports to preserve timeout evidence"
    }

    $stableTimeoutResult = Run-Native {
        powershell -NoProfile -ExecutionPolicy Bypass -File $stableTestScript `
            -Engine $EnginePath -TestPath $timeoutFixture -NoJit -TimeoutSeconds 1
    }
    if ($stableTimeoutResult.Code -eq 0 -or
        $stableTimeoutResult.Output -notmatch '\[TIMEOUT\].*exceeded 1s' -or
        $stableTimeoutResult.Output -notmatch 'Stable tests \(VM\): 0 passed, 0 skipped, 1 failed') {
        Write-Output $stableTimeoutResult.Output
        throw "expected stable test runner to stop and report a timed-out test"
    }

    $skipFixture = Join-Path $temp "hardware_skip.sura"
    Write-Text $skipFixture "print `"hardware_skip: SKIP (CUDA device unavailable)`"`n"
    $skipJson = Join-Path $temp "skip-report.json"
    $skipJUnit = Join-Path $temp "skip-junit.xml"
    $skipResult = Run-Native {
        powershell -NoProfile -ExecutionPolicy Bypass -File $testScript `
            -Path $skipFixture -Engine $EnginePath -Report $skipJson `
            -JUnit $skipJUnit -NoJit
    }
    if ($skipResult.Code -ne 0 -or
        $skipResult.Output -notmatch '\[SKIP\].*CUDA device unavailable' -or
        $skipResult.Output -notmatch 'Sura tests: 0 passed, 1 skipped, 0 failed') {
        Write-Output $skipResult.Output
        throw "expected sura_test.ps1 to classify a runtime capability skip separately from pass"
    }
    $skipJsonText = [System.IO.File]::ReadAllText($skipJson, [System.Text.Encoding]::UTF8)
    $skipJUnitText = [System.IO.File]::ReadAllText($skipJUnit, [System.Text.Encoding]::UTF8)
    if ($skipJsonText -notmatch '"status"\s*:\s*"skip"' -or
        $skipJsonText -notmatch '"skipped"\s*:\s*1' -or
        $skipJsonText -notmatch '"skipReason"\s*:\s*"CUDA device unavailable"' -or
        $skipJUnitText -notmatch '<testsuite[^>]+skipped="1"' -or
        $skipJUnitText -notmatch '<skipped message="CUDA device unavailable"') {
        throw "expected JSON/JUnit reports to preserve runtime skip evidence"
    }
    $strictSkipResult = Run-Native {
        powershell -NoProfile -ExecutionPolicy Bypass -File $testScript `
            -Path $skipFixture -Engine $EnginePath -Report (Join-Path $temp "strict-skip.json") `
            -NoJit -FailOnSkip
    }
    if ($strictSkipResult.Code -eq 0) {
        throw "expected -FailOnSkip to reject an unverified hardware test"
    }
    $stableSkipResult = Run-Native {
        powershell -NoProfile -ExecutionPolicy Bypass -File $stableTestScript `
            -Engine $EnginePath -TestPath $skipFixture -NoJit
    }
    if ($stableSkipResult.Code -ne 0 -or
        $stableSkipResult.Output -notmatch '\[SKIP\].*CUDA device unavailable' -or
        $stableSkipResult.Output -notmatch 'Stable tests \(VM\): 0 passed, 1 skipped, 0 failed') {
        Write-Output $stableSkipResult.Output
        throw "expected stable tests to classify a runtime capability skip separately from pass"
    }

    $bad = Join-Path $temp "bad_pkg"
    Write-Text (Join-Path $bad "sura.pkg.json") @"
{
  "name": "bad_pkg",
  "version": "1.0.0",
  "main": "src/bad_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $bad "src/bad_pkg.sura") "print `"bad`"`n"
    Write-Text (Join-Path $bad "tests/bad_pkg_test.sura") "assert(false)`n"
    $badJson = Join-Path $temp "bad-report.json"
    $badJUnit = Join-Path $temp "bad-junit.xml"
    $badResult = Run-Native { & $SurapkgPath test $bad --no-jit "--json=$badJson" --junit $badJUnit }
    if ($badResult.Code -eq 0 -or
        $badResult.Output -notmatch "Sura tests: 0 passed, 1 failed" -or
        -not (Test-Path -LiteralPath $badJson) -or
        -not (Test-Path -LiteralPath $badJUnit)) {
        Write-Output $badResult.Output
        throw "expected failing surapkg test to return nonzero and still write reports"
    }
    $badText = [System.IO.File]::ReadAllText($badJUnit, [System.Text.Encoding]::UTF8)
    $badJsonText = [System.IO.File]::ReadAllText($badJson, [System.Text.Encoding]::UTF8)
    if ($badJsonText -notmatch '"schema"\s*:\s*"sura\.package\.test\.v1"' -or
        $badJsonText -notmatch '"ok"\s*:\s*false' -or
        $badJsonText -notmatch '"failed"\s*:\s*1' -or
        $badText -notmatch 'failures="1"' -or
        $badText -notmatch '<failure message=' -or
        $badText -notmatch '<property name="jit" value="false"') {
        throw "expected failing JUnit report to record failure and no-JIT mode"
    }

    "test_runner_smoke: PASS"
}
finally {
    if ($null -ne $engineSnapshotUnderTest) {
        Remove-SuraTestEngineSnapshot -Snapshot $engineSnapshotUnderTest
    }
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
$global:LASTEXITCODE = 0

# Verified passing; state the exit code rather than inheriting it.
exit 0
