param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
[Console]::InputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$EnginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_legacy_console_" + [System.Guid]::NewGuid().ToString("N"))

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $Text = $Text -replace "`r`n", "`n"
    if (-not $Text.EndsWith("`n")) { $Text += "`n" }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $runScript = Join-Path $temp "legacy_console_run.sura"
    Write-Text $runScript @'
silent on
cls
grid_init 5 2
grid_set 0 0 *
grid_set 1 0 O green
grid_set 2 0 o yellow
grid_draw
console_raw("\n")
print "legacy_console_commands_smoke: PASS"
'@

    $output = & $EnginePath $runScript 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $output | Out-String | Write-Host
        throw "legacy console run failed with exit code $code"
    }
    $text = ($output | Out-String)
    if ($text -notmatch "legacy_console_commands_smoke: PASS") {
        $text | Write-Host
        throw "legacy console run did not print PASS"
    }

    $checkScript = Join-Path $temp "legacy_console_check.sura"
    Write-Text $checkScript @'
readkey key
if key == q then print "quit" else if key == w then print "up" else print "other"
'@

    $checkOutput = & $EnginePath --check $checkScript 2>&1
    $checkCode = $LASTEXITCODE
    if ($checkCode -ne 0) {
        $checkOutput | Out-String | Write-Host
        throw "legacy console check failed with exit code $checkCode"
    }

    Write-Host "legacy_console_commands_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
