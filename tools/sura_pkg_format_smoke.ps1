param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_format_" + [System.Guid]::NewGuid().ToString("N"))
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
    Write-Text (Join-Path $good "src/good_pkg.sura") "func add do`n  return 2 + 3`nend`n"
    $goodJson = Join-Path $temp "good-format.json"
    $goodResult = Run-Pkg -PkgArgs @("format", $good, "--check", "--json", $goodJson)
    if ($goodResult.Code -ne 0 -or
        $goodResult.Output -notmatch "Sura format check: 1 passed, 0 failed" -or
        -not (Test-Path -LiteralPath $goodJson)) {
        Write-Output $goodResult.Output
        throw "expected formatted package check to pass and write JSON"
    }
    $goodReport = [System.IO.File]::ReadAllText($goodJson, [System.Text.Encoding]::UTF8)
    if ($goodReport -notmatch '"passed"\s*:\s*true' -or
        $goodReport -notmatch '"check"\s*:\s*true' -or
        $goodReport -notmatch '"unchanged"\s*:\s*1') {
        throw "expected formatted package JSON counts"
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
    Write-Text (Join-Path $bad "src/bad_pkg.sura") "if true then`nprint `"format me`"`nend`n"
    $badCheckJson = Join-Path $temp "bad-format-check.json"
    $badCheck = Run-Pkg -PkgArgs @("format", $bad, "--check", "--json", $badCheckJson)
    if ($badCheck.Code -eq 0 -or
        $badCheck.Output -notmatch "\[FAIL\]" -or
        $badCheck.Output -notmatch "Sura format check: 0 passed, 1 failed" -or
        -not (Test-Path -LiteralPath $badCheckJson)) {
        Write-Output $badCheck.Output
        throw "expected unformatted package check to fail and write JSON"
    }

    $badWriteJson = Join-Path $temp "bad-format-write.json"
    $formatResult = Run-Pkg -PkgArgs @("format", $bad, "--json", $badWriteJson)
    if ($formatResult.Code -ne 0 -or
        $formatResult.Output -notmatch "Sura format: 1 formatted, 0 unchanged, 0 failed" -or
        -not (Test-Path -LiteralPath $badWriteJson)) {
        Write-Output $formatResult.Output
        throw "expected format write to rewrite package and write JSON"
    }
    $formatted = [System.IO.File]::ReadAllText((Join-Path $bad "src/bad_pkg.sura"), [System.Text.Encoding]::UTF8)
    if ($formatted -notmatch '  print "format me"') {
        throw "expected formatted indentation"
    }
    $writeReport = [System.IO.File]::ReadAllText($badWriteJson, [System.Text.Encoding]::UTF8)
    if ($writeReport -notmatch '"formatted"\s*:\s*1' -or
        $writeReport -notmatch '"check"\s*:\s*false') {
        throw "expected format write JSON counts"
    }

    $finalCheck = Run-Pkg -PkgArgs @("format", $bad, "--check")
    if ($finalCheck.Code -ne 0 -or
        $finalCheck.Output -notmatch "Sura format check: 1 passed, 0 failed") {
        Write-Output $finalCheck.Output
        throw "expected final format check to pass"
    }

    "pkg_format_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
