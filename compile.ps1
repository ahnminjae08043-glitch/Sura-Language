param(
    [ValidateSet("portable", "native")]
    [string]$Mode = "portable"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$buildScript = Join-Path $root "build.bat"
if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
    throw "build.bat not found: $buildScript"
}

Push-Location $root
try {
    & $env:ComSpec /d /c $buildScript $Mode
    $exitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}

if ($exitCode -ne 0) {
    Write-Host "[FAIL] Sura build failed (exit $exitCode)."
    exit $exitCode
}

Write-Host "[OK] Built SuraLanguage.exe in $Mode mode."
