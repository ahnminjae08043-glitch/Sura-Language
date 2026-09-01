param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$testPath = Join-Path $root "tests\async_structured_stress.sura"

foreach ($mode in @("interpreter", "jit")) {
    $engineArgs = @()
    if ($mode -eq "jit") { $engineArgs += "--jit" }
    $engineArgs += $testPath

    # Windows PowerShell turns native stderr into an ErrorRecord when the
    # surrounding script uses Stop.  Capture it as ordinary test output.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $enginePath @engineArgs 2>&1 | ForEach-Object { "$_" }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }

    $text = $output -join "`n"
    if ($exitCode -ne 0 -or $text -notmatch "structured async stress: PASS") {
        Write-Output $text
        throw "structured async $mode smoke failed (exit $exitCode)"
    }
}

"async_structured_smoke: PASS"
