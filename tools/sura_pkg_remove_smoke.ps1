param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_remove_" + [System.Guid]::NewGuid().ToString("N"))
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
    $lib = Join-Path $temp "remove_lib"
    $app = Join-Path $temp "remove_app"

    Write-Text (Join-Path $lib "sura.pkg.json") @"
{
  "name": "remove_lib",
  "version": "1.0.0",
  "main": "src/remove_lib.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $lib "src/remove_lib.sura") "func remove_lib_marker do`n  return `"remove`"`nend`n"

    New-Item -ItemType Directory -Force -Path $app | Out-Null
    Write-Text (Join-Path $app "sura.pkg.json") @"
{
  "name": "remove_app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $app "src/app.sura") "print `"remove smoke`"`n"

    Push-Location $app
    try {
        $install = Run-Pkg -PkgArgs @("install", $lib)
        if ($install.Code -ne 0 -or -not (Test-Path (Join-Path $app "packages/remove_lib"))) {
            Write-Output $install.Output
            throw "expected remove_lib install to pass"
        }

        $manifestBefore = Get-Content -Raw -Path (Join-Path $app "sura.pkg.json")
        if ($manifestBefore -notmatch '"remove_lib"') {
            Write-Output $manifestBefore
            throw "expected install to record remove_lib dependency"
        }

        $removeReportPath = Join-Path $temp "remove-report.json"
        $remove = Run-Pkg -PkgArgs @("remove", "remove_lib", "--json", $removeReportPath)
        if ($remove.Code -ne 0 -or
            -not (Test-Path $removeReportPath) -or
            $remove.Output -notmatch "removed remove_lib") {
            Write-Output $remove.Output
            throw "expected remove --json to remove installed package"
        }
        $removeReport = Get-Content -Raw -Path $removeReportPath | ConvertFrom-Json
        if ($removeReport.schema -ne "sura.package.remove.v1" -or
            $removeReport.package -ne "remove_lib" -or
            $removeReport.passed -ne $true -or
            $removeReport.installed -ne $true -or
            $removeReport.removed_entries -le 0 -or
            $removeReport.dependency_removed -ne $true -or
            $removeReport.path -notmatch "packages/remove_lib") {
            $removeReport | ConvertTo-Json -Depth 6 | Write-Output
            throw "expected remove JSON report to capture installed package removal"
        }
        if (Test-Path (Join-Path $app "packages/remove_lib")) {
            throw "expected remove_lib package directory to be deleted"
        }
        $manifestAfter = Get-Content -Raw -Path (Join-Path $app "sura.pkg.json")
        if ($manifestAfter -match '"remove_lib"') {
            Write-Output $manifestAfter
            throw "expected remove to delete remove_lib dependency from manifest"
        }

        $missingReportPath = Join-Path $temp "remove-missing-report.json"
        $missing = Run-Pkg -PkgArgs @("remove", "missing_lib", "--json", $missingReportPath)
        if ($missing.Code -ne 0 -or
            -not (Test-Path $missingReportPath) -or
            $missing.Output -notmatch "missing_lib was not installed") {
            Write-Output $missing.Output
            throw "expected removing a missing package to pass with info output"
        }
        $missingReport = Get-Content -Raw -Path $missingReportPath | ConvertFrom-Json
        if ($missingReport.schema -ne "sura.package.remove.v1" -or
            $missingReport.package -ne "missing_lib" -or
            $missingReport.passed -ne $true -or
            $missingReport.installed -ne $false -or
            $missingReport.removed_entries -ne 0 -or
            $missingReport.dependency_removed -ne $false) {
            $missingReport | ConvertTo-Json -Depth 6 | Write-Output
            throw "expected remove JSON report to capture no-op removal"
        }
    }
    finally {
        Pop-Location
    }

    "pkg_remove_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
