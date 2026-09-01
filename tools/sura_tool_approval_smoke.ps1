param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe")
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_tool_approval_" + [System.Guid]::NewGuid().ToString("N"))

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Clear-ApprovalEnv {
    Remove-Item Env:SURA_TOOL_AUTO_APPROVE -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_INTERACTIVE_APPROVAL -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL_COMMAND -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL_REQUEST_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL_RESPONSE_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:SURA_TOOL_APPROVAL_FILE_TIMEOUT_MS -ErrorAction SilentlyContinue
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

function Start-ApprovalFileResponder {
    param([string]$RequestFile, [string]$ResponseFile, [string]$Decision)
    Start-Job -ArgumentList $RequestFile, $ResponseFile, $Decision -ScriptBlock {
        param([string]$RequestFile, [string]$ResponseFile, [string]$Decision)
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        $deadline = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath $RequestFile) {
                $raw = [System.IO.File]::ReadAllText($RequestFile, [System.Text.Encoding]::UTF8)
                if ($raw -match '"requestId"\s*:\s*"([^"]+)"') {
                    $requestId = $matches[1]
                    if ($raw -notmatch '"tool"\s*:\s*"http_get"' -or
                        $raw -notmatch '"target"\s*:\s*"file://' -or
                        $raw -notmatch '"approvalTokenConfigured"\s*:\s*true') {
                        exit 2
                    }
                    $response = [ordered]@{
                        version = 1
                        requestId = $requestId
                        decision = $Decision
                    } | ConvertTo-Json -Compress
                    [System.IO.File]::WriteAllText($ResponseFile, $response, $utf8NoBom)
                    exit 0
                }
            }
            Start-Sleep -Milliseconds 50
        }
        exit 1
    }
}

