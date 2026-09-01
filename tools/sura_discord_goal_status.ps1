param(
    [string]$GoalAuditJson = "artifacts\goal_audit.json",
    [string]$NativePerfJson = "",
    [string]$WebhookUrl = $env:SURA_DISCORD_WEBHOOK,
    [string]$Out = "",
    [string]$Username = "Sura Goal Bot",
    [int]$MaxRemainingItems = 6,
    [switch]$DryRun,
    [switch]$AllowMissing
)

$ErrorActionPreference = "Stop"

function Resolve-InputPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path (Get-Location).Path $Path
}

function Read-Text {
    param([string]$Path)
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Read-JsonFile {
    param([string]$Path)
    return (Read-Text $Path) | ConvertFrom-Json
}

function As-Array {
    param($Items)
    if ($null -eq $Items) { return @() }
    if ($Items -is [string]) { return @($Items) }
    if ($Items -is [System.Collections.IEnumerable]) {
        $out = @()
        foreach ($item in $Items) { $out += $item }
        return $out
    }
    return @($Items)
}

function Get-PropertyValue {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function One-Line {
    param($Value)
    if ($null -eq $Value) { return "" }
    return ([string]$Value).Replace("`r", " ").Replace("`n", " ").Trim()
}

function Format-Number {
    param($Value, [string]$Default = "0")
    if ($null -eq $Value) { return $Default }
    try {
        $number = [double]$Value
        if ([Math]::Abs($number) -gt 0 -and [Math]::Abs($number) -lt 1) {
            return ("{0:N3}" -f $number)
        }
        if ([Math]::Abs($number - [Math]::Round($number)) -lt 0.05) {
            return ("{0:N0}" -f $number)
        }
        return ("{0:N1}" -f $number)
    } catch {
        return [string]$Value
    }
}

function Truncate-Text {
    param([string]$Text, [int]$MaxLength)
    if ($Text.Length -le $MaxLength) { return $Text }
    return $Text.Substring(0, [Math]::Max(0, $MaxLength - 15)).TrimEnd() + "`n... $($UiText.Truncated)"
}

function Json-String {
    param([string]$Escaped)
    return ($Escaped | ConvertFrom-Json)
}

$UiText = [ordered]@{
    Truncated = Json-String '"\ub0b4\uc6a9 \ucd95\uc57d"'
    Title = Json-String '"Sura \uc804\uccb4 \uace8 \uc9c4\ud589\ub3c4"'
    Status = Json-String '"\uc0c1\ud0dc"'
    Progress = Json-String '"\uc804\uccb4 \uc9c4\ud589\ub3c4"'
    Passed = Json-String '"\ud1b5\uacfc"'
    Remaining = Json-String '"\ub0a8\uc740 \uc791\uc5c5"'
    CountSuffix = Json-String '"\uac1c"'
    Blockers = Json-String '"\ube14\ub85c\ucee4"'
    NativeTarget = Json-String '"Native \ubaa9\ud45c"'
    Target = Json-String '"\ubaa9\ud45c"'
    RemainingTitle = Json-String '"\uc804\uccb4 \uace8\uae4c\uc9c0 \ub0a8\uc740 \uc791\uc5c5"'
    None = Json-String '"\uc5c6\uc74c"'
    Next = Json-String '"\ub2e4\uc74c"'
    MorePrefix = Json-String '"\uc678"'
    MoreSuffix = Json-String '"\uac1c \ub354 \uc788\uc74c"'
    BlockersTitle = Json-String '"\ud604\uc7ac \ud575\uc2ec \ube14\ub85c\ucee4"'
}

if (-not $DryRun -and [string]::IsNullOrWhiteSpace($Out) -and [string]::IsNullOrWhiteSpace($WebhookUrl)) {
    Write-Host "discord_goal_status: SKIP (SURA_DISCORD_WEBHOOK empty)"
    exit 0
}

$goalPath = Resolve-InputPath $GoalAuditJson
if (-not (Test-Path -LiteralPath $goalPath)) {
    if ($AllowMissing) {
        Write-Host "discord_goal_status: SKIP (goal audit missing: $GoalAuditJson)"
        exit 0
    }
    throw "goal audit JSON not found: $GoalAuditJson"
}

$audit = Read-JsonFile $goalPath
$remaining = @(As-Array (Get-PropertyValue $audit "remaining_work"))
$blockers = @(As-Array (Get-PropertyValue $audit "blockers"))

$status = One-Line (Get-PropertyValue $audit "status")
if ([string]::IsNullOrWhiteSpace($status)) {
    $status = $(if ([bool](Get-PropertyValue $audit "passed")) { "PASS" } else { "INCOMPLETE" })
}
$progress = Format-Number (Get-PropertyValue $audit "progress_percent")
$passedCount = Format-Number (Get-PropertyValue $audit "passed_count")
$requiredCount = Format-Number (Get-PropertyValue $audit "required_count")
$failedCount = Format-Number (Get-PropertyValue $audit "failed_count")
$blockerCount = Format-Number (Get-PropertyValue $audit "blocker_count")
$maxNativeRatio = Get-PropertyValue $audit "max_native_ratio"

$nativePath = $NativePerfJson
if ([string]::IsNullOrWhiteSpace($nativePath)) {
    $nativePath = Join-Path (Split-Path -Parent $goalPath) "native_perf.json"
}
$nativeLine = ""
$resolvedNative = Resolve-InputPath $nativePath
if (Test-Path -LiteralPath $resolvedNative) {
    $native = Read-JsonFile $resolvedNative
    $ratio = Get-PropertyValue $native "sura_native_ratio"
    $suraMs = Get-PropertyValue $native "sura_jit_ms"
    $nativeMs = Get-PropertyValue $native "native_ms"
    if ($null -ne $ratio) {
        $limit = if ($null -ne $maxNativeRatio) { Format-Number $maxNativeRatio } else { "10" }
        $nativeLine = "$($UiText.NativeTarget): Sura/native $(Format-Number $ratio)x / $($UiText.Target) <= ${limit}x"
        if ($null -ne $suraMs -and $null -ne $nativeMs) {
            $nativeLine += " (Sura $(Format-Number $suraMs)ms, C++ $(Format-Number $nativeMs)ms)"
        }
        $baselines = @(As-Array (Get-PropertyValue $native "baselines"))
        $vec3 = $baselines | Where-Object {
            [string](Get-PropertyValue $_ "id") -eq "vec3" -or
            [string](Get-PropertyValue $_ "dimension") -eq "vec3" -or
            [string](Get-PropertyValue $_ "benchmark") -match "Vec3|3D"
        } | Select-Object -First 1
        if ($vec3) {
            $vec3Ratio = Get-PropertyValue $vec3 "sura_native_ratio"
            $vec3SuraMs = Get-PropertyValue $vec3 "sura_jit_ms"
            $vec3NativeMs = Get-PropertyValue $vec3 "native_ms"
            if ($null -ne $vec3Ratio) {
                $nativeLine += " | 3D Sura/native $(Format-Number $vec3Ratio)x"
                if ($null -ne $vec3SuraMs -and $null -ne $vec3NativeMs) {
                    $nativeLine += " (Sura $(Format-Number $vec3SuraMs)ms, C++ $(Format-Number $vec3NativeMs)ms)"
                }
            }
        }
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("**$($UiText.Title)**")
$lines.Add("$($UiText.Status): $status")
$lines.Add("$($UiText.Progress): $progress% ($passedCount/$requiredCount $($UiText.Passed))")
$lines.Add("$($UiText.Remaining): $failedCount$($UiText.CountSuffix) | $($UiText.Blockers): $blockerCount$($UiText.CountSuffix)")
if (-not [string]::IsNullOrWhiteSpace($nativeLine)) {
    $lines.Add($nativeLine)
}

if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_REPOSITORY) -and -not [string]::IsNullOrWhiteSpace($env:GITHUB_RUN_ID)) {
    $server = if ([string]::IsNullOrWhiteSpace($env:GITHUB_SERVER_URL)) { "https://github.com" } else { $env:GITHUB_SERVER_URL }
    $lines.Add("CI: $server/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID")
}
if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_SHA)) {
    $lines.Add("Commit: $($env:GITHUB_SHA.Substring(0, [Math]::Min(7, $env:GITHUB_SHA.Length)))")
}

