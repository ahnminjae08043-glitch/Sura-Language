param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Split-Path -Parent $PSScriptRoot
$worker = Join-Path $root "tests\jit_main_exception_no_replay_worker.sura"
$marker = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("sura-jit-main-no-replay-" + [Guid]::NewGuid().ToString("N") + ".txt")

try {
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = (& $enginePath --jit $worker -- $marker 2>&1) | Out-String
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorAction
    if ($exitCode -eq 0) {
        throw "expected the JIT worker to fail"
    }
    if (-not (Test-Path -LiteralPath $marker)) {
        throw "JIT worker did not write its pre-error marker"
    }
    $content = [System.IO.File]::ReadAllText($marker, [System.Text.Encoding]::UTF8)
    if ($content -ne "x") {
        throw "native main replayed after a runtime exception; marker was '$content'"
    }
    "jit_main_exception_no_replay: PASS"
}
finally {
    Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
}

exit 0
