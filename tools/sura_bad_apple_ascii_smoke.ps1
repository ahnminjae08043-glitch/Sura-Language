param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$enginePath = (Resolve-Path -LiteralPath (Join-Path $root $Engine)).Path
$script = Join-Path $root "examples\bad_apple_ascii.sura"

foreach ($arguments in @(@($script, "--smoke"), @("--jit", $script, "--smoke"))) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $enginePath @arguments 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    $text = $output -join "`n"
    if ($code -ne 0 -or $text -notmatch "bad_apple_ascii: PASS") {
        throw "Bad Apple ASCII smoke failed: $text"
    }
}

"bad_apple_ascii_smoke: PASS"
