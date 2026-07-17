param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_profile_" + [System.Guid]::NewGuid().ToString("N"))
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

    $pkg = Join-Path $temp "profile_pkg"
    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "profile_pkg",
  "version": "1.0.0",
  "main": "src/profile_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/profile_pkg.sura") @"
func add(a, b) do
  return a + b
end

total is 0
i is 0
while i < 80 do
  total is add(total, i)
  if total > -1 then
    total is total + 1
  end
  i is i + 1
end

print total
"@

    $json = Join-Path $temp "profile.json"
    $profile = Run-Pkg -PkgArgs @("profile", $pkg, "--json", $json, "--", "--mode=fast")
    if ($profile.Code -ne 0 -or
        $profile.Output -notmatch "Sura Runtime Profile Report" -or
        $profile.Output -notmatch "\[profile\] wrote" -or
        $profile.Output -notmatch "profiled package main" -or
        -not (Test-Path -LiteralPath $json)) {
        Write-Output $profile.Output
        throw "expected surapkg profile to write profile JSON and print report"
    }

    $report = Get-Content -Raw -LiteralPath $json | ConvertFrom-Json
    if ($report.summary.arithmetic_sites -lt 1) {
        throw "expected arithmetic profile sites"
    }
    if ($report.summary.branch_sites -lt 1) {
        throw "expected branch profile sites"
    }
    if ($report.summary.call_sites -lt 1) {
        throw "expected call profile sites"
    }

    $textOnly = Run-Pkg -PkgArgs @("profile", $pkg, "--no-jit")
    if ($textOnly.Code -ne 0 -or
        $textOnly.Output -notmatch "Sura Runtime Profile Report" -or
        $textOnly.Output -notmatch "profiled package main") {
        Write-Output $textOnly.Output
        throw "expected text-only package profile to pass"
    }

    "pkg_profile_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
