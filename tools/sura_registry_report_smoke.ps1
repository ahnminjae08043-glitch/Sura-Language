param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_registry_report_" + [System.Guid]::NewGuid().ToString("N"))
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

function Post-Json {
    param([string]$Uri, [object]$Body, [string]$Token = "")
    $json = $Body | ConvertTo-Json -Depth 6 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $headers = @{}
    if ($Token) { $headers["Authorization"] = "Bearer $Token" }
    Invoke-RestMethod -Uri $Uri -Method Post -Headers $headers -ContentType "application/json; charset=utf-8" -Body $bytes
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
$oldToken = $env:SURA_REGISTRY_TOKEN
$serverProcess = $null
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $pkg = Join-Path $temp "report_pkg"
    $localRegistry = Join-Path $temp "local_registry"

    $env:SURA_REGISTRY = $localRegistry
    $env:SURA_REGISTRY_URL = $null
    $env:SURA_REGISTRY_TOKEN = $null
    $localReport = Run-Pkg -PkgArgs @("report", "local_pkg@0.1.0", "local", "registry", "report", "path", "--json")
    if ($localReport.Code -ne 0) {
        Write-Output $localReport.Output
        throw "expected local surapkg report --json to pass"
    }
    $localSubmitReport = $localReport.Output | ConvertFrom-Json
    if ($localSubmitReport.schema -ne "sura.registry.report.v1" -or
        -not $localSubmitReport.passed -or
        $localSubmitReport.source -ne "local" -or
        $localSubmitReport.name -ne "local_pkg" -or
        $localSubmitReport.version -ne "0.1.0" -or
        $localSubmitReport.status -ne "open" -or
        $localSubmitReport.reporter -ne "local-cli" -or
        [string]::IsNullOrWhiteSpace($localSubmitReport.id)) {
        Write-Output $localReport.Output
        throw "expected local report --json to expose submitted report metadata"
    }
    $localReports = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $localRegistry "reports.json")
    if ($localReports -notmatch '"name":"local_pkg"' -or $localReports -notmatch '"status":"open"') {
        throw "expected local reports.json to include report"
    }
    $localReportsJson = Run-Pkg -PkgArgs @("reports", "open", "--json")
    if ($localReportsJson.Code -ne 0) {
        Write-Output $localReportsJson.Output
        throw "expected local surapkg reports --json to pass"
    }
    $localReportsReport = $localReportsJson.Output | ConvertFrom-Json
    $localReportItems = @($localReportsReport.reports)
    if ($localReportsReport.schema -ne "sura.registry.reports.v1" -or
        $localReportsReport.status -ne "open" -or
        $localReportsReport.count -ne 1 -or
        $localReportItems[0].name -ne "local_pkg" -or
        $localReportItems[0].status -ne "open") {
        throw "expected local reports --json to include open local_pkg report"
    }

    $port = Get-Random -Minimum 19000 -Maximum 29000
    $url = "http://127.0.0.1:$port"
    $server = Join-Path (Get-Location) "tools/sura_registry_api.js"
    $stdout = Join-Path $temp "registry.out.log"
    $stderr = Join-Path $temp "registry.err.log"
    $serverProcess = Start-Process -FilePath "node" `
        -ArgumentList @($server, "--root", $registry, "--port", "$port", "--token", "dev-token", "--admin-token", "admin-token") `
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

    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $url
    $env:SURA_REGISTRY_TOKEN = "dev-token"

    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "report_pkg",
  "version": "0.1.0",
  "main": "src/report_pkg.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/report_pkg.sura") "print `"report pkg`"`n"

    $publish = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($publish.Code -ne 0 -or $publish.Output -notmatch "uploaded report_pkg@0\.1\.0") {
        Write-Output $publish.Output
        throw "expected HTTP publish to pass"
    }

    $env:SURA_REGISTRY_TOKEN = "admin-token"
    $advisory = Run-Pkg -PkgArgs @(
        "advisory", "report_pkg@0.1.0",
        "--severity", "high",
        "--title", "Unsafe test API",
        "--description", "Smoke test advisory for registry security metadata",
        "--url", "https://example.com/sura-advisory",
        "--json"
    )
    if ($advisory.Code -ne 0) {
        Write-Output $advisory.Output
        throw "expected admin advisory CLI creation to pass"
    }
    $advisoryCreateReport = $advisory.Output | ConvertFrom-Json
    if ($advisoryCreateReport.schema -ne "sura.registry.advisory.v1" -or
        $advisoryCreateReport.source -ne "http" -or
        $advisoryCreateReport.advisory.name -ne "report_pkg" -or
        $advisoryCreateReport.advisory.version -ne "0.1.0" -or
        $advisoryCreateReport.advisory.severity -ne "high" -or
        $advisoryCreateReport.advisory.title -ne "Unsafe test API" -or
        [string]::IsNullOrWhiteSpace($advisoryCreateReport.advisory.id)) {
        Write-Output ($advisoryCreateReport | ConvertTo-Json -Depth 6)
        throw "expected advisory CLI JSON to expose created advisory"
    }
    $advisoriesApi = Invoke-RestMethod -Uri "${url}/api/advisories?name=report_pkg&version=0.1.0" -Method Get
    $advisoryItems = @($advisoriesApi.advisories)
    if ($advisoryItems.Count -ne 1 -or
        $advisoryItems[0].title -ne "Unsafe test API" -or
        $advisoryItems[0].status -ne "active") {
        throw "expected public advisories API to expose active package advisory"
    }
    $advisoriesCli = Run-Pkg -PkgArgs @("advisories", "report_pkg@0.1.0")
    if ($advisoriesCli.Code -ne 0 -or
        $advisoriesCli.Output -notmatch "Security advisories" -or
        $advisoriesCli.Output -notmatch "high" -or
        $advisoriesCli.Output -notmatch "Unsafe test API") {
        Write-Output $advisoriesCli.Output
        throw "expected surapkg advisories to list remote advisory"
    }
    $advisoriesJson = Run-Pkg -PkgArgs @("advisories", "report_pkg@0.1.0", "--severity", "high", "--json")
    if ($advisoriesJson.Code -ne 0) {
        Write-Output $advisoriesJson.Output
        throw "expected surapkg advisories --json to pass"
    }
    $advisoriesReport = $advisoriesJson.Output | ConvertFrom-Json
    $advisoriesReportItems = @($advisoriesReport.advisories)
    $advisoryActions = @($advisoriesReport.next_actions)
    if ($advisoriesReport.schema -ne "sura.registry.advisories.v1" -or
        $advisoriesReport.source -ne "http" -or
        $advisoriesReport.query -ne "report_pkg@0.1.0" -or
        $advisoriesReport.severity -ne "high" -or
        $advisoriesReport.count -ne 1 -or
        $advisoriesReportItems[0].title -ne "Unsafe test API" -or
        $advisoriesReportItems[0].status -ne "active") {
        throw "expected advisories --json to expose remote advisory metadata"
    }
    if ($advisoryActions.Count -ne 1 -or
        $advisoryActions[0].advisory_id -ne $advisoriesReportItems[0].id -or
        $advisoryActions[0].target -ne "report_pkg@0.1.0" -or
        $advisoryActions[0].severity -ne "high" -or
        $advisoryActions[0].status -ne "active" -or
        $advisoryActions[0].action -notmatch "surapkg update" -or
        $advisoryActions[0].action -notmatch "surapkg audit") {
        $advisoriesReport | ConvertTo-Json -Depth 6
        throw "expected advisories --json to include remote high-severity next_actions"
    }
    $advisoriesHighGate = Run-Pkg -PkgArgs @("advisories", "report_pkg@0.1.0", "--fail-on", "high", "--json")
    if ($advisoriesHighGate.Code -eq 0) {
        Write-Output $advisoriesHighGate.Output
        throw "expected remote advisories --fail-on high to fail on active high advisory"
    }
    $highGateReport = $advisoriesHighGate.Output | ConvertFrom-Json
    if ($highGateReport.passed -ne $false -or
        $highGateReport.source -ne "http" -or
        $highGateReport.fail_on -ne "high" -or
        $highGateReport.failing_count -ne 1 -or
        @($highGateReport.next_actions).Count -ne 1) {
        $highGateReport | ConvertTo-Json -Depth 6
        throw "expected remote advisories --fail-on high JSON gate report"
    }
    $registryHealth = Run-Pkg -PkgArgs @("registry-health", "--json")
    if ($registryHealth.Code -ne 0) {
        Write-Output $registryHealth.Output
        throw "expected remote registry-health --json to pass"
    }
    $registryHealthReport = $registryHealth.Output | ConvertFrom-Json
    if ($registryHealthReport.schema -ne "sura.registry.health.v1" -or
        $registryHealthReport.source -ne "http" -or
        $registryHealthReport.passed -ne $true -or
        $registryHealthReport.health_endpoint_ok -ne $true -or
        $registryHealthReport.package_count -lt 1 -or
        $registryHealthReport.advisory_count -ne 1 -or
        $registryHealthReport.active_advisory_count -ne 1 -or
        $registryHealthReport.reports_reachable -ne $true -or
        $registryHealthReport.error_count -ne 0) {
        $registryHealthReport | ConvertTo-Json -Depth 6
        throw "expected remote registry-health --json to summarize service state"
    }

    $registryHealthWarningGate = Run-Pkg -PkgArgs @("registry-health", "--fail-on-warning", "--json")
    if ($registryHealthWarningGate.Code -ne 0) {
        Write-Output $registryHealthWarningGate.Output
        throw "expected remote registry-health --fail-on-warning --json to pass"
    }
    $registryHealthWarningReport = $registryHealthWarningGate.Output | ConvertFrom-Json
    if ($registryHealthWarningReport.schema -ne "sura.registry.health.v1" -or
        $registryHealthWarningReport.source -ne "http" -or
        $registryHealthWarningReport.passed -ne $true -or
        $registryHealthWarningReport.fail_on_warning -ne $true -or
        $registryHealthWarningReport.warning_count -ne 0 -or
        $registryHealthWarningReport.error_count -ne 0) {
        $registryHealthWarningReport | ConvertTo-Json -Depth 6
        throw "expected remote registry-health --fail-on-warning JSON gate report"
    }

    $packagePage = Invoke-WebRequest -Uri "${url}/package/report_pkg/0.1.0" -UseBasicParsing
    if ($packagePage.StatusCode -ne 200 -or
        $packagePage.Content -notmatch "Sura package report_pkg@0\.1\.0" -or
        $packagePage.Content -notmatch "/api/package/report_pkg/0\.1\.0" -or
        $packagePage.Content -notmatch "package\.surabundle\.json" -or
        $packagePage.Content -notmatch "Unsafe test API") {
        throw "expected browser package detail page to expose package identity, JSON API link, bundle link, and advisories"
    }
    $registryHome = Invoke-WebRequest -Uri "${url}/" -UseBasicParsing
    if ($registryHome.StatusCode -ne 200 -or
        $registryHome.Content -notmatch "Sura Registry" -or
        $registryHome.Content -notmatch "/package/report_pkg/0\.1\.0" -or
        $registryHome.Content -notmatch "/index\.json" -or
        $registryHome.Content -notmatch "active advisories") {
        throw "expected registry home page to list packages, metadata links, and advisory counts"
    }
    $registrySearch = Invoke-WebRequest -Uri "${url}/packages?q=report" -UseBasicParsing
    if ($registrySearch.StatusCode -ne 200 -or
        $registrySearch.Content -notmatch "Packages matching" -or
        $registrySearch.Content -notmatch "report_pkg" -or
        $registrySearch.Content -notmatch "/api/package/report_pkg/0\.1\.0") {
        throw "expected registry package search page to find report_pkg and expose JSON detail link"
    }

    Invoke-WebRequest -Uri "${url}/report_pkg/0.1.0/package.surabundle.json" -UseBasicParsing | Out-Null
    $analyticsApi = Invoke-RestMethod -Uri "${url}/api/analytics" -Method Get
    if (-not $analyticsApi.downloadDays -or -not $analyticsApi.topDownloads) {
        throw "expected registry analytics API to expose daily downloads and top downloads"
    }
    $analyticsCli = Run-Pkg -PkgArgs @("analytics", "report_pkg")
    if ($analyticsCli.Code -ne 0 -or
        $analyticsCli.Output -notmatch "Download analytics" -or
        $analyticsCli.Output -notmatch "report_pkg@0\.1\.0") {
        Write-Output $analyticsCli.Output
        throw "expected surapkg analytics to show remote download analytics"
    }

    $env:SURA_REGISTRY_TOKEN = "admin-token"
    $remoteYank = Run-Pkg -PkgArgs @("yank", "report_pkg@0.1.0", "bad", "release", "--json")
    if ($remoteYank.Code -ne 0) {
        Write-Output $remoteYank.Output
        throw "expected HTTP registry yank --json to pass"
    }
    $remoteYankReport = $remoteYank.Output | ConvertFrom-Json
    if ($remoteYankReport.schema -ne "sura.registry.yank.v1" -or
        $remoteYankReport.source -ne "http" -or
        $remoteYankReport.name -ne "report_pkg" -or
        $remoteYankReport.version -ne "0.1.0" -or
        $remoteYankReport.yanked -ne $true -or
        $remoteYankReport.reason -ne "bad release" -or
        $remoteYankReport.by -ne "admin") {
        throw "expected HTTP yank --json to report admin-yanked package state"
    }
    $remoteYanks = Run-Pkg -PkgArgs @("yanks", "report_pkg", "--json")
    if ($remoteYanks.Code -ne 0) {
        Write-Output $remoteYanks.Output
        throw "expected HTTP registry yanks --json to pass"
    }
    $remoteYanksReport = $remoteYanks.Output | ConvertFrom-Json
    $remoteYankItems = @($remoteYanksReport.yanks)
    if ($remoteYanksReport.schema -ne "sura.registry.yanks.v1" -or
        $remoteYanksReport.source -ne "http" -or
        $remoteYanksReport.query -ne "report_pkg" -or
        $remoteYanksReport.count -ne 1 -or
        $remoteYankItems[0].name -ne "report_pkg" -or
        $remoteYankItems[0].version -ne "0.1.0" -or
        $remoteYankItems[0].reason -ne "bad release" -or
        $remoteYankItems[0].by -ne "admin") {
        throw "expected HTTP yanks --json to list admin-yanked package state"
    }
    $remoteYankedSearch = Run-Pkg -PkgArgs @("search", "report_pkg", "--json")
    if ($remoteYankedSearch.Code -ne 0) {
        Write-Output $remoteYankedSearch.Output
        throw "expected search after HTTP yank to pass"
    }
    $remoteYankedSearchReport = $remoteYankedSearch.Output | ConvertFrom-Json
    if ($remoteYankedSearchReport.packages[0].yanked -ne $true) {
        throw "expected search --json to expose HTTP yanked package metadata"
    }
    $remoteUnyank = Run-Pkg -PkgArgs @("unyank", "report_pkg@0.1.0", "--json")
    if ($remoteUnyank.Code -ne 0) {
        Write-Output $remoteUnyank.Output
        throw "expected HTTP registry unyank --json to pass"
    }
    $remoteUnyankReport = $remoteUnyank.Output | ConvertFrom-Json
    if ($remoteUnyankReport.schema -ne "sura.registry.yank.v1" -or
        $remoteUnyankReport.source -ne "http" -or
        $remoteUnyankReport.yanked -ne $false) {
        throw "expected HTTP unyank --json to report restored package state"
    }

    $report = Run-Pkg -PkgArgs @("report", "report_pkg@0.1.0", "suspicious", "package", "behavior", "seen", "--json")
    if ($report.Code -ne 0) {
        Write-Output $report.Output
        throw "expected surapkg report --json to pass"
    }
    $submitReport = $report.Output | ConvertFrom-Json
    if ($submitReport.schema -ne "sura.registry.report.v1" -or
        -not $submitReport.passed -or
        $submitReport.source -ne "http" -or
        $submitReport.name -ne "report_pkg" -or
        $submitReport.version -ne "0.1.0" -or
        $submitReport.status -ne "open" -or
        [string]::IsNullOrWhiteSpace($submitReport.id)) {
        Write-Output $report.Output
        throw "expected HTTP report --json to expose submitted report metadata"
    }

    $koreanReasonBytes = [byte[]](0xED,0x95,0x9C,0xEA,0xB5,0xAD,0xEC,0x96,0xB4,0x20,0xEC,0x8B,0xA0,0xEA,0xB3,0xA0,0x20,0xEB,0x82,0xB4,0xEC,0x9A,0xA9,0x20,0xED,0x85,0x8C,0xEC,0x8A,0xA4,0xED,0x8A,0xB8)
    $koreanReason = [System.Text.Encoding]::UTF8.GetString($koreanReasonBytes)
    $apiReport = Post-Json -Uri "${url}/api/report" -Body @{
        name = "report_pkg"
        version = "0.1.0"
        reason = $koreanReason
        contact = "qa@example.com"
        source = "smoke"
    }
    if (-not $apiReport.ok -or -not $apiReport.id) {
        throw "expected direct API report to pass"
    }

    $reports = Invoke-RestMethod -Uri "${url}/api/reports" -Headers @{ Authorization = "Bearer admin-token" } -Method Get
    if (-not $reports.reports -or $reports.reports.Count -lt 2) {
        throw "expected admin report listing to include reports"
    }

    $env:SURA_REGISTRY_TOKEN = "admin-token"
    $queue = Run-Pkg -PkgArgs @("reports", "open")
    if ($queue.Code -ne 0 -or
        $queue.Output -notmatch "Reports" -or
        $queue.Output -notmatch "report_pkg@0\.1\.0" -or
        $queue.Output -notmatch "open") {
        Write-Output $queue.Output
        throw "expected surapkg reports to list open registry reports"
    }
    $queueJson = Run-Pkg -PkgArgs @("reports", "open", "--json")
    if ($queueJson.Code -ne 0) {
        Write-Output $queueJson.Output
        throw "expected surapkg reports --json to pass"
    }
    $queueReport = $queueJson.Output | ConvertFrom-Json
    $queueReports = @($queueReport.reports)
    if ($queueReport.schema -ne "sura.registry.reports.v1" -or
        $queueReport.status -ne "open" -or
        $queueReport.count -lt 2 -or
        $queueReports[0].name -ne "report_pkg" -or
        $queueReports[0].status -ne "open" -or
        [string]::IsNullOrWhiteSpace($queueReports[0].reason)) {
        throw "expected surapkg reports --json to include open report metadata"
    }

    $queueId = $reports.reports[0].id
    $reviewCli = Run-Pkg -PkgArgs @("review-report", $queueId, "reviewing", "triage", "started")
    if ($reviewCli.Code -ne 0 -or $reviewCli.Output -notmatch "reviewed report $queueId -> reviewing") {
        Write-Output $reviewCli.Output
        throw "expected surapkg review-report to update report status"
    }
    $reviewJson = Run-Pkg -PkgArgs @("review-report", $queueId, "reviewing", "triage", "started", "--json")
    if ($reviewJson.Code -ne 0) {
        Write-Output $reviewJson.Output
        throw "expected review-report --json to pass"
    }
    $reviewCliReport = $reviewJson.Output | ConvertFrom-Json
    if ($reviewCliReport.schema -ne "sura.registry.review_report.v1" -or
        $reviewCliReport.ok -ne $true -or
        $reviewCliReport.report.id -ne $queueId -or
        $reviewCliReport.report.status -ne "reviewing" -or
        $reviewCliReport.report.review_note -ne "triage started" -or
        $reviewCliReport.report.reviewed_by -ne "admin") {
        throw "expected review-report --json to include reviewed report state"
    }

    $review = Post-Json -Uri "${url}/api/reports/review" -Token "admin-token" -Body @{
        id = $apiReport.id
        status = "actioned"
        note = "reviewed in smoke"
    }
    if (-not $review.ok -or $review.report.status -ne "actioned") {
        throw "expected report review to update status"
    }

    $reportsRaw = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $registry "reports.json")
    if (-not $reportsRaw.Contains($koreanReason) -or $reportsRaw -notmatch '"status": "actioned"') {
        throw "expected reports.json to preserve Korean text and review status"
    }

    $missing = Run-Pkg -PkgArgs @("report", "missing_pkg@1.0.0", "missing", "package", "should", "fail")
    if ($missing.Code -eq 0) {
        Write-Output $missing.Output
        throw "expected report for missing package to fail"
    }

    "registry_report_smoke: PASS"
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