$lines.Add("")
$lines.Add("**$($UiText.RemainingTitle)**")
if ($remaining.Count -eq 0) {
    $lines.Add("- $($UiText.None)")
} else {
    $take = [Math]::Max(1, $MaxRemainingItems)
    foreach ($item in @($remaining | Select-Object -First $take)) {
        $category = One-Line (Get-PropertyValue $item "category")
        $id = One-Line (Get-PropertyValue $item "id")
        $requirement = One-Line (Get-PropertyValue $item "requirement")
        $message = One-Line (Get-PropertyValue $item "message")
        $nextAction = One-Line (Get-PropertyValue $item "next_action")

        $label = if ([string]::IsNullOrWhiteSpace($id)) { $category } else { "$category/$id" }
        $line = "- [$label] $requirement"
        if (-not [string]::IsNullOrWhiteSpace($message)) {
            $line += " - $message"
        }
        $lines.Add($line)
        if (-not [string]::IsNullOrWhiteSpace($nextAction)) {
            $lines.Add("  $($UiText.Next): $nextAction")
        }
    }
    if ($remaining.Count -gt $take) {
        $lines.Add("- $($UiText.MorePrefix) $($remaining.Count - $take)$($UiText.MoreSuffix)")
    }
}

if ($blockers.Count -gt 0) {
    $lines.Add("")
    $lines.Add("**$($UiText.BlockersTitle)**")
    foreach ($item in @($blockers | Select-Object -First 3)) {
        $id = One-Line (Get-PropertyValue $item "id")
        $message = One-Line (Get-PropertyValue $item "message")
        $lines.Add("- ${id}: $message")
    }
}

$content = Truncate-Text (($lines | ForEach-Object { [string]$_ }) -join "`n") 1900
$payload = [ordered]@{
    username = $Username
    content = $content
    allowed_mentions = [ordered]@{
        parse = @()
    }
}

$json = $payload | ConvertTo-Json -Depth 8
if (-not [string]::IsNullOrWhiteSpace($Out)) {
    $outPath = $Out
    if (-not [System.IO.Path]::IsPathRooted($outPath)) {
        $outPath = Join-Path (Get-Location).Path $outPath
    }
    $parent = Split-Path -Parent $outPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($outPath, $json, (New-Object System.Text.UTF8Encoding($false)))
}

if ($DryRun) {
    Write-Host "discord_goal_status: DRY-RUN"
    exit 0
}

if ([string]::IsNullOrWhiteSpace($WebhookUrl)) {
    Write-Host "discord_goal_status: SKIP (SURA_DISCORD_WEBHOOK empty)"
    exit 0
}

$bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
Invoke-RestMethod -Uri $WebhookUrl -Method Post -ContentType "application/json; charset=utf-8" -Body $bytes | Out-Null
Write-Host "discord_goal_status: SENT"
