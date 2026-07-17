param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_engine_format_" + [System.Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $file = Join-Path $root "format_test.sura"
    Set-Content -LiteralPath $file -Encoding UTF8 @"
if true then
print "format me"
end
"@

    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $checkOutput = & $enginePath --format-check $file 2>&1
    $checkCode = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    $checkText = $checkOutput -join "`n"
    if ($checkCode -eq 0 -or $checkText -notmatch "is not formatted") {
        $checkOutput | Write-Host
        throw "expected --format-check to fail before formatting"
    }

    $formatOutput = & $enginePath --format $file 2>&1
    $formatCode = $LASTEXITCODE
    if ($formatCode -ne 0 -or ($formatOutput -join "`n") -notmatch "Sura format: 1 formatted, 0 unchanged, 0 failed") {
        $formatOutput | Write-Host
        throw "expected --format to rewrite the file"
    }

    $formatted = Get-Content -Raw -Encoding UTF8 -LiteralPath $file
    if ($formatted -notmatch "  print `"format me`"") {
        throw "expected formatted indentation"
    }

    $finalCheck = & $enginePath --format-check $file 2>&1
    $finalCode = $LASTEXITCODE
    if ($finalCode -ne 0 -or ($finalCheck -join "`n") -notmatch "Sura format check: 1 passed, 0 failed") {
        $finalCheck | Write-Host
        throw "expected --format-check to pass after formatting"
    }

    Write-Host "[PASS] engine format smoke"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
