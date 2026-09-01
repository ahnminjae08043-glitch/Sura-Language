param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_doctor_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Surapkg @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $pkg = Join-Path $temp "doctor_pkg"
    $broken = Join-Path $temp "broken_pkg"
    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $null

    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "doctor_pkg",
  "version": "0.1.0",
  "main": "src/doctor_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/doctor_pkg.sura") @"
print "doctor ok"
"@
    Write-Text (Join-Path $registry "index.json") @"
{
  "packages": [
    {"name":"doctor_pkg","version":"0.1.0","bundle":"doctor_pkg/0.1.0/package.surabundle.json","hash":"abc"}
  ]
}
"@
    Write-Text (Join-Path $registry "stats.json") @"
{
  "downloads": {"doctor_pkg@0.1.0": 1},
  "publishes": {"doctor_pkg@0.1.0": 1}
}
"@

    $ok = Run-Pkg -PkgArgs @("doctor", $pkg)
    if ($ok.Code -ne 0 -or
        $ok.Output -notmatch "platform:" -or
        $ok.Output -notmatch "sura command" -or
        $ok.Output -notmatch "C\+\+ compiler:" -or
        $ok.Output -notmatch "PowerShell runner:" -or
        $ok.Output -notmatch "package manifest: doctor_pkg@0.1.0" -or
        $ok.Output -notmatch "doctor:") {
        Write-Output $ok.Output
        throw "expected doctor to pass for healthy package"
    }
    $doctorReportPath = Join-Path $temp "doctor-report.json"
    $okJson = Run-Pkg -PkgArgs @("doctor", $pkg, "--json", $doctorReportPath)
    if ($okJson.Code -ne 0 -or -not (Test-Path $doctorReportPath)) {
        Write-Output $okJson.Output
        throw "expected doctor --json to pass and write a report"
    }
    $doctorReport = [System.IO.File]::ReadAllText($doctorReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($doctorReport.schema -ne "sura.doctor.v1" -or
        $doctorReport.passed -ne $true -or
        $doctorReport.error_count -ne 0 -or
        $doctorReport.ok_count -lt 1 -or
        -not ($doctorReport.items | Where-Object { $_.status -eq "ok" -and $_.message -match "platform:" }) -or
        -not ($doctorReport.items | Where-Object { $_.message -match "sura command" }) -or
        -not ($doctorReport.items | Where-Object { $_.status -eq "ok" -and $_.message -match "C\+\+ compiler:" }) -or
        -not ($doctorReport.items | Where-Object { $_.status -eq "ok" -and $_.message -match "PowerShell runner:" }) -or
        -not ($doctorReport.items | Where-Object { $_.status -eq "ok" -and $_.message -match "package manifest: doctor_pkg@0\.1\.0" })) {
        $doctorReport | ConvertTo-Json -Depth 6
        throw "unexpected doctor JSON report"
    }

    Write-Text (Join-Path $broken "sura.pkg.json") @"
{
  "name": "broken_pkg",
  "version": "0.1.0",
  "main": "src/missing.sura",
  "dependencies": {}
}
"@

    $bad = Run-Pkg -PkgArgs @("doctor", $broken)
    if ($bad.Code -eq 0 -or $bad.Output -notmatch "package main file missing") {
        Write-Output $bad.Output
        throw "expected doctor to fail for missing package main"
    }
    $badReportPath = Join-Path $temp "bad-doctor-report.json"
    $badJson = Run-Pkg -PkgArgs @("doctor", $broken, "--json=$badReportPath")
    if ($badJson.Code -eq 0 -or -not (Test-Path $badReportPath)) {
        Write-Output $badJson.Output
        throw "expected failing doctor --json to write a report"
    }
    $badReport = [System.IO.File]::ReadAllText($badReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($badReport.schema -ne "sura.doctor.v1" -or
        $badReport.passed -ne $false -or
        $badReport.error_count -lt 1 -or
        -not ($badReport.items | Where-Object { $_.status -eq "error" -and $_.message -match "package main file missing" })) {
        $badReport | ConvertTo-Json -Depth 6
        throw "unexpected failing doctor JSON report"
    }

    "doctor_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# The last check above is a negative test, so this script printed PASS while
# inheriting its nonzero exit code. State the verdict explicitly.
exit 0
