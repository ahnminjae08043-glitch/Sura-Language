param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_quality_" + [System.Guid]::NewGuid().ToString("N"))
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
    $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $good = Join-Path $temp "good_pkg"
    Write-Text (Join-Path $good "sura.pkg.json") @"
{
  "name": "good_pkg",
  "version": "1.0.0",
  "main": "src/good_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $good "src/good_pkg.sura") @"
func add do
  return 2 + 3
end
"@
    Write-Text (Join-Path $good "tests/good_pkg_test.sura") @"
assert(2 + 3 == 5)
"@
    Write-Text (Join-Path $good "README.md") "# good_pkg`n"

    $sign = Run-Pkg -PkgArgs @("sign", $good)
    if ($sign.Code -ne 0) {
        Write-Output $sign.Output
        throw "expected sign to pass"
    }

    $quality = Run-Pkg -PkgArgs @("quality", $good)
    if ($quality.Code -ne 0 -or $quality.Output -notmatch "quality:\s+PASS" -or $quality.Output -notmatch "score:\s+100/100") {
        Write-Output $quality.Output
        throw "expected complete package quality to pass with 100/100"
    }

    $qualityReport = Join-Path $temp "good-quality.json"
    $qualityJson = Run-Pkg -PkgArgs @("quality", $good, "--json", $qualityReport)
    if ($qualityJson.Code -ne 0 -or -not (Test-Path $qualityReport)) {
        Write-Output $qualityJson.Output
        throw "expected quality --json to pass and write a report"
    }
    $report = [System.IO.File]::ReadAllText($qualityReport, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($report.schema -ne "sura.package.quality.v1" -or
        $report.package -ne "good_pkg" -or
        $report.version -ne "1.0.0" -or
        $report.score -ne 100 -or
        $report.possible -ne 100 -or
        $report.grade -ne "A+" -or
        $report.passed -ne $true -or
        $report.errors -ne 0 -or
        @($report.next_actions).Count -ne 0 -or
        -not ($report.items | Where-Object { $_.status -eq "pass" -and $_.category -eq "security" -and $_.message -eq "security audit passed" })) {
        $report | ConvertTo-Json -Depth 6
        throw "unexpected quality JSON report"
    }

    $broken = Join-Path $temp "broken_pkg"
    Write-Text (Join-Path $broken "sura.pkg.json") @"
{
  "name": "broken_pkg",
  "version": "0.1.0",
  "dependencies": {}
}
"@
    $brokenReport = Join-Path $temp "broken-quality.json"
    $brokenQuality = Run-Pkg -PkgArgs @("quality", $broken, "--json=$brokenReport")
    if ($brokenQuality.Code -eq 0 -or $brokenQuality.Output -notmatch "quality:\s+FAIL" -or $brokenQuality.Output -notmatch "manifest missing main") {
        Write-Output $brokenQuality.Output
        throw "expected broken package quality to fail"
    }
    if (-not (Test-Path $brokenReport)) {
        Write-Output $brokenQuality.Output
        throw "expected failing quality --json to write a report"
    }
    $brokenJson = [System.IO.File]::ReadAllText($brokenReport, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($brokenJson.passed -ne $false -or $brokenJson.errors -lt 1 -or
        -not ($brokenJson.items | Where-Object { $_.status -eq "fail" -and $_.category -eq "manifest" -and $_.message -eq "manifest missing main" -and $_.action -eq "add main to sura.pkg.json" }) -or
        -not ($brokenJson.next_actions | Where-Object { $_.category -eq "manifest" -and $_.action -eq "add main to sura.pkg.json" })) {
        $brokenJson | ConvertTo-Json -Depth 6
        throw "unexpected failing quality JSON report"
    }

    $risky = Join-Path $temp "risky_pkg"
    Write-Text (Join-Path $risky "sura.pkg.json") @"
{
  "name": "risky_pkg",
  "version": "0.1.0",
  "main": "src/risky_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $risky "src/risky_pkg.sura") @"
print http_get("https://example.com/status")
"@
    Write-Text (Join-Path $risky "tests/risky_pkg_test.sura") "assert(true)`n"
    Write-Text (Join-Path $risky "README.md") "# risky_pkg`n"
    $riskyQuality = Run-Pkg -PkgArgs @("quality", $risky)
    if ($riskyQuality.Code -eq 0 -or $riskyQuality.Output -notmatch "security audit findings") {
        Write-Output $riskyQuality.Output
        throw "expected risky package quality to fail on security audit"
    }

    "quality_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# This gate printed PASS while exiting nonzero: its last native command
# was a negative check that correctly failed, and the script inherited
# that code. CI reads the exit code, so a passing gate reported failure.
exit 0
