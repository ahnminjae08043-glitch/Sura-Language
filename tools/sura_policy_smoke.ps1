param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_policy_" + [System.Guid]::NewGuid().ToString("N"))
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

    $pkg = Join-Path $temp "policy_pkg"
    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "policy_pkg",
  "version": "1.0.0",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/main.sura") @"
use tool

func read_local(path) do
  spec is tool.spec("http_request", {method: "GET", url: "file://" + path, headers: {"X-Agent": "policy-smoke"}, timeout: 15})
  policy is {tools: ["http_request"], url_prefixes: ["file://"], http_methods: ["GET"], allowed_headers: ["X-Agent"], allow_shell: false}
  return tool.call_policy(spec, policy)
end

func read_status do
  spec is tool_spec("http_get", {url: "https://api.example.com/status"})
  policy is {tools: ["http_get"], url_prefixes: ["https://api.example.com/"], allow_shell: false}
  return tool_call_policy(spec, policy)
end
"@

    $policyReportPath = Join-Path $temp "policy-report.json"
    $policy = Run-Pkg -PkgArgs @("policy", $pkg, "--json", $policyReportPath)
    if ($policy.Code -ne 0 -or $policy.Output -notmatch "wrote tool policy") {
        Write-Output $policy.Output
        throw "expected policy generation to pass"
    }
    if (-not (Test-Path -LiteralPath $policyReportPath)) {
        throw "expected policy generation to write a JSON report"
    }
    $policyPath = Join-Path $pkg "sura.tools.json"
    if (-not (Test-Path -LiteralPath $policyPath)) {
        throw "expected sura.tools.json to be generated"
    }
    $policyReport = Get-Content -Raw -Path $policyReportPath | ConvertFrom-Json
    if ($policyReport.schema -ne "sura.package.policy.v1" -or
        $policyReport.package -ne "policy_pkg" -or
        $policyReport.version -ne "1.0.0" -or
        $policyReport.passed -ne $true -or
        $policyReport.policy -notmatch "sura\.tools\.json" -or
        @($policyReport.tools).Count -ne 2 -or
        $policyReport.tools -notcontains "http_get" -or
        $policyReport.tools -notcontains "http_request" -or
        $policyReport.url_prefixes -notcontains "file://" -or
        $policyReport.url_prefixes -notcontains "https://api.example.com/" -or
        $policyReport.http_methods -notcontains "GET" -or
        $policyReport.allowed_headers -notcontains "X-Agent" -or
        $policyReport.shell_seen -ne $false -or
        $policyReport.requires_manual_command_prefixes -ne $false) {
        $policyReport | ConvertTo-Json -Depth 8 | Write-Output
        throw "expected policy --json to report inferred tool policy"
    }
    $policyText = Get-Content -Raw -Path $policyPath
    if ($policyText -notmatch '"http_get"' -or
        $policyText -notmatch '"http_request"' -or
        $policyText -notmatch '"file://"' -or
        $policyText -notmatch '"https://api\.example\.com/"' -or
        $policyText -notmatch '"http_methods"\s*:\s*\[\s*"GET"\s*\]' -or
        $policyText -notmatch '"allowed_headers"\s*:\s*\[\s*"X-Agent"\s*\]' -or
        $policyText -notmatch '"allow_shell"\s*:\s*false') {
        Write-Output $policyText
        throw "generated tool policy did not include inferred tools, prefixes, methods, and headers"
    }

    $audit = Run-Pkg -PkgArgs @("audit", $pkg)
    if ($audit.Code -ne 0 -or $audit.Output -notmatch "audit passed") {
        Write-Output $audit.Output
        throw "expected generated tool policy package audit to pass"
    }
    $signReportPath = Join-Path $temp "sign-policy-report.json"
    $sign = Run-Pkg -PkgArgs @("sign-policy", $pkg, "--json", $signReportPath)
    if ($sign.Code -ne 0) {
        Write-Output $sign.Output
        throw "expected generated tool policy signing to pass"
    }
    $signReport = Get-Content -Raw -Path $signReportPath | ConvertFrom-Json
    if ($signReport.schema -ne "sura.package.sign_policy.v1" -or
        $signReport.passed -ne $true -or
        $signReport.manifest -notmatch "sura\.tools\.json" -or
        $signReport.signature -notmatch "sura\.tools\.sig" -or
        $signReport.source -ne "sura.tools.json" -or
        $signReport.algorithm -ne "sha256-tool-policy-v1" -or
        -not $signReport.hash) {
        throw "expected sign-policy --json to report signed tool policy"
    }
    $verifyReportPath = Join-Path $temp "verify-policy-report.json"
    $verify = Run-Pkg -PkgArgs @("verify-policy", $pkg, "--json", $verifyReportPath)
    if ($verify.Code -ne 0 -or $verify.Output -notmatch "tool policy signature verified") {
        Write-Output $verify.Output
        throw "expected generated tool policy verification to pass"
    }
    $verifyReport = Get-Content -Raw -Path $verifyReportPath | ConvertFrom-Json
    if ($verifyReport.schema -ne "sura.package.verify_policy.v1" -or
        $verifyReport.passed -ne $true -or
        $verifyReport.manifest -notmatch "sura\.tools\.json" -or
        $verifyReport.signature -notmatch "sura\.tools\.sig" -or
        $verifyReport.source -ne "sura.tools.json" -or
        $verifyReport.algorithm -ne "sha256-tool-policy-v1" -or
        -not $verifyReport.expected_hash -or
        $verifyReport.expected_hash -ne $verifyReport.actual_hash) {
        throw "expected verify-policy --json to report signed tool policy verification"
    }

    $again = Run-Pkg -PkgArgs @("policy", $pkg)
    if ($again.Code -eq 0 -or $again.Output -notmatch "already exists") {
        Write-Output $again.Output
        throw "expected policy generation to refuse overwriting an existing policy"
    }

    $shellPkg = Join-Path $temp "shell_policy_pkg"
    Write-Text (Join-Path $shellPkg "sura.pkg.json") @"
{
  "name": "shell_policy_pkg",
  "version": "1.0.0",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $shellPkg "src/main.sura") @"
func run_shell do
  spec is tool_spec("shell", {command: "echo hi"})
  policy is {tools: ["shell"], allow_shell: true, command_prefixes: ["echo "]}
  return tool_call_policy(spec, policy)
end
"@
    $shellPolicy = Run-Pkg -PkgArgs @("policy", $shellPkg)
    if ($shellPolicy.Code -eq 0 -or $shellPolicy.Output -notmatch "manual command_prefixes") {
        Write-Output $shellPolicy.Output
        throw "expected shell policy generation to require manual command prefixes"
    }

    "policy_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
