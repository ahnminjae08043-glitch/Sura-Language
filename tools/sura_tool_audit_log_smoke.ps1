param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Surapkg = (Join-Path (Split-Path -Parent $PSScriptRoot) "surapkg.exe")
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_tool_audit_" + [System.Guid]::NewGuid().ToString("N"))

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Clear-ToolEnv {
    Remove-Item Env:SURA_TOOL_AUTO_APPROVE -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_INTERACTIVE_APPROVAL -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL_COMMAND -ErrorAction SilentlyContinue
}

function Run-Sura {
    param([string]$Script)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Engine $Script 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Surapkg @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    if (-not (Test-Path -LiteralPath $Engine)) {
        throw "Sura engine not found: $Engine"
    }
    if (-not (Test-Path -LiteralPath $Surapkg)) {
        throw "surapkg not found: $Surapkg"
    }
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $toolFile = Join-Path $temp "tool_context.txt"
    $korean = -join ([char[]](0xAC10, 0xC0AC, 0x0020, 0xB85C, 0xADF8))
    [System.IO.File]::WriteAllText($toolFile, $korean, $utf8NoBom)
    $toolUrlPath = $toolFile -replace "\\", "/"
    $auditLog = Join-Path $temp "tool-audit.jsonl"
    $cleanAuditLog = Join-Path $temp "tool-audit-clean.jsonl"
    $summaryReportPath = Join-Path $temp "tool-log-report.json"
    $failingReportPath = Join-Path $temp "tool-log-failing-report.json"
    $cleanReportPath = Join-Path $temp "tool-log-clean-report.json"

    $blockedScript = Join-Path $temp "blocked.sura"
    Write-Text $blockedScript @"
spec is tool_spec("http_get", {url: "file://$toolUrlPath"})
policy is {tools: ["http_get"], url_prefixes: ["https://blocked"], approval: false}
tool_call_policy(spec, policy)
"@

    $approvalScript = Join-Path $temp "approval.sura"
    Write-Text $approvalScript @"
spec is tool_spec("http_get", {url: "file://$toolUrlPath"})
policy is {tools: ["http_get"], url_prefixes: ["file://"], approval: true, approval_token: "audit-token", approval_message: "$korean"}
tool_call_policy(spec, policy)
"@

    Clear-ToolEnv
    $env:SURA_TOOL_AUDIT_LOG = $auditLog

    $blocked = Run-Sura $blockedScript
    if ($blocked.Code -eq 0 -or $blocked.Output -notmatch "outside policy") {
        Write-Output $blocked.Output
        throw "expected policy-denied call to fail"
    }

    $missingApproval = Run-Sura $approvalScript
    if ($missingApproval.Code -eq 0 -or $missingApproval.Output -notmatch "approval required") {
        Write-Output $missingApproval.Output
        throw "expected approval-denied call to fail"
    }

    $env:SURA_TOOL_APPROVAL_TOKEN = "audit-token"
    $approved = Run-Sura $approvalScript
    if ($approved.Code -ne 0) {
        Write-Output $approved.Output
        throw "expected approved call to pass"
    }

    if (-not (Test-Path -LiteralPath $auditLog)) {
        throw "expected audit log to be created"
    }
    $log = [System.IO.File]::ReadAllText($auditLog, [System.Text.Encoding]::UTF8)
    foreach ($event in @("policy_denied", "policy_allowed", "approval_denied", "approval_granted", "executed")) {
        if ($log -notmatch ('"event"\s*:\s*"' + [regex]::Escape($event) + '"')) {
            Write-Output $log
            throw "expected audit log event '$event'"
        }
    }
    if ($log -notmatch '"approvalTokenConfigured"\s*:\s*true' -or
        $log -notmatch '"approvalRequired"\s*:\s*true' -or
        $log -notmatch [regex]::Escape($korean)) {
        Write-Output $log
        throw "expected audit log to include approval metadata and UTF-8 approval message"
    }

    $summary = Run-Pkg -PkgArgs @("tool-log", $auditLog, "--tail", "10", "--json", $summaryReportPath)
    if ($summary.Code -ne 0 -or
        $summary.Output -notmatch "Sura tool audit log" -or
        $summary.Output -notmatch "policy_denied:\s+1" -or
        $summary.Output -notmatch "policy_allowed:\s+2" -or
        $summary.Output -notmatch "approval_denied:\s+1" -or
        $summary.Output -notmatch "executed:\s+1" -or
        $summary.Output -notmatch "denied_or_failed:\s+2") {
        Write-Output $summary.Output
        throw "expected tool-log summary to include event counts"
    }
    if (-not (Test-Path -LiteralPath $summaryReportPath)) {
        throw "expected tool-log JSON report"
    }
    $summaryReport = Get-Content -LiteralPath $summaryReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($summaryReport.schema -ne "sura.tool_log.summary.v1" -or
        -not $summaryReport.passed -or
        $summaryReport.fail_on_denied -or
        [int]$summaryReport.event_count -lt 5 -or
        [int]$summaryReport.bad_event_count -ne 2 -or
        [int]$summaryReport.counts.policy_denied -ne 1 -or
        [int]$summaryReport.counts.policy_allowed -ne 2 -or
        [int]$summaryReport.counts.approval_denied -ne 1 -or
        [int]$summaryReport.counts.executed -ne 1 -or
        @($summaryReport.recent_events).Count -lt 5) {
        Get-Content -LiteralPath $summaryReportPath -Raw -Encoding UTF8 | Write-Output
        throw "expected valid tool-log JSON report"
    }

    $failingSummary = Run-Pkg -PkgArgs @("tool-log", $auditLog, "--fail-on-denied", "--json", $failingReportPath)
    if ($failingSummary.Code -eq 0 -or $failingSummary.Output -notmatch "denied or failed") {
        Write-Output $failingSummary.Output
        throw "expected tool-log --fail-on-denied to fail on denied events"
    }
    if (-not (Test-Path -LiteralPath $failingReportPath)) {
        throw "expected failing tool-log JSON report"
    }
    $failingReport = Get-Content -LiteralPath $failingReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($failingReport.schema -ne "sura.tool_log.summary.v1" -or
        $failingReport.passed -or
        -not $failingReport.fail_on_denied -or
        [int]$failingReport.bad_event_count -ne 2) {
        Get-Content -LiteralPath $failingReportPath -Raw -Encoding UTF8 | Write-Output
        throw "expected failing tool-log JSON report to mark denied gate"
    }

    Remove-Item Env:SURA_TOOL_APPROVAL_TOKEN -ErrorAction SilentlyContinue
    $env:SURA_TOOL_AUDIT_LOG = $cleanAuditLog
    $env:SURA_TOOL_APPROVAL_TOKEN = "audit-token"
    $cleanRun = Run-Sura $approvalScript
    if ($cleanRun.Code -ne 0) {
        Write-Output $cleanRun.Output
        throw "expected clean approved call to pass"
    }
    $cleanSummary = Run-Pkg -PkgArgs @("tool-log", $cleanAuditLog, "--fail-on-denied", "--json", $cleanReportPath)
    if ($cleanSummary.Code -ne 0 -or
        $cleanSummary.Output -notmatch "denied_or_failed:\s+0" -or
        $cleanSummary.Output -notmatch "approval_granted:\s+1" -or
        $cleanSummary.Output -notmatch "executed:\s+1") {
        Write-Output $cleanSummary.Output
        throw "expected clean tool-log summary to pass"
    }
    $cleanReport = Get-Content -LiteralPath $cleanReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($cleanReport.schema -ne "sura.tool_log.summary.v1" -or
        -not $cleanReport.passed -or
        -not $cleanReport.fail_on_denied -or
        [int]$cleanReport.bad_event_count -ne 0 -or
        [int]$cleanReport.counts.executed -ne 1) {
        Get-Content -LiteralPath $cleanReportPath -Raw -Encoding UTF8 | Write-Output
        throw "expected clean tool-log JSON report to pass"
    }

    "tool_audit_log_smoke: PASS"
}
finally {
    Clear-ToolEnv
    Remove-Item Env:SURA_TOOL_AUDIT_LOG -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
