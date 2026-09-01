param(
    [string]$Surapkg = ".\surapkg.exe",
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_run_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$oldEngine = $env:SURA_ENGINE

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
    $env:SURA_ENGINE = $EnginePath
    $pkg = Join-Path $temp "run_pkg"
    $ko = -join ([char[]](0xD55C, 0xAE00))

    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "run_pkg",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {}
}
"@

    Write-Text (Join-Path $pkg "src/app.sura") @"
args is argv()
assert_eq(path_basename(script_name()), "app.sura")
assert_eq(argc(), 2)
assert_eq(args[0], "--mode=fast")
assert_eq(args[1], "$ko")
file_write("run.out", args[1])
print "pkg_run_smoke: PASS " + args[1]
"@

    $runJson = Join-Path $temp "run-report.json"
    $run = Run-Pkg -PkgArgs @("run", $pkg, "--json", $runJson, "--", "--mode=fast", $ko)
    if ($run.Code -ne 0 -or $run.Output -notmatch "pkg_run_smoke: PASS") {
        Write-Output $run.Output
        throw "expected surapkg run to execute package main"
    }
    if (-not (Test-Path -LiteralPath $runJson)) {
        throw "expected surapkg run --json to write a report"
    }
    $runReport = Get-Content -Raw -Encoding UTF8 -Path $runJson | ConvertFrom-Json
    if ($runReport.schema -ne "sura.package.run.v1" -or
        -not $runReport.passed -or
        $runReport.package -ne "run_pkg" -or
        $runReport.version -ne "0.1.0" -or
        -not $runReport.jit -or
        $runReport.exitCode -ne 0 -or
        $runReport.args.Count -ne 2 -or
        $runReport.args[0] -ne "--mode=fast" -or
        $runReport.args[1] -ne $ko -or
        $runReport.output -notmatch "pkg_run_smoke: PASS") {
        Write-Output (Get-Content -Raw -Encoding UTF8 -Path $runJson)
        throw "expected surapkg run JSON report to expose execution metadata"
    }

    $outPath = Join-Path $pkg "run.out"
    if (-not (Test-Path -LiteralPath $outPath)) {
        throw "expected surapkg run to execute from package root"
    }
    $written = [System.IO.File]::ReadAllText($outPath, [System.Text.Encoding]::UTF8)
    if ($written -ne $ko) {
        throw "expected surapkg run to preserve UTF-8 script arguments"
    }

    Write-Text (Join-Path $pkg "src/app.sura") "assert(false)`n"
    $badJson = Join-Path $temp "run-fail-report.json"
    $badRun = Run-Pkg -PkgArgs @("run", $pkg, "--no-jit", "--json=$badJson", "--", "failing")
    if ($badRun.Code -eq 0 -or -not (Test-Path -LiteralPath $badJson)) {
        Write-Output $badRun.Output
        throw "expected failing surapkg run --json to return nonzero and still write a report"
    }
    $badReport = Get-Content -Raw -Encoding UTF8 -Path $badJson | ConvertFrom-Json
    if ($badReport.schema -ne "sura.package.run.v1" -or
        $badReport.passed -or
        $badReport.jit -or
        $badReport.exitCode -eq 0 -or
        $badReport.args[0] -ne "failing") {
        Write-Output (Get-Content -Raw -Encoding UTF8 -Path $badJson)
        throw "expected failing surapkg run JSON report to capture failure metadata"
    }

    $help = Run-Pkg -PkgArgs @("run", "--help")
    if ($help.Code -ne 0 -or $help.Output -notmatch "surapkg run" -or $help.Output -notmatch "--json run\.json") {
        Write-Output $help.Output
        throw "expected surapkg run help to mention JSON reports"
    }

    "pkg_run_smoke: PASS"
}
finally {
    if ($null -eq $oldEngine) {
        Remove-Item Env:SURA_ENGINE -ErrorAction SilentlyContinue
    } else {
        $env:SURA_ENGINE = $oldEngine
    }
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# Verified passing; state the exit code rather than inheriting it.
exit 0
