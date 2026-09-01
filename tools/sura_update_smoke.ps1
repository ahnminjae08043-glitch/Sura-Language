param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_update_" + [System.Guid]::NewGuid().ToString("N"))
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

function Write-PackageVersion {
    param([string]$Path, [string]$Version, [string]$Marker)
    Write-Text (Join-Path $Path "sura.pkg.json") @"
{
  "name": "math_extra",
  "version": "$Version",
  "main": "src/math_extra.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $Path "src/math_extra.sura") @"
func version_marker do
  return "$Marker"
end
"@
}

function Write-NamedPackage {
    param(
        [string]$Path,
        [string]$Name,
        [string]$Version,
        [string]$Marker,
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
    Write-Text (Join-Path $Path "src/$Name.sura") @"
func ${Name}_marker do
  return "$Marker"
end
"@
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
$oldAllowCritical = $env:SURA_ALLOW_CRITICAL_ADVISORY_INSTALL
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $pkg = Join-Path $temp "math_extra"
    $project = Join-Path $temp "app"
    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $null

    Write-PackageVersion -Path $pkg -Version "0.1.0" -Marker "v1"
    $publishOld = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($publishOld.Code -ne 0) {
        Write-Output $publishOld.Output
        throw "expected publish 0.1.0 to pass"
    }

    Write-PackageVersion -Path $pkg -Version "0.2.0" -Marker "v2"
    $publishNew = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($publishNew.Code -ne 0) {
        Write-Output $publishNew.Output
        throw "expected publish 0.2.0 to pass"
    }
    $indexPath = Join-Path $registry "index.json"
    $index = Get-Content -Raw -Path $indexPath
    $index = $index -replace "`n  ]`n}`n?$", ",`n    {`"name`":`"math_extra`",`"version`":`"0.3.0`",`"bundle`":`"math_extra/0.3.0/package.surabundle.json`",`"hash`":`"yanked`",`"yanked`":true}`n  ]`n}`n"
    Write-Text $indexPath $index

    $basePkg = Join-Path $temp "base_lib"
    Write-NamedPackage -Path $basePkg -Name "base_lib" -Version "1.0.0" -Marker "base1"
    $publishBase = Run-Pkg -PkgArgs @("publish", $basePkg)
    if ($publishBase.Code -ne 0) {
        Write-Output $publishBase.Output
        throw "expected publish base_lib 1.0.0 to pass"
    }

    Write-NamedPackage -Path $basePkg -Name "base_lib" -Version "2.0.0" -Marker "base2"
    $publishBase2 = Run-Pkg -PkgArgs @("publish", $basePkg)
    if ($publishBase2.Code -ne 0) {
        Write-Output $publishBase2.Output
        throw "expected publish base_lib 2.0.0 to pass"
    }

    $midPkg = Join-Path $temp "mid_lib"
    Write-NamedPackage -Path $midPkg -Name "mid_lib" -Version "1.0.0" -Marker "mid1" -Dependencies "{`n    `"base_lib`": `">=1.0.0 <2.0.0`"`n  }"
    $publishMid = Run-Pkg -PkgArgs @("publish", $midPkg)
    if ($publishMid.Code -ne 0) {
        Write-Output $publishMid.Output
        throw "expected publish mid_lib 1.0.0 to pass"
    }

    $otherPkg = Join-Path $temp "other_mid"
    Write-NamedPackage -Path $otherPkg -Name "other_mid" -Version "1.0.0" -Marker "other1" -Dependencies "{`n    `"base_lib`": `">=2.0.0 <3.0.0`"`n  }"
    $publishOther = Run-Pkg -PkgArgs @("publish", $otherPkg)
    if ($publishOther.Code -ne 0) {
        Write-Output $publishOther.Output
        throw "expected publish other_mid 1.0.0 to pass"
    }

    $dangerPkg = Join-Path $temp "danger_pkg"
    Write-NamedPackage -Path $dangerPkg -Name "danger_pkg" -Version "1.0.0" -Marker "danger"
    $publishDanger = Run-Pkg -PkgArgs @("publish", $dangerPkg)
    if ($publishDanger.Code -ne 0) {
        Write-Output $publishDanger.Output
        throw "expected publish danger_pkg 1.0.0 to pass"
    }
    Write-Text (Join-Path $registry "advisories.json") @"
{
  "advisories": [
    {"id":"adv-math-high","name":"math_extra","version":"0.1.0","severity":"high","status":"active","title":"legacy test advisory","description":"install should warn but allow non-critical advisories"},
    {"id":"adv-danger-critical","name":"danger_pkg","version":"1.0.0","severity":"critical","status":"active","title":"blocked test advisory","description":"install should block critical advisories"}
  ]
}
"@
    $criticalAdvisoriesJson = Run-Pkg -PkgArgs @("advisories", "danger_pkg@1.0.0", "--severity", "critical", "--json")
    if ($criticalAdvisoriesJson.Code -ne 0) {
        Write-Output $criticalAdvisoriesJson.Output
        throw "expected critical advisories --json to pass"
    }
    $criticalAdvisoriesReport = $criticalAdvisoriesJson.Output | ConvertFrom-Json
    $criticalActions = @($criticalAdvisoriesReport.next_actions)
    if ($criticalAdvisoriesReport.schema -ne "sura.registry.advisories.v1" -or
        $criticalAdvisoriesReport.count -ne 1 -or
        $criticalActions.Count -ne 1 -or
        $criticalActions[0].advisory_id -ne "adv-danger-critical" -or
        $criticalActions[0].target -ne "danger_pkg@1.0.0" -or
        $criticalActions[0].severity -ne "critical" -or
        $criticalActions[0].action -notmatch "block installs/updates" -or
        $criticalActions[0].action -notmatch "surapkg audit") {
        $criticalAdvisoriesReport | ConvertTo-Json -Depth 6
        throw "expected critical advisories --json to include blocking next_actions"
    }
    $criticalGate = Run-Pkg -PkgArgs @("advisories", "danger_pkg@1.0.0", "--fail-on", "critical", "--json")
    if ($criticalGate.Code -eq 0) {
        Write-Output $criticalGate.Output
        throw "expected advisories --fail-on critical to fail on active critical advisory"
    }
    $criticalGateReport = $criticalGate.Output | ConvertFrom-Json
    if ($criticalGateReport.passed -ne $false -or
        $criticalGateReport.fail_on -ne "critical" -or
        $criticalGateReport.failing_count -ne 1 -or
        $criticalGateReport.count -ne 1 -or
        @($criticalGateReport.next_actions).Count -ne 1) {
        $criticalGateReport | ConvertTo-Json -Depth 6
        throw "expected advisories --fail-on critical JSON gate report"
    }

    New-Item -ItemType Directory -Force -Path $project | Out-Null
    Push-Location $project
    try {
        Write-Text (Join-Path $project "sura.pkg.json") @"
{
  "name": "app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {}
}
"@
        Write-Text (Join-Path $project "src/app.sura") "print `"app`"`n"

        $installReportPath = Join-Path $temp "install-report.json"
        $installOld = Run-Pkg -PkgArgs @("install", "math_extra@0.1.0", "--json", $installReportPath)
        if ($installOld.Code -ne 0 -or $installOld.Output -notmatch "registry advisory high") {
            Write-Output $installOld.Output
            throw "expected install 0.1.0 to warn and pass with high advisory"
        }
        $blockedInstall = Run-Pkg -PkgArgs @("install", "danger_pkg@1.0.0")
        if ($blockedInstall.Code -eq 0 -or
            $blockedInstall.Output -notmatch "registry advisory critical" -or
            $blockedInstall.Output -notmatch "install blocked by active critical registry advisory") {
            Write-Output $blockedInstall.Output
            throw "expected install to block active critical advisory"
        }
        if (Test-Path -Path (Join-Path $project "packages/danger_pkg")) {
            throw "expected critical advisory block to remove extracted package"
        }
        $installReport = Get-Content -Raw -Path $installReportPath | ConvertFrom-Json
        if ($installReport.schema -ne "sura.package.install.v1" -or
            $installReport.requested -ne "math_extra@0.1.0" -or
            $installReport.package -ne "math_extra" -or
            $installReport.version -ne "0.1.0" -or
            $installReport.passed -ne $true -or
            $installReport.from_registry -ne $true -or
            $installReport.remote -ne $false -or
            $installReport.dependency_recorded -ne $true -or
            -not (Test-Path -Path $installReport.destination)) {
            throw "expected install --json to report installed registry package"
        }
        $manifestPath = Join-Path $project "sura.pkg.json"
        $projectManifest = Get-Content -Raw -Path $manifestPath
        $projectManifest = $projectManifest -replace 'registry:math_extra@0\.1\.0', '>=0.1.0 <0.3.0'
        Write-Text $manifestPath $projectManifest

        $outdated = Run-Pkg -PkgArgs @("outdated")
        if ($outdated.Code -ne 0 -or $outdated.Output -notmatch "math_extra\s+0\.1\.0\s+0\.2\.0") {
            Write-Output $outdated.Output
            throw "expected outdated to show math_extra update"
        }
        $outdatedJson = Run-Pkg -PkgArgs @("outdated", "--json")
        if ($outdatedJson.Code -ne 0) {
            Write-Output $outdatedJson.Output
            throw "expected outdated --json to pass"
        }
        $outdatedReport = $outdatedJson.Output | ConvertFrom-Json
        if ($outdatedReport.schema -ne "sura.package.outdated.v1" -or
            $outdatedReport.up_to_date -ne $false -or
            $outdatedReport.count -ne 1 -or
            $outdatedReport.packages[0].name -ne "math_extra" -or
            $outdatedReport.packages[0].current -ne "0.1.0" -or
            $outdatedReport.packages[0].latest -ne "0.2.0" -or
            $outdatedReport.packages[0].dependency_spec -ne ">=0.1.0 <0.3.0") {
            throw "expected outdated --json to report math_extra update"
        }

        $updateReportPath = Join-Path $temp "update-report.json"
        $update = Run-Pkg -PkgArgs @("update", "math_extra", "--json", $updateReportPath)
        if ($update.Code -ne 0 -or $update.Output -notmatch "updated 1 package") {
            Write-Output $update.Output
            throw "expected update to install latest math_extra"
        }
        $updateReport = Get-Content -Raw -Path $updateReportPath | ConvertFrom-Json
        if ($updateReport.schema -ne "sura.package.update.v1" -or
            $updateReport.query -ne "math_extra" -or
            $updateReport.passed -ne $true -or
            $updateReport.updated_count -ne 1 -or
            $updateReport.failed_count -ne 0 -or
            $updateReport.packages[0].name -ne "math_extra" -or
            $updateReport.packages[0].previous -ne "0.1.0" -or
            $updateReport.packages[0].installed -ne "0.2.0" -or
            $updateReport.packages[0].dependency_spec -ne ">=0.1.0 <0.3.0") {
            throw "expected update --json to report applied package update"
        }

        $installedManifest = Get-Content -Raw -Path (Join-Path $project "packages/math_extra/sura.pkg.json")
        $installedSource = Get-Content -Raw -Path (Join-Path $project "packages/math_extra/src/math_extra.sura")
        $projectManifest = Get-Content -Raw -Path (Join-Path $project "sura.pkg.json")
        if ($installedManifest -notmatch '"version"\s*:\s*"0\.2\.0"' -or $installedSource -notmatch "v2" -or $projectManifest -notmatch ">=0\.1\.0 <0\.3\.0") {
            throw "expected update to write version 0.2.0 and preserve dependency constraint"
        }

        $clean = Run-Pkg -PkgArgs @("outdated", "math_extra")
        if ($clean.Code -ne 0 -or $clean.Output -notmatch "All packages are up to date") {
            Write-Output $clean.Output
            throw "expected package to be up to date after update"
        }
        $cleanJson = Run-Pkg -PkgArgs @("outdated", "math_extra", "--json")
        if ($cleanJson.Code -ne 0) {
            Write-Output $cleanJson.Output
            throw "expected filtered outdated --json to pass after update"
        }
        $cleanReport = $cleanJson.Output | ConvertFrom-Json
        if ($cleanReport.schema -ne "sura.package.outdated.v1" -or
            $cleanReport.query -ne "math_extra" -or
            $cleanReport.up_to_date -ne $true -or
            $cleanReport.count -ne 0 -or
            ($null -ne $cleanReport.packages -and @($cleanReport.packages).Count -ne 0)) {
            throw "expected filtered outdated --json to report up-to-date package"
        }
    }
    finally {
        Pop-Location
    }

    $advisoryAuditProject = Join-Path $temp "advisory_audit_app"
    New-Item -ItemType Directory -Force -Path $advisoryAuditProject | Out-Null
    Push-Location $advisoryAuditProject
    try {
        Write-Text (Join-Path $advisoryAuditProject "sura.pkg.json") @"
{
  "name": "advisory_audit_app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {}
}
"@
        Write-Text (Join-Path $advisoryAuditProject "src/app.sura") "print `"advisory audit`"`n"

        $warnInstall = Run-Pkg -PkgArgs @("install", "math_extra@0.1.0")
        if ($warnInstall.Code -ne 0 -or $warnInstall.Output -notmatch "registry advisory high") {
            Write-Output $warnInstall.Output
            throw "expected non-critical advisory install to warn and pass"
        }
        $warnAudit = Run-Pkg -PkgArgs @("audit", ".")
        if ($warnAudit.Code -ne 0 -or $warnAudit.Output -notmatch "dependency advisory high") {
            Write-Output $warnAudit.Output
            throw "expected audit to warn but pass on high dependency advisory"
        }

        $env:SURA_ALLOW_CRITICAL_ADVISORY_INSTALL = "1"
        $criticalInstall = Run-Pkg -PkgArgs @("install", "danger_pkg@1.0.0")
        $env:SURA_ALLOW_CRITICAL_ADVISORY_INSTALL = $oldAllowCritical
        if ($criticalInstall.Code -ne 0 -or $criticalInstall.Output -notmatch "registry advisory critical") {
            Write-Output $criticalInstall.Output
            throw "expected override install to warn and pass for critical advisory"
        }
        $criticalAuditReportPath = Join-Path $temp "critical-advisory-audit.json"
        $criticalAudit = Run-Pkg -PkgArgs @("audit", ".", "--json", $criticalAuditReportPath)
        if ($criticalAudit.Code -eq 0 -or
            $criticalAudit.Output -notmatch "dependency advisory critical" -or
            -not (Test-Path $criticalAuditReportPath)) {
            Write-Output $criticalAudit.Output
            throw "expected audit to fail on active critical dependency advisory"
        }
        $criticalAuditReport = Get-Content -Raw -Path $criticalAuditReportPath | ConvertFrom-Json
        if ($criticalAuditReport.passed -ne $false -or
            $criticalAuditReport.finding_count -lt 1 -or
            -not ($criticalAuditReport.findings | Where-Object { $_.kind -eq "registry_advisory" -and $_.message -match "danger_pkg@1.0.0" })) {
            $criticalAuditReport | ConvertTo-Json -Depth 6
            throw "expected audit JSON to include critical registry advisory finding"
        }
    }
    finally {
        $env:SURA_ALLOW_CRITICAL_ADVISORY_INSTALL = $oldAllowCritical
        Pop-Location
    }

    $restoreProject = Join-Path $temp "restore_app"
    New-Item -ItemType Directory -Force -Path $restoreProject | Out-Null
    Push-Location $restoreProject
    try {
        Write-Text (Join-Path $restoreProject "sura.pkg.json") @"
{
  "name": "restore_app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {
    "math_extra": ">=0.1.0 <0.3.0"
  }
}
"@
        Write-Text (Join-Path $restoreProject "src/app.sura") "print `"restore`"`n"

        $resolve = Run-Pkg -PkgArgs @("resolve")
        if ($resolve.Code -ne 0 -or $resolve.Output -notmatch "math_extra.+0\.2\.0") {
            Write-Output $resolve.Output
            throw "expected resolve to select latest matching constrained version"
        }
        $resolveJson = Run-Pkg -PkgArgs @("resolve", "--json")
        if ($resolveJson.Code -ne 0) {
            Write-Output $resolveJson.Output
            throw "expected resolve --json to pass"
        }
        $resolveReport = $resolveJson.Output | ConvertFrom-Json
        if ($resolveReport.schema -ne "sura.package.resolve.v1" -or
            $resolveReport.ok -ne $true -or
            $resolveReport.root.name -ne "restore_app" -or
            @($resolveReport.packages).Count -ne 1 -or
            $resolveReport.packages[0].name -ne "math_extra" -or
            $resolveReport.packages[0].version -ne "0.2.0" -or
            $resolveReport.packages[0].from_registry -ne $true -or
            $resolveReport.packages[0].requirements[0].spec -ne ">=0.1.0 <0.3.0") {
            throw "expected resolve --json to report constrained math_extra dependency"
        }

        $restoreReportPath = Join-Path $temp "restore-report.json"
        $restore = Run-Pkg -PkgArgs @("restore", "--json", $restoreReportPath)
        if ($restore.Code -ne 0) {
            Write-Output $restore.Output
            throw "expected restore to install constrained dependency"
        }
        $restoreReport = Get-Content -Raw -Path $restoreReportPath | ConvertFrom-Json
        if ($restoreReport.schema -ne "sura.package.restore.v1" -or
            $restoreReport.passed -ne $true -or
            $restoreReport.package_count -ne 1 -or
            $restoreReport.failed_count -ne 0 -or
            $restoreReport.packages[0].name -ne "math_extra" -or
            $restoreReport.packages[0].version -ne "0.2.0" -or
            $restoreReport.packages[0].action -ne "installed" -or
            $restoreReport.packages[0].direct -ne $true -or
            $restoreReport.packages[0].dependency_spec -ne ">=0.1.0 <0.3.0" -or
            $restoreReport.packages[0].from_registry -ne $true -or
            $restoreReport.packages[0].installed_after -ne $true) {
            throw "expected restore --json to report installed constrained dependency"
        }

        $restoredManifest = Get-Content -Raw -Path (Join-Path $restoreProject "packages/math_extra/sura.pkg.json")
        $restoreAppManifest = Get-Content -Raw -Path (Join-Path $restoreProject "sura.pkg.json")
        if ($restoredManifest -notmatch '"version"\s*:\s*"0\.2\.0"' -or $restoreAppManifest -notmatch ">=0\.1\.0 <0\.3\.0") {
            throw "expected restore to install latest matching version and preserve constraint"
        }
    }
    finally {
        Pop-Location
    }

    $transitiveProject = Join-Path $temp "transitive_app"
    New-Item -ItemType Directory -Force -Path $transitiveProject | Out-Null
    Push-Location $transitiveProject
    try {
        Write-Text (Join-Path $transitiveProject "sura.pkg.json") @"
{
  "name": "transitive_app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {
    "mid_lib": "^1.0.0"
  }
}
"@
        Write-Text (Join-Path $transitiveProject "src/app.sura") "print `"transitive`"`n"

        $resolveTransitive = Run-Pkg -PkgArgs @("resolve")
        if ($resolveTransitive.Code -ne 0 -or
            $resolveTransitive.Output -notmatch "mid_lib\s+1\.0\.0" -or
            $resolveTransitive.Output -notmatch "base_lib\s+1\.0\.0") {
            Write-Output $resolveTransitive.Output
            throw "expected resolve to include transitive base_lib 1.0.0"
        }
        $resolveTransitiveJson = Run-Pkg -PkgArgs @("resolve", "--json")
        if ($resolveTransitiveJson.Code -ne 0) {
            Write-Output $resolveTransitiveJson.Output
            throw "expected transitive resolve --json to pass"
        }
        $transitiveReport = $resolveTransitiveJson.Output | ConvertFrom-Json
        $transitiveNames = @($transitiveReport.packages | ForEach-Object { "$($_.name)@$($_.version)" })
        if ($transitiveReport.schema -ne "sura.package.resolve.v1" -or
            $transitiveReport.ok -ne $true -or
            $transitiveNames -notcontains "mid_lib@1.0.0" -or
            $transitiveNames -notcontains "base_lib@1.0.0") {
            throw "expected transitive resolve --json to include direct and transitive packages"
        }

        $restoreTransitiveReportPath = Join-Path $temp "restore-transitive-report.json"
        $restoreTransitive = Run-Pkg -PkgArgs @("restore", "--json", $restoreTransitiveReportPath)
        if ($restoreTransitive.Code -ne 0) {
            Write-Output $restoreTransitive.Output
            throw "expected restore to install transitive dependencies"
        }
        $restoreTransitiveReport = Get-Content -Raw -Path $restoreTransitiveReportPath | ConvertFrom-Json
        $restoreTransitiveNames = @($restoreTransitiveReport.packages | ForEach-Object { "$($_.name)@$($_.version):$($_.direct)" })
        if ($restoreTransitiveReport.schema -ne "sura.package.restore.v1" -or
            $restoreTransitiveReport.passed -ne $true -or
            $restoreTransitiveReport.package_count -ne 2 -or
            $restoreTransitiveReport.failed_count -ne 0 -or
            $restoreTransitiveNames -notcontains "mid_lib@1.0.0:True" -or
            $restoreTransitiveNames -notcontains "base_lib@1.0.0:False") {
            throw "expected restore --json to report direct and transitive packages"
        }
        if (-not (Test-Path (Join-Path $transitiveProject "packages/mid_lib/sura.pkg.json")) -or
            -not (Test-Path (Join-Path $transitiveProject "packages/base_lib/sura.pkg.json"))) {
            throw "expected restore to install both direct and transitive packages"
        }

        $lockReportPath = Join-Path $temp "lock-report.json"
        $lockTransitive = Run-Pkg -PkgArgs @("lock", "--json", $lockReportPath)
        if ($lockTransitive.Code -ne 0) {
            Write-Output $lockTransitive.Output
            throw "expected lock to include transitive dependencies"
        }
        $lockText = Get-Content -Raw -Path (Join-Path $transitiveProject "sura.lock.json")
        if ($lockText -notmatch '"mid_lib"' -or $lockText -notmatch '"base_lib"') {
            throw "expected lockfile to include direct and transitive packages"
        }
        $lockReport = Get-Content -Raw -Path $lockReportPath | ConvertFrom-Json
        $lockNames = @($lockReport.packages | ForEach-Object { "$($_.name)@$($_.version)" })
        if ($lockReport.schema -ne "sura.package.lock.v1" -or
            $lockReport.passed -ne $true -or
            $lockReport.package_count -ne 2 -or
            $lockNames -notcontains "mid_lib@1.0.0" -or
            $lockNames -notcontains "base_lib@1.0.0" -or
            -not $lockReport.packages[0].hash) {
            throw "expected lock --json to report direct and transitive locked packages"
        }

        $verifyReportPath = Join-Path $temp "verify-lock-report.json"
        $verifyLock = Run-Pkg -PkgArgs @("verify", "--json", $verifyReportPath)
        if ($verifyLock.Code -ne 0) {
            Write-Output $verifyLock.Output
            throw "expected verify --json to pass for lockfile"
        }
        $verifyReport = Get-Content -Raw -Path $verifyReportPath | ConvertFrom-Json
        $verifyStatuses = @($verifyReport.packages | ForEach-Object { "$($_.name)@$($_.version):$($_.status)" })
        if ($verifyReport.schema -ne "sura.package.verify.v1" -or
            $verifyReport.mode -ne "lockfile" -or
            $verifyReport.passed -ne $true -or
            $verifyReport.package_count -ne 2 -or
            $verifyStatuses -notcontains "mid_lib@1.0.0:ok" -or
            $verifyStatuses -notcontains "base_lib@1.0.0:ok") {
            throw "expected verify --json to report clean lockfile packages"
        }
    }
    finally {
        Pop-Location
    }

    $conflictProject = Join-Path $temp "conflict_app"
    New-Item -ItemType Directory -Force -Path $conflictProject | Out-Null
    Push-Location $conflictProject
    try {
        Write-Text (Join-Path $conflictProject "sura.pkg.json") @"
{
  "name": "conflict_app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {
    "mid_lib": "^1.0.0",
    "other_mid": "^1.0.0"
  }
}
"@
        Write-Text (Join-Path $conflictProject "src/app.sura") "print `"conflict`"`n"

        $conflict = Run-Pkg -PkgArgs @("resolve")
        if ($conflict.Code -eq 0 -or
            $conflict.Output -notmatch "base_lib" -or
            $conflict.Output -notmatch "dependency resolution failed") {
            Write-Output $conflict.Output
            throw "expected resolve to fail on incompatible transitive constraints"
        }
        $conflictJson = Run-Pkg -PkgArgs @("resolve", "--json")
        if ($conflictJson.Code -eq 0) {
            Write-Output $conflictJson.Output
            throw "expected conflict resolve --json to fail"
        }
        $conflictReport = $conflictJson.Output | ConvertFrom-Json
        if ($conflictReport.schema -ne "sura.package.resolve.v1" -or
            $conflictReport.ok -ne $false -or
            @($conflictReport.errors).Count -lt 1 -or
            ($conflictReport.errors -join "`n") -notmatch "base_lib") {
            throw "expected conflict resolve --json to report dependency errors"
        }
    }
    finally {
        Pop-Location
    }

    "update_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    $env:SURA_ALLOW_CRITICAL_ADVISORY_INSTALL = $oldAllowCritical
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
