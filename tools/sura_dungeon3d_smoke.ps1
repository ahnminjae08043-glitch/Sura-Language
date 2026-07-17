param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
[Console]::InputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$EnginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Split-Path -Parent $PSScriptRoot
$script = Join-Path $root "dungeon3d.sura"

$output = & $EnginePath $script -- --smoke 2>&1
$code = $LASTEXITCODE
if ($code -ne 0) {
    $output | Out-String | Write-Host
    throw "dungeon3d smoke failed with exit code $code"
}

$text = ($output | Out-String)
if ($text -match "ANOMALY: use of REX\.w") {
    $text | Write-Host
    throw "dungeon3d smoke leaked native window anomaly output"
}

if ($text -notmatch "dungeon3d_smoke: PASS") {
    $text | Write-Host
    throw "dungeon3d smoke did not print PASS"
}

Write-Host "dungeon3d_smoke: PASS"
