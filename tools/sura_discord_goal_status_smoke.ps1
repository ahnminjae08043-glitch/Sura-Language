param()

$ErrorActionPreference = "Stop"

$script = Join-Path $PSScriptRoot "sura_discord_goal_status.ps1"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_discord_goal_status_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$powerShellExe = (Get-Command pwsh -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if (-not $powerShellExe) {
    $powerShellExe = (Get-Command powershell -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source)
}

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Json-String {
    param([string]$Escaped)
    return ($Escaped | ConvertFrom-Json)
}

$titleText = Json-String '"Sura \uc804\uccb4 \uace8 \uc9c4\ud589\ub3c4"'
$remainingText = Json-String '"\uc804\uccb4 \uace8\uae4c\uc9c0 \ub0a8\uc740 \uc791\uc5c5"'
$nativeText = Json-String '"Native \ubaa9\ud45c"'
$mojibakePattern = ([string][char]0x00ec) + "|" + ([string][char]0x00eb) + "|" + ([string][char]0x00ea)

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $goalPath = Join-Path $temp "goal_audit.json"
    $nativePath = Join-Path $temp "native_perf.json"
    $payloadPath = Join-Path $temp "discord_payload.json"

    $goalAudit = [ordered]@{
        schema = "sura.goal.audit.v1"
        generated_utc = "2026-05-18T00:00:00Z"
        passed = $false
        status = "INCOMPLETE"
        progress_percent = 97.4
        required_count = 39
        passed_count = 38
        failed_count = 1
        blocker_count = 1
        max_native_ratio = 10.0
        remaining_work = @([ordered]@{
            category = "performance"
            id = "native_cpp_speed_goal"
            requirement = "Rust/C++-class native speed proof"
            message = "Sura/native ratio exceeds target"
            next_action = "optimize JIT/AOT hot loops"
        })
        blockers = @([ordered]@{
            category = "performance"
            id = "native_cpp_speed_goal"
            requirement = "Rust/C++-class native speed proof"
            message = "Sura/native ratio exceeds target"
            next_action = "optimize JIT/AOT hot loops"
        })
    }
    $nativePerf = [ordered]@{
        schema = "sura.native.performance.v1"
        passed = $true
        sura_jit_ms = 521.4
        native_ms = 1.0
        sura_native_ratio = 521.4
    }
    Write-Text $goalPath ($goalAudit | ConvertTo-Json -Depth 8)
    Write-Text $nativePath ($nativePerf | ConvertTo-Json -Depth 8)

    $out = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $script -GoalAuditJson $goalPath -NativePerfJson $nativePath -Out $payloadPath -DryRun 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $out -notmatch "discord_goal_status:\s+DRY-RUN") {
        Write-Output $out
        throw "expected dry-run Discord status generation to pass"
    }
    if (-not (Test-Path -LiteralPath $payloadPath)) {
        throw "expected Discord payload JSON"
    }

    $payloadText = [System.IO.File]::ReadAllText($payloadPath, [System.Text.Encoding]::UTF8)
    $payload = $payloadText | ConvertFrom-Json
    if ($payload.username -ne "Sura Goal Bot" -or
        -not $payload.content.Contains($titleText) -or
        -not $payload.content.Contains($remainingText) -or
        $payload.content -notmatch "97\.4%" -or
        $payload.content -notmatch "native_cpp_speed_goal" -or
        -not $payload.content.Contains($nativeText) -or
        $payload.content -match $mojibakePattern) {
        Write-Output $payloadText
        throw "unexpected Discord payload content"
    }

    $skipOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $script -GoalAuditJson (Join-Path $temp "missing.json") -AllowMissing -DryRun 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $skipOut -notmatch "goal audit missing") {
        Write-Output $skipOut
        throw "expected missing goal audit to be skipped with -AllowMissing"
    }

    "discord_goal_status_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
