param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_audit_report_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Audit {
    param([string]$Path, [string]$Report, [string]$Sarif)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $args = @("audit", $Path, "--json", $Report)
    if ($Sarif) {
        $args += @("--sarif", $Sarif)
    }
    $out = & $Surapkg @args 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $risky = Join-Path $temp "risky_package"
    Write-Text (Join-Path $risky "sura.pkg.json") @"
{
  "name": "risky_package",
  "version": "0.1.0",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $risky "src/main.sura") @"
func fetch_status() do
  return http_get("https://example.com/status")
end
"@

    $riskyReport = Join-Path $temp "risky-audit.json"
    $riskySarif = Join-Path $temp "risky-audit.sarif"
    $riskyResult = Run-Audit -Path $risky -Report $riskyReport -Sarif $riskySarif
    if ($riskyResult.Code -eq 0 -or -not (Test-Path -LiteralPath $riskyReport) -or -not (Test-Path -LiteralPath $riskySarif)) {
        Write-Output $riskyResult.Output
        throw "expected risky package audit JSON and SARIF reports to be written with a failing exit code"
    }
    $riskyJson = Get-Content -Raw -Path $riskyReport | ConvertFrom-Json
    if ($riskyJson.passed -ne $false -or [int]$riskyJson.finding_count -lt 1) {
        Write-Output $riskyResult.Output
        throw "expected risky audit JSON to report failed findings"
    }
    $networkFindings = @()
    foreach ($finding in @($riskyJson.findings)) {
        $lineNumber = [int]$finding.line
        if (($finding.kind -eq "network access") -and
            ($finding.message -match "network access") -and
            ($finding.file -match "src[\\/]main\.sura") -and
            ($lineNumber -gt 0)) {
            $networkFindings += $finding
        }
    }
    if ($networkFindings.Count -lt 1) {
        $riskyJson | ConvertTo-Json -Depth 8
        throw "expected structured network access finding in risky audit JSON"
    }
    $riskySarifJson = Get-Content -Raw -Path $riskySarif | ConvertFrom-Json
    $sarifResults = @($riskySarifJson.runs[0].results)
    $sarifRules = @($riskySarifJson.runs[0].tool.driver.rules)
    $networkSarif = @()
    foreach ($result in $sarifResults) {
        if (($result.ruleId -eq "sura.audit.network-access") -and
            ($result.message.text -match "network access") -and
            ($result.locations[0].physicalLocation.artifactLocation.uri -match "src/main\.sura") -and
            ([int]$result.locations[0].physicalLocation.region.startLine -gt 0)) {
            $networkSarif += $result
        }
    }
    if ($riskySarifJson.version -ne "2.1.0" -or $sarifRules.Count -lt 1 -or $networkSarif.Count -lt 1) {
        $riskySarifJson | ConvertTo-Json -Depth 12
        throw "expected SARIF audit report to expose network-access rule and source location"
    }

    $safe = Join-Path $temp "safe_agent"
    Write-Text (Join-Path $safe "sura.pkg.json") @"
{
  "name": "safe_agent",
  "version": "0.1.0",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $safe "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_get"],
  "url_prefixes": ["file://"],
  "approval": true,
  "approval_token": "ci-token",
  "allow_shell": false,
  "command_prefixes": []
}
"@
    Write-Text (Join-Path $safe "src/main.sura") @"
func read_context(path) do
  policy is {tools: ["http_get"], url_prefixes: ["file://"], approval: true}
  spec is {name: "http_get", url: "file://" + path}
  return tool_call_policy(spec, policy)
end
"@

    $safeReport = Join-Path $temp "safe-audit.json"
    $safeSarif = Join-Path $temp "safe-audit.sarif"
    $safeResult = Run-Audit -Path $safe -Report $safeReport -Sarif $safeSarif
    if ($safeResult.Code -ne 0 -or -not (Test-Path -LiteralPath $safeReport) -or -not (Test-Path -LiteralPath $safeSarif)) {
        Write-Output $safeResult.Output
        throw "expected safe package audit JSON and SARIF reports to pass"
    }
    $safeJson = Get-Content -Raw -Path $safeReport | ConvertFrom-Json
    $safeFindings = @($safeJson.findings)
    if ($safeJson.passed -ne $true -or [int]$safeJson.finding_count -ne 0 -or $safeFindings.Count -ne 0) {
        $safeJson | ConvertTo-Json -Depth 8
        throw "expected safe audit JSON to contain no findings"
    }
    $safeSarifJson = Get-Content -Raw -Path $safeSarif | ConvertFrom-Json
    $safeSarifResults = @($safeSarifJson.runs[0].results)
    if ($safeSarifJson.version -ne "2.1.0" -or $safeSarifResults.Count -ne 0) {
        $safeSarifJson | ConvertTo-Json -Depth 12
        throw "expected safe audit SARIF to contain no results"
    }

    "audit_report_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
