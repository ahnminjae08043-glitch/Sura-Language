param(
    [Parameter(Mandatory=$true)][string]$Source,
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [switch]$Release,
    [string]$ReleaseKey = "",
    [string]$ReleaseId = "",
    [string]$ReleaseExpires = ""
)

$root = Split-Path -Parent $PSScriptRoot
$src = (Resolve-Path -LiteralPath $Source).Path
$artifact = if ($Release) {
    [System.IO.Path]::ChangeExtension($src, ".sura.srp")
} else {
    [System.IO.Path]::ChangeExtension($src, ".sura.bc")
}

if ($Release) {
    $releaseArgs = @("--release", $src, "--out", $artifact)
    if ($ReleaseKey) {
        $releaseArgs += @("--release-key", $ReleaseKey)
    }
    if ($ReleaseId) {
        $releaseArgs += @("--release-id", $ReleaseId)
    }
    if ($ReleaseExpires) {
        $releaseArgs += @("--release-expires", $ReleaseExpires)
    }
    & $Engine @releaseArgs
} else {
    & $Engine --compile $src --out $artifact
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$launcher = [System.IO.Path]::ChangeExtension($src, ".aot.bat")
$loadFlag = if ($Release) { "--load-release" } else { "--load" }
@"
@echo off
"$Engine" $loadFlag "$artifact" %*
"@ | Set-Content -LiteralPath $launcher -Encoding ASCII

Write-Host "[OK] AOT artifact: $artifact"
Write-Host "[OK] Launcher: $launcher"
if ($Release -and $ReleaseKey) {
    Write-Host "[OK] Release key required at runtime via SURA_RELEASE_KEY or --load-release-key."
}
if ($Release -and $ReleaseExpires) {
    Write-Host "[OK] Release expires: $ReleaseExpires"
}
