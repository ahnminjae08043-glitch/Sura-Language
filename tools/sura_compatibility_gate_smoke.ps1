param(
    [string]$RepoRoot = ".",
    [string]$Engine = "",
    [string]$Surapkg = ""
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$gate = Join-Path $root "tools/sura_compatibility_gate.ps1"
$enginePath = if ([string]::IsNullOrWhiteSpace($Engine)) {
    Join-Path $root $(if ($env:OS -eq "Windows_NT") { "SuraLanguage.exe" } else { "SuraLanguage" })
} else { $Engine }
$surapkgPath = if ([string]::IsNullOrWhiteSpace($Surapkg)) {
    Join-Path $root $(if ($env:OS -eq "Windows_NT") { "surapkg.exe" } else { "surapkg" })
} else { $Surapkg }
$enginePath = (Resolve-Path -LiteralPath $enginePath).Path
$surapkgPath = (Resolve-Path -LiteralPath $surapkgPath).Path
$hostPath = (Get-Process -Id $PID).Path

function Read-Utf8([string]$Path) {
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Write-Json([string]$Path, $Value) {
    [System.IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 20) + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Invoke-Gate([string]$Contract, [string]$Report) {
    $arguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $gate,
        "-RepoRoot", $root,
        "-Engine", $enginePath,
        "-Surapkg", $surapkgPath,
        "-ContractPath", $Contract,
        "-JsonOut", $Report
    )
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = (& $hostPath @arguments 2>&1 | ForEach-Object { "$_" }) -join "`n"
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
$temp = Join-Path $tempRoot ("sura_compat_gate_smoke_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $contractPath = Join-Path $root "compatibility.json"
    $goodReport = Join-Path $temp "good-report.json"
    $good = Invoke-Gate $contractPath $goodReport
    if ($good.ExitCode -ne 0 -or $good.Output -notmatch 'sura_compatibility_gate: PASS') {
        throw "expected the unmodified compatibility contract to pass: $($good.Output)"
    }
    $report = (Read-Utf8 $goodReport) | ConvertFrom-Json
    if (-not $report.passed -or
        $report.stable_api.checked -ne "passed" -or
        [int]$report.stable_api.modules -le 0 -or
        [int]$report.stable_api.signatures -le 0) {
        throw "good compatibility report did not record stable API verification"
    }
    $historicalReports = @($report.historical_probes)
    if ($historicalReports.Count -ne 1 -or
        [string]$historicalReports[0].status -ne "verification_only" -or
        [string]$historicalReports[0].current_runtime.status -ne "passed" -or
        @($historicalReports[0].current_runtime.fixtures).Count -ne 3) {
        throw "good compatibility report did not record historical source verification"
    }
    $historicalApplicable = ($env:OS -eq "Windows_NT" -and [string]$env:PROCESSOR_ARCHITECTURE -eq "AMD64")
    $expectedArchivedStatus = if ($historicalApplicable) { "passed" } else { "not_applicable" }
    if ([string]$historicalReports[0].archived_runtime.status -ne $expectedArchivedStatus) {
        throw "historical archived runtime status mismatch: expected $expectedArchivedStatus"
    }
    if ($historicalApplicable -and
        ([string]$historicalReports[0].archived_runtime.bytecode_forward_load -ne "passed" -or
         @($historicalReports[0].archived_runtime.fixtures).Count -ne 3)) {
        throw "good compatibility report did not record archived runtime and bytecode verification"
    }

    $emptyHistoricalContract = (Read-Utf8 $contractPath) | ConvertFrom-Json
    $emptyHistoricalContract.source.historical_probes[0].fixtures = @()
    $emptyHistoricalPath = Join-Path $temp "empty-historical-fixtures.json"
    Write-Json $emptyHistoricalPath $emptyHistoricalContract
    $emptyHistorical = Invoke-Gate $emptyHistoricalPath (Join-Path $temp "empty-historical-report.json")
    if ($emptyHistorical.ExitCode -eq 0 -or $emptyHistorical.Output -notmatch 'no source fixtures') {
        throw "expected an empty historical probe to fail: $($emptyHistorical.Output)"
    }

    $shortContract = (Read-Utf8 $contractPath) | ConvertFrom-Json
    $shortContract.support.minimum_maintenance_until = [string]$shortContract.support.policy_effective
    $shortPath = Join-Path $temp "short-support.json"
    Write-Json $shortPath $shortContract
    $short = Invoke-Gate $shortPath (Join-Path $temp "short-report.json")
    if ($short.ExitCode -eq 0 -or $short.Output -notmatch 'maintenance') {
        throw "expected an undersized maintenance window to fail: $($short.Output)"
    }

    $apiContract = (Read-Utf8 $contractPath) | ConvertFrom-Json
    $sourceSnapshotPath = if ([System.IO.Path]::IsPathRooted([string]$apiContract.stable_api.snapshot)) {
        [string]$apiContract.stable_api.snapshot
    } else {
        Join-Path $root ([string]$apiContract.stable_api.snapshot)
    }
    $changedSnapshot = (Read-Utf8 $sourceSnapshotPath) | ConvertFrom-Json
    $changedSnapshot.modules[0].symbols[0].signature = [string]$changedSnapshot.modules[0].symbols[0].signature + " [changed]"
    $changedSnapshotPath = Join-Path $temp "changed-stable-api.json"
    Write-Json $changedSnapshotPath $changedSnapshot
    $apiContract.stable_api.snapshot = $changedSnapshotPath
    $apiContractPath = Join-Path $temp "changed-api-contract.json"
    Write-Json $apiContractPath $apiContract
    $changed = Invoke-Gate $apiContractPath (Join-Path $temp "changed-report.json")
    if ($changed.ExitCode -eq 0 -or $changed.Output -notmatch 'stable API signature changed') {
        throw "expected a changed stable API signature to fail: $($changed.Output)"
    }

    Write-Host "sura_compatibility_gate_smoke: PASS (guaranteed and historical contracts accepted; empty history, short support, and changed API rejected)"
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    $leaf = [System.IO.Path]::GetFileName($resolvedTemp)
    $parent = [System.IO.Path]::GetDirectoryName($resolvedTemp).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    if ($parent -eq $tempRoot -and $leaf -match '^sura_compat_gate_smoke_[0-9a-f]{32}$' -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
