param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_restore_lock_" + [System.Guid]::NewGuid().ToString("N"))
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

function Write-Package {
    param(
        [string]$Path,
        [string]$Name,
        [string]$Version,
        [string]$Dependencies = "{}"
    )
    Write-Text (Join-Path $Path "sura.pkg.json") @"
{
  "name": "$Name",
  "version": "$Version",
  "main": "src/$Name.sura",
  "dependencies": $Dependencies
}
"@
    Write-Text (Join-Path $Path "src/$Name.sura") "func ${Name}_marker do`n  return `"$Version`"`nend`n"
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $null

    $base = Join-Path $temp "lock_base"
    Write-Package -Path $base -Name "lock_base" -Version "1.0.0"
    $publishBase = Run-Pkg -PkgArgs @("publish", $base)
    if ($publishBase.Code -ne 0) {
        Write-Output $publishBase.Output
        throw "expected lock_base publish to pass"
    }

    $mid = Join-Path $temp "lock_mid"
    Write-Package -Path $mid -Name "lock_mid" -Version "1.2.0" -Dependencies "{`n    `"lock_base`": `">=1.0.0 <2.0.0`"`n  }"
    $publishMid = Run-Pkg -PkgArgs @("publish", $mid)
    if ($publishMid.Code -ne 0) {
        Write-Output $publishMid.Output
        throw "expected lock_mid publish to pass"
    }

    $app = Join-Path $temp "lock_app"
    New-Item -ItemType Directory -Force -Path $app | Out-Null
    Write-Text (Join-Path $app "sura.pkg.json") @"
{
  "name": "lock_app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {
    "lock_mid": "^1.0.0"
  }
}
"@
    Write-Text (Join-Path $app "src/app.sura") "print `"restore lock smoke`"`n"

    Push-Location $app
    try {
        $restoreReportPath = Join-Path $temp "restore-report.json"
        $restore = Run-Pkg -PkgArgs @("restore", "--json", $restoreReportPath)
        if ($restore.Code -ne 0 -or
            -not (Test-Path $restoreReportPath) -or
            -not (Test-Path (Join-Path $app "packages/lock_mid")) -or
            -not (Test-Path (Join-Path $app "packages/lock_base"))) {
            Write-Output $restore.Output
            throw "expected restore --json to install direct and transitive dependencies"
        }
        $restoreReport = Get-Content -Raw -Path $restoreReportPath | ConvertFrom-Json
        $restorePackages = @($restoreReport.packages)
        $midReport = @($restorePackages | Where-Object { $_.name -eq "lock_mid" })[0]
        $baseReport = @($restorePackages | Where-Object { $_.name -eq "lock_base" })[0]
        if ($restoreReport.schema -ne "sura.package.restore.v1" -or
            $restoreReport.passed -ne $true -or
            $restoreReport.package_count -ne 2 -or
            $midReport.version -ne "1.2.0" -or
            $midReport.direct -ne $true -or
            $midReport.dependency_spec -ne "^1.0.0" -or
            $midReport.action -ne "installed" -or
            $baseReport.version -ne "1.0.0" -or
            $baseReport.direct -ne $false -or
            $baseReport.action -ne "installed") {
            $restoreReport | ConvertTo-Json -Depth 10 | Write-Output
            throw "expected restore JSON report to describe direct and transitive installs"
        }

        $restoreAgainPath = Join-Path $temp "restore-again-report.json"
        $restoreAgain = Run-Pkg -PkgArgs @("restore", "--json", $restoreAgainPath)
        if ($restoreAgain.Code -ne 0 -or -not (Test-Path $restoreAgainPath)) {
            Write-Output $restoreAgain.Output
            throw "expected second restore --json to pass"
        }
        $restoreAgainReport = Get-Content -Raw -Path $restoreAgainPath | ConvertFrom-Json
        $actions = @($restoreAgainReport.packages | ForEach-Object { $_.action })
        if ($restoreAgainReport.passed -ne $true -or $actions -contains "installed") {
            $restoreAgainReport | ConvertTo-Json -Depth 10 | Write-Output
            throw "expected second restore to report already installed dependencies"
        }

        $lockReportPath = Join-Path $temp "lock-report.json"
        $lock = Run-Pkg -PkgArgs @("lock", "--json", $lockReportPath)
        if ($lock.Code -ne 0 -or
            -not (Test-Path $lockReportPath) -or
            -not (Test-Path (Join-Path $app "sura.lock.json"))) {
            Write-Output $lock.Output
            throw "expected lock --json to write lockfile and report"
        }
        $lockReport = Get-Content -Raw -Path $lockReportPath | ConvertFrom-Json
        $lockPackages = @($lockReport.packages)
        $lockMid = @($lockPackages | Where-Object { $_.name -eq "lock_mid" })[0]
        $lockBase = @($lockPackages | Where-Object { $_.name -eq "lock_base" })[0]
        if ($lockReport.schema -ne "sura.package.lock.v1" -or
            $lockReport.passed -ne $true -or
            $lockReport.package_count -ne 2 -or
            -not $lockMid.hash -or
            -not $lockBase.hash -or
            $lockMid.spec -notmatch "\^1\.0\.0" -or
            $lockBase.spec -notmatch ">=1\.0\.0 <2\.0\.0") {
            $lockReport | ConvertTo-Json -Depth 10 | Write-Output
            throw "expected lock JSON report to include resolved package hashes and specs"
        }

        $verifyReportPath = Join-Path $temp "verify-report.json"
        $verify = Run-Pkg -PkgArgs @("verify", "--json", $verifyReportPath)
        if ($verify.Code -ne 0 -or -not (Test-Path $verifyReportPath)) {
            Write-Output $verify.Output
            throw "expected verify --json to pass against lockfile"
        }
        $verifyReport = Get-Content -Raw -Path $verifyReportPath | ConvertFrom-Json
        $statuses = @($verifyReport.packages | ForEach-Object { $_.status })
        if ($verifyReport.schema -ne "sura.package.verify.v1" -or
            $verifyReport.mode -ne "lockfile" -or
            $verifyReport.passed -ne $true -or
            $verifyReport.package_count -ne 2 -or
            ($statuses | Where-Object { $_ -ne "ok" }).Count -ne 0) {
            $verifyReport | ConvertTo-Json -Depth 10 | Write-Output
            throw "expected verify JSON report to validate every locked package"
        }
    }
    finally {
        Pop-Location
    }

    "pkg_restore_lock_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
