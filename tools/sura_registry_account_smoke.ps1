param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_registry_account_" + [System.Guid]::NewGuid().ToString("N"))
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
    Push-Location $temp
    try {
        $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $ErrorActionPreference = $old
    }
    return @{ Code = $code; Output = ($out -join "`n") }
}

function Post-Json {
    param([string]$Uri, [object]$Body, [string]$Token = "")
    $json = $Body | ConvertTo-Json -Depth 6 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $headers = @{}
    if ($Token) { $headers["Authorization"] = "Bearer $Token" }
    Invoke-RestMethod -Uri $Uri -Method Post -Headers $headers -ContentType "application/json; charset=utf-8" -Body $bytes
}

function Write-Package {
    param([string]$Root, [string]$Version)
    Write-Text (Join-Path $Root "sura.pkg.json") @"
{
  "name": "account_pkg",
  "version": "$Version",
  "main": "src/account_pkg.sura",
  "bench_report": "artifacts/account-bench.json",
  "audit_report": "artifacts/account-audit.json",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $Root "src/account_pkg.sura") @"
func entry do
  print "account $Version"
end

entry()
"@
    Write-Text (Join-Path $Root "README.md") "# account_pkg $Version`n"
    Write-Text (Join-Path $Root "docs/api.json") @"
{
  "name": "account_pkg",
  "version": "$Version",
  "symbols": [
    {"kind": "function", "name": "entry", "signature": "func entry do", "source": "src/account_pkg.sura", "line": 1}
  ],
  "benchmark": {
    "source": "artifacts/account-bench.json",
    "speedup": 2.5,
    "suraFasterByPython": 3.25
  }
}
"@
    Write-Text (Join-Path $Root "docs/search-index.json") @"
{"entries":[{"type":"symbol","kind":"function","name":"entry","title":"entry","text":"func entry do","source":"src/account_pkg.sura","line":1}]}
"@
    Write-Text (Join-Path $Root "artifacts/account-bench.json") @"
{
  "schema": "sura.package.bench.v1",
  "speedup": 2.5,
  "sura_faster_by_python": 3.25
}
"@
    Write-Text (Join-Path $Root "artifacts/account-audit.json") @"
{
  "schema": "sura.package.audit.v1",
  "passed": true,
  "finding_count": 2,
  "findings": [
    {"kind": "network_call", "message": "HTTP API use requires package policy review", "file": "src/account_pkg.sura", "line": 1},
    {"kind": "ffi_call", "message": "Native bridge is allowed only with signed release metadata", "file": "src/account_pkg.sura", "line": 2}
  ]
}
"@
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
$oldToken = $env:SURA_REGISTRY_TOKEN
$serverProcess = $null
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $serverRegistry = Join-Path $temp "server_registry"
    $clientRegistry = Join-Path $temp "client_registry"
    $pkg = Join-Path $temp "account_pkg"

    $port = Get-Random -Minimum 49001 -Maximum 59000
    $url = "http://127.0.0.1:$port"
    $server = Join-Path (Get-Location) "tools/sura_registry_api.js"
    $stdout = Join-Path $temp "registry_account.out.log"
    $stderr = Join-Path $temp "registry_account.err.log"
    $serverProcess = Start-Process -FilePath "node" `
        -ArgumentList @($server, "--root", $serverRegistry, "--port", "$port", "--token", "dev-token", "--admin-token", "admin-token") `
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

    $issued = Post-Json -Uri "${url}/api/tokens" -Token "admin-token" -Body @{
        user = "alice"
        token = "alice-old-token"
    }
    if (-not $issued.ok -or $issued.user -ne "alice" -or -not $issued.recoveryCode) {
        throw "expected admin token creation to return a recovery code"
    }

    $env:SURA_REGISTRY = $clientRegistry
    $env:SURA_REGISTRY_URL = $url
    $env:SURA_REGISTRY_TOKEN = "alice-old-token"

    Write-Package -Root $pkg -Version "0.1.0"
    $publishOld = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($publishOld.Code -ne 0 -or $publishOld.Output -notmatch "uploaded account_pkg@0\.1\.0") {
        Write-Output $publishOld.Output
        throw "expected first publish with original token to pass"
    }

    $recoverReportPath = Join-Path $temp "recover-token-report.json"
    $recover = Run-Pkg -PkgArgs @("recover-token", "alice", "$($issued.recoveryCode)", "alice-new-token", "--json", $recoverReportPath)
    if ($recover.Code -ne 0 -or
        -not (Test-Path $recoverReportPath) -or
        $recover.Output -notmatch "recovered registry token for alice" -or
        $recover.Output -notmatch "token:\s+alice-new-token" -or
        $recover.Output -notmatch "recoveryCode:") {
        Write-Output $recover.Output
        throw "expected surapkg recover-token to return a new token and recovery code"
    }
    $recoverReport = Get-Content -Raw -Path $recoverReportPath | ConvertFrom-Json
    if ($recoverReport.schema -ne "sura.registry.recover_token.v1" -or
        $recoverReport.user -ne "alice" -or
        $recoverReport.registry_url -ne $url -or
        $recoverReport.passed -ne $true -or
        $recoverReport.requested_token_supplied -ne $true -or
        $recoverReport.token_returned -ne $true -or
        $recoverReport.recovery_code_returned -ne $true -or
        $recoverReport.token -ne "alice-new-token" -or
        -not $recoverReport.recovery_code) {
        $recoverReport | ConvertTo-Json -Depth 6 | Write-Output
        throw "expected recover-token JSON report to capture the recovered token result"
    }

    $reuse = Run-Pkg -PkgArgs @("recover-token", "alice", "$($issued.recoveryCode)", "alice-reuse-token")
    if ($reuse.Code -eq 0) {
        Write-Output $reuse.Output
        throw "expected used recovery code to fail"
    }

    Write-Package -Root $pkg -Version "0.2.0"
    $env:SURA_REGISTRY_TOKEN = "alice-old-token"
    $oldPublishAfterRecovery = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($oldPublishAfterRecovery.Code -eq 0) {
        Write-Output $oldPublishAfterRecovery.Output
        throw "expected revoked original token to fail publishing"
    }

    $env:SURA_REGISTRY_TOKEN = "alice-new-token"
    $newPublish = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($newPublish.Code -ne 0 -or $newPublish.Output -notmatch "uploaded account_pkg@0\.2\.0") {
        Write-Output $newPublish.Output
        throw "expected recovered token to publish the owned package"
    }

    $detail = Invoke-RestMethod -Uri "${url}/api/package/account_pkg/0.2.0" -Method Get
    $auditFindings = @($detail.audit.findings)
    if (-not $detail.ok -or
        $detail.name -ne "account_pkg" -or
        $detail.version -ne "0.2.0" -or
        $detail.owner -ne "alice" -or
        $detail.api.symbols[0].name -ne "entry" -or
        $detail.benchmark.speedup -ne 2.5 -or
        $detail.audit.passed -ne $true -or
        $detail.audit.finding_count -ne 2 -or
        $auditFindings.Count -ne 2 -or
        $auditFindings[0].kind -ne "network_call") {
        throw "expected package detail API to expose owner, docs, benchmark, and audit metadata"
    }

    $latestDetail = Invoke-RestMethod -Uri "${url}/api/package/account_pkg" -Method Get
    if ($latestDetail.version -ne "0.2.0") {
        throw "expected unversioned package detail API to resolve latest"
    }

    $packagePage = Invoke-WebRequest -Uri "${url}/package/account_pkg/0.2.0" -UseBasicParsing
    if ($packagePage.StatusCode -ne 200 -or
        $packagePage.Content -notmatch "Security Audit" -or
        $packagePage.Content -notmatch "audit passed" -or
        $packagePage.Content -notmatch "2 findings" -or
        $packagePage.Content -notmatch "network_call" -or
        $packagePage.Content -notmatch "HTTP API use requires package policy review") {
        throw "expected browser package page to render audit summary and findings"
    }

    $remoteOwners = Run-Pkg -PkgArgs @("owners", "account_pkg", "--json")
    if ($remoteOwners.Code -ne 0) {
        Write-Output $remoteOwners.Output
        throw "expected HTTP registry owners --json to pass"
    }
    $ownersReport = $remoteOwners.Output | ConvertFrom-Json
    $ownerItems = @($ownersReport.owners)
    if ($ownersReport.schema -ne "sura.registry.owners.v1" -or
        $ownersReport.source -ne "http" -or
        $ownersReport.query -ne "account_pkg" -or
        $ownersReport.count -ne 1 -or
        $ownerItems[0].name -ne "account_pkg" -or
        $ownerItems[0].owner -ne "alice" -or
        [string]::IsNullOrWhiteSpace($ownerItems[0].created_at) -or
        [string]::IsNullOrWhiteSpace($ownerItems[0].updated_at)) {
        throw "expected HTTP owners --json to include account_pkg owner metadata"
    }

    $remoteInfo = Run-Pkg -PkgArgs @("info", "account_pkg@0.2.0")
    if ($remoteInfo.Code -ne 0 -or
        $remoteInfo.Output -notmatch "account_pkg@0\.2\.0 \(registry\)" -or
        $remoteInfo.Output -notmatch "owner:\s+alice" -or
        $remoteInfo.Output -notmatch "Benchmark" -or
        $remoteInfo.Output -notmatch "JIT speedup:\s+2\.5x" -or
        $remoteInfo.Output -notmatch "Sura faster than Python:\s+3\.25x" -or
        $remoteInfo.Output -notmatch "Security Audit" -or
        $remoteInfo.Output -notmatch "passed:\s+true" -or
        $remoteInfo.Output -notmatch "findings:\s+2" -or
        $remoteInfo.Output -notmatch "entry") {
        Write-Output $remoteInfo.Output
        throw "expected surapkg info to read remote registry package detail with benchmark and audit metadata"
    }

    $remoteInfoJson = Run-Pkg -PkgArgs @("info", "account_pkg@0.2.0", "--json")
    if ($remoteInfoJson.Code -ne 0) {
        Write-Output $remoteInfoJson.Output
        throw "expected surapkg info --json to pass"
    }
    $infoReport = $remoteInfoJson.Output | ConvertFrom-Json
    $infoSymbols = @($infoReport.symbols)
    if ($infoReport.schema -ne "sura.package.info.v1" -or
        -not $infoReport.passed -or
        $infoReport.source -ne "registry" -or
        $infoReport.package -ne "account_pkg" -or
        $infoReport.version -ne "0.2.0" -or
        $infoReport.owner -ne "alice" -or
        $infoReport.benchmark.speedup -ne 2.5 -or
        $infoReport.benchmark.sura_faster_by_python -ne 3.25 -or
        $infoReport.audit.present -ne $true -or
        $infoReport.audit.passed -ne $true -or
        $infoReport.audit.finding_count -ne 2 -or
        $infoSymbols.Count -lt 1 -or
        $infoSymbols[0].kind -ne "function" -or
        $infoSymbols[0].name -ne "entry" -or
        $infoSymbols[0].source -ne "src/account_pkg.sura") {
        Write-Output $remoteInfoJson.Output
        throw "expected surapkg info --json to expose registry metadata with audit fields"
    }

    $tokensRaw = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $serverRegistry "tokens.json")
    if ($tokensRaw -notmatch '"revokedAt"' -or $tokensRaw -notmatch '"recoveredAt"') {
        throw "expected tokens.json to record token revocation and recovery"
    }

    "registry_account_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    $env:SURA_REGISTRY_TOKEN = $oldToken
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force
    }
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