try {
    if (-not (Test-Path -LiteralPath $Engine)) {
        throw "Sura engine not found: $Engine"
    }
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $toolFile = Join-Path $temp "tool_context.txt"
    $korean = -join ([char[]](0xC2B9, 0xC778, 0x0020, 0xC644, 0xB8CC))
    [System.IO.File]::WriteAllText($toolFile, $korean, $utf8NoBom)
    $toolUrlPath = $toolFile -replace "\\", "/"

    $tokenScript = Join-Path $temp "token_required.sura"
    Write-Text $tokenScript @"
spec is tool_spec("http_get", {url: "file://$toolUrlPath"})
policy is {tools: ["http_get"], url_prefixes: ["file://"], approval: true, approval_token: "secret-token", approval_message: "Korean context read"}
assert(tool_allowed(spec, policy))
print tool_call_policy(spec, policy)
"@

    $autoScript = Join-Path $temp "auto_approval.sura"
    Write-Text $autoScript @"
spec is tool_spec("http_get", {url: "file://$toolUrlPath"})
policy is {tools: ["http_get"], url_prefixes: ["file://"], approval: true}
assert(tool_allowed(spec, policy))
print tool_call_policy(spec, policy)
"@

    $offScript = Join-Path $temp "approval_off.sura"
    Write-Text $offScript @"
spec is tool_spec("http_get", {url: "file://$toolUrlPath"})
policy is {tools: ["http_get"], url_prefixes: ["file://"], approval: false}
assert(tool_allowed(spec, policy))
print tool_call_policy(spec, policy)
"@

    $callback = Join-Path $temp "approval_callback.ps1"
    Write-Text $callback @"
if (`$env:SURA_TOOL_APPROVAL_TOOL -eq "http_get" -and
    `$env:SURA_TOOL_APPROVAL_TARGET -like "file://*" -and
    `$env:SURA_TOOL_APPROVAL_MESSAGE -eq "Korean context read" -and
    `$env:SURA_TOOL_APPROVAL_TOKEN_CONFIGURED -eq "1") {
    Write-Output "allow"
    exit 0
}
Write-Output "deny"
exit 3
"@

    $denyCallback = Join-Path $temp "approval_deny_callback.ps1"
    Write-Text $denyCallback @"
Write-Output "deny"
exit 0
"@

    Clear-ApprovalEnv
    $missing = Run-Sura $tokenScript
    if ($missing.Code -eq 0 -or $missing.Output -notmatch "approval required") {
        Write-Output $missing.Output
        throw "expected approval-required tool call to fail without token"
    }

    Clear-ApprovalEnv
    $env:SURA_TOOL_APPROVAL_TOKEN = "wrong-token"
    $wrong = Run-Sura $tokenScript
    if ($wrong.Code -eq 0 -or $wrong.Output -notmatch "approval required") {
        Write-Output $wrong.Output
        throw "expected approval-required tool call to fail with wrong token"
    }

    Clear-ApprovalEnv
    $env:SURA_TOOL_APPROVAL_TOKEN = "secret-token"
    $tokenOk = Run-Sura $tokenScript
    if ($tokenOk.Code -ne 0 -or $tokenOk.Output -notmatch [regex]::Escape($korean)) {
        Write-Output $tokenOk.Output
        throw "expected matching approval token to allow tool call"
    }

    Clear-ApprovalEnv
    $env:SURA_TOOL_AUTO_APPROVE = "1"
    $autoOk = Run-Sura $autoScript
    if ($autoOk.Code -ne 0 -or $autoOk.Output -notmatch [regex]::Escape($korean)) {
        Write-Output $autoOk.Output
        throw "expected SURA_TOOL_AUTO_APPROVE to allow tool call"
    }

    Clear-ApprovalEnv
    $env:SURA_TOOL_APPROVAL = "allow"
    $allowOk = Run-Sura $autoScript
    if ($allowOk.Code -ne 0 -or $allowOk.Output -notmatch [regex]::Escape($korean)) {
        Write-Output $allowOk.Output
        throw "expected SURA_TOOL_APPROVAL=allow to allow tokenless policy"
    }

    Clear-ApprovalEnv
    $env:SURA_TOOL_APPROVAL_COMMAND = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$callback`""
    $callbackOk = Run-Sura $tokenScript
    if ($callbackOk.Code -ne 0 -or $callbackOk.Output -notmatch [regex]::Escape($korean)) {
        Write-Output $callbackOk.Output
        throw "expected SURA_TOOL_APPROVAL_COMMAND to allow token policy through host callback"
    }

    $requestFile = Join-Path $temp "approval_request.json"
    $responseFile = Join-Path $temp "approval_response.json"
    Clear-ApprovalEnv
    $env:SURA_TOOL_APPROVAL_REQUEST_FILE = $requestFile
    $env:SURA_TOOL_APPROVAL_RESPONSE_FILE = $responseFile
    $env:SURA_TOOL_APPROVAL_FILE_TIMEOUT_MS = "5000"
    $fileAllowJob = Start-ApprovalFileResponder $requestFile $responseFile "allow"
    $fileOk = Run-Sura $tokenScript
    Wait-Job $fileAllowJob | Out-Null
    $fileAllowState = $fileAllowJob.State
    Remove-Job $fileAllowJob
    if ($fileAllowState -ne "Completed" -or $fileOk.Code -ne 0 -or $fileOk.Output -notmatch [regex]::Escape($korean)) {
        Write-Output $fileOk.Output
        throw "expected approval request/response files to allow token policy through host UI bridge"
    }

    Clear-ApprovalEnv
    Remove-Item -LiteralPath $requestFile, $responseFile -ErrorAction SilentlyContinue
    $env:SURA_TOOL_APPROVAL_REQUEST_FILE = $requestFile
    $env:SURA_TOOL_APPROVAL_RESPONSE_FILE = $responseFile
    $env:SURA_TOOL_APPROVAL_FILE_TIMEOUT_MS = "5000"
    $fileDenyJob = Start-ApprovalFileResponder $requestFile $responseFile "deny"
    $fileDenied = Run-Sura $tokenScript
    Wait-Job $fileDenyJob | Out-Null
    $fileDenyState = $fileDenyJob.State
    Remove-Job $fileDenyJob
    if ($fileDenyState -ne "Completed" -or $fileDenied.Code -eq 0 -or $fileDenied.Output -notmatch "approval file denied") {
        Write-Output $fileDenied.Output
        throw "expected approval request/response file denial to block tool call"
    }

    Clear-ApprovalEnv
    $env:SURA_TOOL_APPROVAL_COMMAND = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$denyCallback`""
    $callbackDenied = Run-Sura $tokenScript
    if ($callbackDenied.Code -eq 0 -or $callbackDenied.Output -notmatch "approval command denied") {
        Write-Output $callbackDenied.Output
        throw "expected SURA_TOOL_APPROVAL_COMMAND denial to block tool call"
    }

    Clear-ApprovalEnv
    $offOk = Run-Sura $offScript
    if ($offOk.Code -ne 0 -or $offOk.Output -notmatch [regex]::Escape($korean)) {
        Write-Output $offOk.Output
        throw "expected approval:false to preserve existing behavior"
    }

    "tool_approval_smoke: PASS"
}
finally {
    Clear-ApprovalEnv
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
