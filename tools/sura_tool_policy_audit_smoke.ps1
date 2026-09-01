param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_tool_policy_audit_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Audit {
    param([string]$Path)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Surapkg audit $Path 2>&1 | ForEach-Object { "$_" }
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
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $valid = Join-Path $temp "valid_agent"
    Write-Text (Join-Path $valid "sura.pkg.json") @"
{
  "name": "valid_agent",
  "version": "0.1.0",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $valid "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_get", "http_request"],
  "url_prefixes": ["file://"],
  "http_methods": ["GET"],
  "allowed_headers": ["X-Agent"],
  "required_headers": {"X-Agent": "sura"},
  "max_timeout": 30,
  "max_body_bytes": 0,
  "approval": true,
  "approval_token": "ci-token",
  "approval_message": "Read local context",
  "allow_shell": false,
  "command_prefixes": []
}
"@
    Write-Text (Join-Path $valid "src/main.sura") @"
func read_context(path) do
  policy is {tools: ["http_get"], url_prefixes: ["file://"], allow_shell: false}
  spec is {name: "http_get", url: "file://" + path}
  return tool_call_policy(spec, policy)
end

func read_context_request(path) do
  policy is {tools: ["http_request"], url_prefixes: ["file://"], http_methods: ["GET"], allow_shell: false}
  spec is {name: "http_request", url: "file://" + path, method: "GET", headers: {"X-Agent": "sura"}}
  return tool_call_policy(spec, policy)
