param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
[Console]::InputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$root = Split-Path -Parent $PSScriptRoot
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$gamePath = Join-Path $root "examples\starblaster_3d.sura"

if (-not (Test-Path -LiteralPath $gamePath)) {
    throw "StarBlaster 3D example not found: $gamePath"
}

$output = & $enginePath $gamePath -- --smoke 2>&1
$code = $LASTEXITCODE
if ($code -ne 0) {
    $output | Out-String | Write-Host
    throw "starblaster_3d_smoke failed with exit code $code"
}

$text = $output | Out-String
if ($text -match "ANOMALY: use of REX\.w") {
    $text | Write-Host
    throw "starblaster_3d smoke leaked native window anomaly output"
}

if ($text -notmatch "starblaster_3d_smoke: PASS") {
    $text | Write-Host
    throw "starblaster_3d_smoke did not print PASS"
}

Write-Host "starblaster_3d_smoke: PASS"
