param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_registry_verify_" + [System.Guid]::NewGuid().ToString("N"))
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

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
$serverProcess = $null
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $pkg = Join-Path $temp "trusted_pkg"
    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $null

    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "trusted_pkg",
  "version": "1.0.0",
  "main": "src/trusted_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/trusted_pkg.sura") @"
func trusted do
  return true
end
"@

    $publish = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($publish.Code -ne 0) {
        Write-Output $publish.Output
        throw "expected publish to pass"
    }

    Write-Text (Join-Path $registry "owners.json") @"
{
  "packages": {
    "trusted_pkg": {"owner": "alice", "createdAt": "2026-01-01T00:00:00Z"}
  }
}
"@
    Write-Text (Join-Path $registry "yanks.json") @"
{
  "yanked": {
    "trusted_pkg@1.0.0": {"reason": "metadata check only", "by": "admin", "at": "2026-01-01T00:00:00Z"}
  }
}
"@
    Write-Text (Join-Path $registry "reports.json") @"
{
  "reports": [
    {"id":"r1","name":"trusted_pkg","version":"1.0.0","reason":"metadata check","status":"open"}
  ]
}
"@
    Write-Text (Join-Path $registry "advisories.json") @"
{
  "advisories": [
    {"id":"adv1","name":"trusted_pkg","version":"1.0.0","severity":"high","status":"active","title":"metadata check advisory","description":"verify-registry should accept advisory references to existing package versions","url":"https://example.com/adv1","createdBy":"admin","createdAt":"2026-01-01T00:00:00Z","updatedAt":"2026-01-01T00:00:00Z"}
  ]
}
"@

    $verify = Run-Pkg -PkgArgs @("verify-registry", $registry)
    if ($verify.Code -ne 0 -or $verify.Output -notmatch "verify-registry:\s+1 package\(s\).*0 error\(s\)") {
        Write-Output $verify.Output
        throw "expected verify-registry to pass"
    }
    $verifyReportPath = Join-Path $temp "registry-verify.json"
    $verifyJson = Run-Pkg -PkgArgs @("verify-registry", $registry, "--json", $verifyReportPath)
    if ($verifyJson.Code -ne 0 -or -not (Test-Path $verifyReportPath)) {
        Write-Output $verifyJson.Output
        throw "expected verify-registry --json to pass and write a report"
    }
    $verifyReport = [System.IO.File]::ReadAllText($verifyReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($verifyReport.schema -ne "sura.registry.verify.v1" -or
        $verifyReport.source -ne "local" -or
        $verifyReport.passed -ne $true -or
        $verifyReport.package_count -ne 1 -or
        $verifyReport.error_count -ne 0) {
        $verifyReport | ConvertTo-Json -Depth 6
        throw "unexpected local verify-registry JSON report"
    }
    $owners = Run-Pkg -PkgArgs @("owners", "trusted_pkg", "--json")
    if ($owners.Code -ne 0) {
        Write-Output $owners.Output
        throw "expected local registry owners --json to pass"
    }
    $ownersReport = $owners.Output | ConvertFrom-Json
    $ownerItems = @($ownersReport.owners)
    if ($ownersReport.schema -ne "sura.registry.owners.v1" -or
        $ownersReport.source -ne "local" -or
        $ownersReport.query -ne "trusted_pkg" -or
        $ownersReport.count -ne 1 -or
        $ownerItems[0].name -ne "trusted_pkg" -or
        $ownerItems[0].owner -ne "alice" -or
        $ownerItems[0].created_at -ne "2026-01-01T00:00:00Z") {
        throw "expected local owners --json to include trusted_pkg owner metadata"
    }

    $remoteRegistry = Join-Path $temp "remote_registry"
    Copy-Item -Path $registry -Destination $remoteRegistry -Recurse
    Remove-Item -LiteralPath (Join-Path $remoteRegistry "yanks.json") -Force
    $port = Get-Random -Minimum 29001 -Maximum 39000
    $url = "http://127.0.0.1:$port"
    $server = Join-Path (Get-Location) "tools/sura_registry_api.js"
    $stdout = Join-Path $temp "registry_verify.out.log"
    $stderr = Join-Path $temp "registry_verify.err.log"
    $serverProcess = Start-Process -FilePath "node" `
        -ArgumentList @($server, "--root", $remoteRegistry, "--port", "$port", "--token", "dev-token", "--admin-token", "admin-token") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            $health = Invoke-RestMethod -Uri "${url}/health" -Method Get
            if ($health.ok) { $ready = $true; break }
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $ready) {
        if (Test-Path $stderr) { Get-Content -Raw -Path $stderr | Write-Output }
        throw "registry API did not become ready"
    }

    $env:SURA_REGISTRY_URL = $url
    $remoteVerify = Run-Pkg -PkgArgs @("verify-registry")
    if ($remoteVerify.Code -ne 0 -or $remoteVerify.Output -notmatch "url:\s+http://127\.0\.0\.1:$port" -or $remoteVerify.Output -notmatch "0 error\(s\)") {
        Write-Output $remoteVerify.Output
        throw "expected remote verify-registry to pass"
    }
    $remoteReportPath = Join-Path $temp "remote-registry-verify.json"
    $remoteVerifyJson = Run-Pkg -PkgArgs @("verify-registry", "--json=$remoteReportPath")
    if ($remoteVerifyJson.Code -ne 0 -or -not (Test-Path $remoteReportPath)) {
        Write-Output $remoteVerifyJson.Output
        throw "expected remote verify-registry --json to pass and write a report"
    }
    $remoteReport = [System.IO.File]::ReadAllText($remoteReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($remoteReport.schema -ne "sura.registry.verify.v1" -or
        $remoteReport.source -ne "remote" -or
        $remoteReport.url -ne $url -or
        $remoteReport.passed -ne $true -or
        $remoteReport.package_count -ne 1 -or
        $remoteReport.error_count -ne 0) {
        $remoteReport | ConvertTo-Json -Depth 6
        throw "unexpected remote verify-registry JSON report"
    }
    $env:SURA_REGISTRY_URL = $null

    $badRegistry = Join-Path $temp "bad_registry"
    Copy-Item -Path $registry -Destination $badRegistry -Recurse
    $indexPath = Join-Path $badRegistry "index.json"
    $index = Get-Content -Raw -Path $indexPath
    $index = $index -replace '"hash"\s*:\s*"[^"]+"', '"hash":"bad-hash"'
    Write-Text $indexPath $index
    $badReportPath = Join-Path $temp "bad-registry-verify.json"
    $badVerify = Run-Pkg -PkgArgs @("verify-registry", $badRegistry, "--json", $badReportPath)
    if ($badVerify.Code -eq 0 -or $badVerify.Output -notmatch "index hash mismatch") {
        Write-Output $badVerify.Output
        throw "expected bad registry hash to fail"
    }
    if (-not (Test-Path $badReportPath)) {
        Write-Output $badVerify.Output
        throw "expected bad verify-registry --json to write a report"
    }
    $badReport = [System.IO.File]::ReadAllText($badReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($badReport.passed -ne $false -or
        $badReport.error_count -lt 1 -or
        -not ($badReport.findings | Where-Object { $_.severity -eq "error" -and $_.message -match "index hash mismatch" })) {
        $badReport | ConvertTo-Json -Depth 6
        throw "unexpected bad verify-registry JSON report"
    }

    $badAdvisoryRegistry = Join-Path $temp "bad_advisory_registry"
    Copy-Item -Path $registry -Destination $badAdvisoryRegistry -Recurse
    Write-Text (Join-Path $badAdvisoryRegistry "advisories.json") @"
{
  "advisories": [
    {"id":"adv-missing","name":"missing_pkg","version":"9.9.9","severity":"critical","status":"active","title":"missing package advisory","description":"verify-registry should fail advisories that point at missing package versions"}
  ]
}
"@
    $badAdvisoryReportPath = Join-Path $temp "bad-advisory-registry-verify.json"
    $badAdvisoryVerify = Run-Pkg -PkgArgs @("verify-registry", $badAdvisoryRegistry, "--json", $badAdvisoryReportPath)
    if ($badAdvisoryVerify.Code -eq 0 -or $badAdvisoryVerify.Output -notmatch "advisory metadata references missing package version") {
        Write-Output $badAdvisoryVerify.Output
        throw "expected bad advisory metadata to fail"
    }
    if (-not (Test-Path $badAdvisoryReportPath)) {
        Write-Output $badAdvisoryVerify.Output
        throw "expected bad advisory verify-registry --json to write a report"
    }
    $badAdvisoryReport = [System.IO.File]::ReadAllText($badAdvisoryReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($badAdvisoryReport.passed -ne $false -or
        $badAdvisoryReport.error_count -lt 1 -or
        -not ($badAdvisoryReport.findings | Where-Object { $_.severity -eq "error" -and $_.message -match "advisory metadata references missing package version" })) {
        $badAdvisoryReport | ConvertTo-Json -Depth 6
        throw "unexpected bad advisory verify-registry JSON report"
    }

    $missingSigRegistry = Join-Path $temp "missing_sig_registry"
    Copy-Item -Path $registry -Destination $missingSigRegistry -Recurse
    Remove-Item -LiteralPath (Join-Path $missingSigRegistry "trusted_pkg/1.0.0/sura.pkg.sig") -Force
    $missingSigReportPath = Join-Path $temp "missing-sig-registry-verify.json"
    $missingSig = Run-Pkg -PkgArgs @("verify-registry", $missingSigRegistry, "--json=$missingSigReportPath")
    if ($missingSig.Code -eq 0 -or $missingSig.Output -notmatch "signature file not found") {
        Write-Output $missingSig.Output
        throw "expected missing signature to fail"
    }
    if (-not (Test-Path $missingSigReportPath)) {
        Write-Output $missingSig.Output
        throw "expected missing signature verify-registry --json to write a report"
    }
    $missingSigReport = [System.IO.File]::ReadAllText($missingSigReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($missingSigReport.passed -ne $false -or
        $missingSigReport.error_count -lt 1 -or
        -not ($missingSigReport.findings | Where-Object { $_.severity -eq "error" -and $_.message -match "signature file not found" })) {
        $missingSigReport | ConvertTo-Json -Depth 6
        throw "unexpected missing signature verify-registry JSON report"
    }

    "registry_verify_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force
    }
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# This gate printed PASS while exiting nonzero: its last native command
# was a negative check that correctly failed, and the script inherited
# that code. CI reads the exit code, so a passing gate reported failure.
exit 0
