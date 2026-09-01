param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_version_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Pkg {
    param([string[]]$PkgArgs, [string]$WorkDir = $temp)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location $WorkDir
    try {
        $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        $ErrorActionPreference = $old
    }
    return @{ Code = $code; Output = ($out -join "`n") }
}

function Read-Json {
    param([string]$Path)
    return Get-Content -Raw -Encoding UTF8 -LiteralPath $Path | ConvertFrom-Json
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $pkg = Join-Path $temp "version_pkg"
    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "version_pkg",
  "version": "1.2.3",
  "main": "src/version_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/version_pkg.sura") "print `"version smoke`"`n"

    $currentReport = Join-Path $temp "version-current.json"
    $current = Run-Pkg -WorkDir $pkg -PkgArgs @("version", "--json", $currentReport)
    if ($current.Code -ne 0 -or $current.Output -notmatch "version_pkg version 1\.2\.3") {
        Write-Output $current.Output
        throw "expected current version query to pass"
    }
    $currentJson = Read-Json $currentReport
    if ($currentJson.schema -ne "sura.package.version.v1" -or
        $currentJson.package -ne "version_pkg" -or
        $currentJson.previous_version -ne "1.2.3" -or
        $currentJson.version -ne "1.2.3" -or
        $currentJson.mode -ne "current" -or
        $currentJson.changed -ne $false) {
        $currentJson | ConvertTo-Json -Depth 6 | Write-Output
        throw "unexpected current version JSON report"
    }

    $patchReport = Join-Path $temp "version-patch.json"
    $patch = Run-Pkg -PkgArgs @("version", $pkg, "patch", "--json", $patchReport)
    if ($patch.Code -ne 0 -or $patch.Output -notmatch "updated version_pkg 1\.2\.3 -> 1\.2\.4") {
        Write-Output $patch.Output
        throw "expected patch bump to pass"
    }
    $patchJson = Read-Json $patchReport
    if ($patchJson.previous_version -ne "1.2.3" -or $patchJson.version -ne "1.2.4" -or $patchJson.mode -ne "patch" -or $patchJson.changed -ne $true) {
        $patchJson | ConvertTo-Json -Depth 6 | Write-Output
        throw "unexpected patch version report"
    }

    $minorReport = Join-Path $temp "version-minor.json"
    $minor = Run-Pkg -WorkDir $pkg -PkgArgs @("version", "minor", "--json=$minorReport")
    if ($minor.Code -ne 0 -or $minor.Output -notmatch "updated version_pkg 1\.2\.4 -> 1\.3\.0") {
        Write-Output $minor.Output
        throw "expected minor bump to pass"
    }
    $minorJson = Read-Json $minorReport
    if ($minorJson.previous_version -ne "1.2.4" -or $minorJson.version -ne "1.3.0" -or $minorJson.mode -ne "minor" -or $minorJson.changed -ne $true) {
        $minorJson | ConvertTo-Json -Depth 6 | Write-Output
        throw "unexpected minor version report"
    }

    $majorReport = Join-Path $temp "version-major.json"
    $major = Run-Pkg -PkgArgs @("version", $pkg, "major", "--json", $majorReport)
    if ($major.Code -ne 0 -or $major.Output -notmatch "updated version_pkg 1\.3\.0 -> 2\.0\.0") {
        Write-Output $major.Output
        throw "expected major bump to pass"
    }
    $majorJson = Read-Json $majorReport
    if ($majorJson.previous_version -ne "1.3.0" -or $majorJson.version -ne "2.0.0" -or $majorJson.mode -ne "major" -or $majorJson.changed -ne $true) {
        $majorJson | ConvertTo-Json -Depth 6 | Write-Output
        throw "unexpected major version report"
    }

    $setReport = Join-Path $temp "version-set.json"
    $set = Run-Pkg -PkgArgs @("version", $pkg, "2.5.7-beta_1", "--json", $setReport)
    if ($set.Code -ne 0 -or $set.Output -notmatch "updated version_pkg 2\.0\.0 -> 2\.5\.7-beta_1") {
        Write-Output $set.Output
        throw "expected explicit version set to pass"
    }
    $setJson = Read-Json $setReport
    if ($setJson.previous_version -ne "2.0.0" -or $setJson.version -ne "2.5.7-beta_1" -or $setJson.mode -ne "set" -or $setJson.changed -ne $true) {
        $setJson | ConvertTo-Json -Depth 6 | Write-Output
        throw "unexpected explicit version report"
    }

    $sameReport = Join-Path $temp "version-same.json"
    $same = Run-Pkg -PkgArgs @("version", $pkg, "2.5.7-beta_1", "--json", $sameReport)
    if ($same.Code -ne 0 -or $same.Output -notmatch "version_pkg version 2\.5\.7-beta_1") {
        Write-Output $same.Output
        throw "expected setting the same version to be a no-op"
    }
    $sameJson = Read-Json $sameReport
    if ($sameJson.previous_version -ne "2.5.7-beta_1" -or $sameJson.version -ne "2.5.7-beta_1" -or $sameJson.mode -ne "set" -or $sameJson.changed -ne $false) {
        $sameJson | ConvertTo-Json -Depth 6 | Write-Output
        throw "unexpected no-op set version report"
    }
    $manifest = Read-Json (Join-Path $pkg "sura.pkg.json")
    if ($manifest.version -ne "2.5.7-beta_1") {
        $manifest | ConvertTo-Json -Depth 6 | Write-Output
        throw "expected manifest version to match final explicit version"
    }

    $missingVersionPkg = Join-Path $temp "missing_version_pkg"
    Write-Text (Join-Path $missingVersionPkg "sura.pkg.json") @"
{
  "name": "missing_version_pkg",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $missingVersionPkg "src/main.sura") "print `"missing version`"`n"
    $insertReport = Join-Path $temp "version-insert.json"
    $insert = Run-Pkg -PkgArgs @("version", $missingVersionPkg, "0.1.0", "--json", $insertReport)
    if ($insert.Code -ne 0 -or $insert.Output -notmatch "updated missing_version_pkg  -> 0\.1\.0") {
        Write-Output $insert.Output
        throw "expected explicit version to be inserted when manifest has no version"
    }
    $insertJson = Read-Json $insertReport
    $insertManifest = Read-Json (Join-Path $missingVersionPkg "sura.pkg.json")
    if ($insertJson.previous_version -ne "" -or $insertJson.version -ne "0.1.0" -or $insertJson.mode -ne "set" -or
        $insertJson.changed -ne $true -or $insertManifest.version -ne "0.1.0") {
        $insertJson | ConvertTo-Json -Depth 6 | Write-Output
        Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $missingVersionPkg "sura.pkg.json") | Write-Output
        throw "expected missing version insertion report and manifest update"
    }

    $invalid = Run-Pkg -PkgArgs @("version", $pkg, "latest")
    if ($invalid.Code -eq 0 -or $invalid.Output -notmatch "invalid package version") {
        Write-Output $invalid.Output
        throw "expected invalid literal version to fail"
    }

    $badBumpPkg = Join-Path $temp "bad_bump_pkg"
    Write-Text (Join-Path $badBumpPkg "sura.pkg.json") @"
{
  "name": "bad_bump_pkg",
  "version": "1.2.beta",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $badBumpPkg "src/main.sura") "print `"bad bump`"`n"
    $badBump = Run-Pkg -PkgArgs @("version", $badBumpPkg, "patch")
    if ($badBump.Code -eq 0 -or $badBump.Output -notmatch "not numeric enough to bump") {
        Write-Output $badBump.Output
        throw "expected non-numeric patch bump to fail"
    }

    $missingManifest = Join-Path $temp "not_a_package"
    New-Item -ItemType Directory -Force -Path $missingManifest | Out-Null
    $missing = Run-Pkg -PkgArgs @("version", $missingManifest)
    if ($missing.Code -eq 0 -or $missing.Output -notmatch "sura.pkg.json not found") {
        Write-Output $missing.Output
        throw "expected version outside package to fail"
    }

    "pkg_version_smoke: PASS"
} finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# This gate printed PASS while exiting 1: its last native command was a
# negative check that correctly failed, and the script inherited that
# code. CI reads the exit code, so a passing gate looked like a failure.
# Only reached on the success path - a throw terminates before here.
exit 0