end
"@

    $signPolicy = Run-Pkg -PkgArgs @("sign-policy", $valid)
    if ($signPolicy.Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $valid "sura.tools.sig"))) {
        Write-Output $signPolicy.Output
        throw "expected tool policy signing to pass"
    }
    $verifyPolicy = Run-Pkg -PkgArgs @("verify-policy", $valid)
    if ($verifyPolicy.Code -ne 0 -or $verifyPolicy.Output -notmatch "tool policy signature verified") {
        Write-Output $verifyPolicy.Output
        throw "expected tool policy verification to pass"
    }

    $ok = Run-Audit $valid
    if ($ok.Code -ne 0) {
        Write-Output $ok.Output
        throw "expected valid tool policy package audit to pass"
    }

    $tampered = Join-Path $temp "tampered_policy"
    Copy-Item -LiteralPath $valid -Destination $tampered -Recurse
    $tamperedPolicyPath = Join-Path $tampered "sura.tools.json"
    $tamperedText = [System.IO.File]::ReadAllText($tamperedPolicyPath, [System.Text.Encoding]::UTF8).Replace('"file://"', '"file:///tmp/"')
    [System.IO.File]::WriteAllText($tamperedPolicyPath, $tamperedText, $utf8NoBom)
    $tamperedVerify = Run-Pkg -PkgArgs @("verify-policy", $tampered)
    if ($tamperedVerify.Code -eq 0 -or $tamperedVerify.Output -notmatch "hash mismatch") {
        Write-Output $tamperedVerify.Output
        throw "expected tampered tool policy signature verification to fail"
    }
    $tamperedAudit = Run-Audit $tampered
    if ($tamperedAudit.Code -eq 0 -or $tamperedAudit.Output -notmatch "hash mismatch") {
        Write-Output $tamperedAudit.Output
        throw "expected tampered signed tool policy audit to fail"
    }

    $badUrl = Join-Path $temp "bad_url"
    Copy-Item -LiteralPath $valid -Destination $badUrl -Recurse
    Write-Text (Join-Path $badUrl "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_get"],
  "url_prefixes": ["ftp://"],
  "allow_shell": false,
  "command_prefixes": []
}
"@
    $badUrlResult = Run-Audit $badUrl
    if ($badUrlResult.Code -eq 0 -or $badUrlResult.Output -notmatch "url prefix") {
        Write-Output $badUrlResult.Output
        throw "expected invalid URL prefix tool policy to fail audit"
    }

    $badShell = Join-Path $temp "bad_shell"
    Copy-Item -LiteralPath $valid -Destination $badShell -Recurse
    Write-Text (Join-Path $badShell "sura.tools.json") @"
{
  "version": 1,
  "tools": ["shell"],
  "allow_shell": true,
  "command_prefixes": []
}
"@
    $badShellResult = Run-Audit $badShell
    if ($badShellResult.Code -eq 0 -or $badShellResult.Output -notmatch "command_prefixes") {
        Write-Output $badShellResult.Output
        throw "expected shell policy without command prefixes to fail audit"
    }

    $badHttpMethod = Join-Path $temp "bad_http_method"
    Copy-Item -LiteralPath $valid -Destination $badHttpMethod -Recurse
    Write-Text (Join-Path $badHttpMethod "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_request"],
  "url_prefixes": ["file://"],
  "http_methods": ["GET!"],
  "allow_shell": false,
  "command_prefixes": []
}
"@
    $badHttpMethodResult = Run-Audit $badHttpMethod
    if ($badHttpMethodResult.Code -eq 0 -or $badHttpMethodResult.Output -notmatch "http method") {
        Write-Output $badHttpMethodResult.Output
        throw "expected invalid HTTP method policy to fail audit"
    }

    $badHeaderPolicy = Join-Path $temp "bad_header_policy"
    Copy-Item -LiteralPath $valid -Destination $badHeaderPolicy -Recurse
    Write-Text (Join-Path $badHeaderPolicy "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_request"],
  "url_prefixes": ["file://"],
  "http_methods": ["GET"],
  "allowed_headers": ["Bad Header"],
  "required_headers": {"X-Agent": "sura"},
  "max_timeout": -1,
  "allow_shell": false,
  "command_prefixes": []
}
"@
    $badHeaderResult = Run-Audit $badHeaderPolicy
    if ($badHeaderResult.Code -eq 0 -or $badHeaderResult.Output -notmatch "allowed_headers|max_timeout") {
        Write-Output $badHeaderResult.Output
        throw "expected invalid HTTP header/timeout constraints to fail audit"
    }

    $badApprovalPolicy = Join-Path $temp "bad_approval_policy"
    Copy-Item -LiteralPath $valid -Destination $badApprovalPolicy -Recurse
    Write-Text (Join-Path $badApprovalPolicy "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_get"],
  "url_prefixes": ["file://"],
  "approval": false,
  "approval_token": "orphan-token",
  "allow_shell": false,
  "command_prefixes": []
}
"@
    $badApprovalResult = Run-Audit $badApprovalPolicy
    if ($badApprovalResult.Code -eq 0 -or $badApprovalResult.Output -notmatch "approval_token") {
        Write-Output $badApprovalResult.Output
        throw "expected orphan approval token policy to fail audit"
    }

    $missingManifest = Join-Path $temp "missing_manifest"
    Copy-Item -LiteralPath $valid -Destination $missingManifest -Recurse
    Remove-Item -LiteralPath (Join-Path $missingManifest "sura.tools.json") -Force
    $missingResult = Run-Audit $missingManifest
    if ($missingResult.Code -eq 0 -or $missingResult.Output -notmatch "sura.tools.json") {
        Write-Output $missingResult.Output
        throw "expected tool_call_policy without sura.tools.json to fail audit"
    }

    $directTool = Join-Path $temp "direct_tool"
    Copy-Item -LiteralPath $valid -Destination $directTool -Recurse
    Write-Text (Join-Path $directTool "src/main.sura") @"
func read_context(path) do
  return tool http_request {url: "file://" + path}
end
"@
    $directResult = Run-Audit $directTool
    if ($directResult.Code -eq 0 -or $directResult.Output -notmatch "direct tool call") {
        Write-Output $directResult.Output
        throw "expected direct tool call to fail audit"
    }

    "tool_policy_audit_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# This gate printed PASS while exiting nonzero: its last native command
# was a negative check that correctly failed, and the script inherited
# that code. CI reads the exit code, so a passing gate reported failure.
exit 0
