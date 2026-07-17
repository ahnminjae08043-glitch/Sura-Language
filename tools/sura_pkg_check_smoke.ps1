param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_check_" + [System.Guid]::NewGuid().ToString("N"))
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
    $goodJson = Join-Path $temp "good-check.json"
    $goodResult = Run-Pkg -PkgArgs @("check", $good, "--json", $goodJson)
    if ($goodResult.Code -ne 0 -or
        $goodResult.Output -notmatch "Sura check: 2 passed, 0 failed" -or
        -not (Test-Path -LiteralPath $goodJson)) {
        Write-Output $goodResult.Output
        throw "expected clean package check to pass and write JSON"
    }
    $goodReport = [System.IO.File]::ReadAllText($goodJson, [System.Text.Encoding]::UTF8)
    if ($goodReport -notmatch '"passed"\s*:\s*2' -or
        $goodReport -notmatch '"failed"\s*:\s*0' -or
        $goodReport -notmatch '"strict"\s*:\s*true') {
        throw "expected clean check JSON counts and strict-by-default mode"
    }

    $strictJson = Join-Path $temp "strict-check.json"
    $strictResult = Run-Pkg -PkgArgs @("check", $good, "--strict", "--json", $strictJson)
    if ($strictResult.Code -ne 0 -or
        $strictResult.Output -notmatch "Sura check: 2 passed, 0 failed") {
        Write-Output $strictResult.Output
        throw "expected strict clean package check to pass"
    }
    $strictReport = [System.IO.File]::ReadAllText($strictJson, [System.Text.Encoding]::UTF8)
    if ($strictReport -notmatch '"strict"\s*:\s*true') {
        throw "expected strict mode in check JSON"
    }

    $legacyJson = Join-Path $temp "legacy-check.json"
    $legacyResult = Run-Pkg -PkgArgs @("check", $good, "--legacy-types", "--json", $legacyJson)
    if ($legacyResult.Code -ne 0 -or
        $legacyResult.Output -notmatch "Sura check: 2 passed, 0 failed") {
        Write-Output $legacyResult.Output
        throw "expected explicit legacy type check to preserve compatibility"
    }
    $legacyReport = [System.IO.File]::ReadAllText($legacyJson, [System.Text.Encoding]::UTF8)
    if ($legacyReport -notmatch '"strict"\s*:\s*false') {
        throw "expected legacy type mode in check JSON"
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
    Write-Text (Join-Path $bad "src/bad_pkg.sura") @"
if true then
  print "missing end"
"@
    $badJson = Join-Path $temp "bad-check.json"
    $badResult = Run-Pkg -PkgArgs @("check", $bad, "--json", $badJson)
    if ($badResult.Code -eq 0 -or
        $badResult.Output -notmatch "\[FAIL\]" -or
        $badResult.Output -notmatch "Sura check: 0 passed, 1 failed" -or
        -not (Test-Path -LiteralPath $badJson)) {
        Write-Output $badResult.Output
        throw "expected broken package check to fail and write JSON"
    }
    $badReport = [System.IO.File]::ReadAllText($badJson, [System.Text.Encoding]::UTF8)
    if ($badReport -notmatch '"failed"\s*:\s*1' -or
        $badReport -notmatch '"status"\s*:\s*"fail"') {
        throw "expected failing check JSON status"
    }

    "pkg_check_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
