param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$surapkgPath = (Resolve-Path -LiteralPath $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_registry_service_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$serverProcess = $null

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Read-Text {
    param([string]$Path)
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Assert-Contains {
    param([string]$Path, [string[]]$Needles)
    $text = Read-Text $Path
    foreach ($needle in $Needles) {
        if (-not $text.Contains($needle)) {
            throw "$Path missing expected text: $needle"
        }
    }
}

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $surapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

function Post-Json {
    param([string]$Uri, [object]$Body, [string]$Token = "")
    $json = $Body | ConvertTo-Json -Depth 8 -Compress
    $headers = @{}
    if ($Token) { $headers["Authorization"] = "Bearer $Token" }
    Invoke-RestMethod -Uri $Uri -Method Post -Headers $headers -ContentType "application/json; charset=utf-8" -Body ([System.Text.Encoding]::UTF8.GetBytes($json))
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
$oldToken = $env:SURA_REGISTRY_TOKEN
$oldAdminToken = $env:SURA_REGISTRY_ADMIN_TOKEN
$oldHost = $env:SURA_REGISTRY_HOST
$oldPort = $env:PORT

try {
    Assert-Contains (Join-Path $root "deploy/registry/Dockerfile") @(
        "HEALTHCHECK",
        "SURA_REGISTRY_HOST=0.0.0.0",
        "VOLUME",
        "USER sura"
    )
    Assert-Contains (Join-Path $root "deploy/registry/docker-compose.yml") @(
        "restart: unless-stopped",
        "SURA_REGISTRY_TOKEN",
        "SURA_REGISTRY_ADMIN_TOKEN",
        "healthcheck"
    )
    Assert-Contains (Join-Path $root "deploy/registry/systemd/sura-registry.service") @(
        "Restart=always",
        "EnvironmentFile=/etc/sura/registry.env",
        "ReadWritePaths=/var/lib/sura-registry"
    )

    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $pkgRoot = Join-Path $registry "service_pkg/0.1.0"
    Write-Text (Join-Path $pkgRoot "package.surabundle.json") @"
{
  "schema": "sura.package.bundle.v1",
  "name": "service_pkg",
  "version": "0.1.0",
  "files": [
    {
      "path": "sura.pkg.json",
      "content": "{\"name\":\"service_pkg\",\"version\":\"0.1.0\",\"main\":\"src/service_pkg.sura\"}"
    },
    {
      "path": "src/service_pkg.sura",
      "content": "print \"service pkg\""
    }
  ]
}
"@
    Write-Text (Join-Path $registry "reports.json") "{`"reports`":[]}`n"
    Write-Text (Join-Path $registry "advisories.json") "{`"advisories`":[]}`n"
    Write-Text (Join-Path $registry "stats.json") "{`"downloads`":{},`"publishes`":{},`"downloadDays`":{},`"publishDays`":{}}`n"

    $port = Get-Random -Minimum 30000 -Maximum 39000
    $url = "http://127.0.0.1:$port"
    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $url
    $env:SURA_REGISTRY_TOKEN = "service-token"
    $env:SURA_REGISTRY_ADMIN_TOKEN = "service-admin-token"
    $env:SURA_REGISTRY_HOST = "127.0.0.1"
    $env:PORT = "$port"

    $stdout = Join-Path $temp "registry-service.out.log"
    $stderr = Join-Path $temp "registry-service.err.log"
    $server = Join-Path $root "tools/sura_registry_api.js"
    $serverProcess = Start-Process -FilePath "node" `
        -ArgumentList @($server) `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    $health = $null
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $health = Invoke-RestMethod -Uri "$url/health" -Method Get
            if ($health.ok) { break }
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $health -or -not $health.ok) {
        if (Test-Path -LiteralPath $stderr) { Get-Content -Raw -Path $stderr | Write-Output }
        throw "registry service did not become healthy"
    }
    if ($health.schema -ne "sura.registry.health_endpoint.v1" -or
        $health.service -ne "sura-registry" -or
        $health.packageCount -lt 1 -or
        $health.reportCount -ne 0 -or
        $health.openReportCount -ne 0) {
        $health | ConvertTo-Json -Depth 8
        throw "unexpected registry service health payload"
    }

    $report = Post-Json -Uri "$url/api/report" -Body @{
        name = "service_pkg"
        version = "0.1.0"
        reason = "service smoke abuse report"
        contact = "ops@example.com"
        source = "service-smoke"
    }
    if (-not $report.ok -or [string]::IsNullOrWhiteSpace($report.id)) {
        $report | ConvertTo-Json -Depth 8
        throw "expected registry service report API to create an abuse report"
    }

    $reports = Invoke-RestMethod -Uri "$url/api/reports" -Headers @{ Authorization = "Bearer service-admin-token" } -Method Get
    if (-not $reports.reports -or @($reports.reports).Count -ne 1 -or $reports.reports[0].status -ne "open") {
        $reports | ConvertTo-Json -Depth 8
        throw "expected admin report queue to include the abuse report"
    }

    $review = Post-Json -Uri "$url/api/reports/review" -Token "service-admin-token" -Body @{
        id = $report.id
        status = "reviewing"
        note = "service smoke triage"
    }
    if (-not $review.ok -or $review.report.status -ne "reviewing" -or $review.report.reviewedBy -ne "admin") {
        $review | ConvertTo-Json -Depth 8
        throw "expected admin review queue to update report status"
    }

    $reviewingQueue = Invoke-RestMethod -Uri "$url/api/reports?status=reviewing&limit=10" -Headers @{ Authorization = "Bearer service-admin-token" } -Method Get
    if ($reviewingQueue.schema -ne "sura.registry.reports.queue.v1" -or
        $reviewingQueue.count -ne 1 -or
        $reviewingQueue.reports[0].id -ne $report.id -or
        $reviewingQueue.counts.reviewing -ne 1) {
        $reviewingQueue | ConvertTo-Json -Depth 8
        throw "expected filtered admin review queue summary"
    }

    $action = Post-Json -Uri "$url/api/reports/review" -Token "service-admin-token" -Body @{
        id = $report.id
        status = "actioned"
        note = "service smoke yank action"
        yank = $true
    }
    if (-not $action.ok -or $action.yanked -ne $true -or $action.report.status -ne "actioned" -or $action.report.action -ne "yanked") {
        $action | ConvertTo-Json -Depth 8
        throw "expected actioned abuse report review to yank package"
    }

    $actionQueue = Invoke-RestMethod -Uri "$url/api/reports?status=actioned&name=service_pkg" -Headers @{ Authorization = "Bearer service-admin-token" } -Method Get
    if ($actionQueue.count -ne 1 -or $actionQueue.counts.actioned -ne 1 -or $actionQueue.reports[0].id -ne $report.id) {
        $actionQueue | ConvertTo-Json -Depth 8
        throw "expected actioned report queue to expose moderated report"
    }

    $indexAfterAction = Invoke-RestMethod -Uri "$url/index.json" -Method Get
    $pkgAfterAction = @($indexAfterAction.packages | Where-Object { $_.name -eq "service_pkg" -and $_.version -eq "0.1.0" })[0]
    if (-not $pkgAfterAction -or $pkgAfterAction.yanked -ne $true -or [string]::IsNullOrWhiteSpace($pkgAfterAction.yankReason)) {
        $indexAfterAction | ConvertTo-Json -Depth 8
        throw "expected actioned abuse review to update registry yank metadata"
    }

    $moderationLog = Read-Text (Join-Path $registry "moderation-log.jsonl")
    if ($moderationLog -notmatch '"schema":"sura.registry.moderation_event.v1"' -or
        $moderationLog -notmatch '"type":"report.created"' -or
        $moderationLog -notmatch '"type":"report.reviewed"' -or
        $moderationLog -notmatch '"yanked":true') {
        Write-Output $moderationLog
        throw "expected moderation audit log to record report creation and actioned review"
    }

    $healthAfter = Invoke-RestMethod -Uri "$url/health" -Method Get
    if ($healthAfter.reportCount -ne 1 -or
        $healthAfter.openReportCount -ne 0 -or
        $healthAfter.reportCounts.actioned -ne 1) {
        $healthAfter | ConvertTo-Json -Depth 8
        throw "expected health endpoint to expose reviewed report counts"
    }

    $env:SURA_REGISTRY_TOKEN = "service-admin-token"
    $registryHealth = Run-Pkg -PkgArgs @("registry-health", "--fail-on-warning", "--json")
    if ($registryHealth.Code -ne 0) {
        Write-Output $registryHealth.Output
        throw "expected surapkg registry-health --fail-on-warning to pass against service"
    }
    $registryHealthReport = $registryHealth.Output | ConvertFrom-Json
    if ($registryHealthReport.schema -ne "sura.registry.health.v1" -or
        $registryHealthReport.source -ne "http" -or
        $registryHealthReport.passed -ne $true -or
        $registryHealthReport.health_endpoint_ok -ne $true -or
        $registryHealthReport.reports_reachable -ne $true -or
        $registryHealthReport.warning_count -ne 0 -or
        $registryHealthReport.error_count -ne 0) {
        $registryHealthReport | ConvertTo-Json -Depth 8
        throw "expected registry-health to verify the hosted-service contract"
    }

    "registry_service_smoke: PASS"
}
finally {
    if ($serverProcess -and -not $serverProcess.HasExited) {
        $serverProcess.CloseMainWindow() | Out-Null
        Start-Sleep -Milliseconds 200
        if (-not $serverProcess.HasExited) {
            Stop-Process -Id $serverProcess.Id -Force
        }
    }
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    $env:SURA_REGISTRY_TOKEN = $oldToken
    $env:SURA_REGISTRY_ADMIN_TOKEN = $oldAdminToken
    $env:SURA_REGISTRY_HOST = $oldHost
    $env:PORT = $oldPort
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
