param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_cli_args_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $script = Join-Path $temp "cli_args.sura"
    Write-Text $script @"
args is argv()
assert_eq(path_basename(script_name()), "cli_args.sura")
assert_eq(argc(), 4)
assert_eq(length(args), 4)
assert_eq(args[0], "--mode=fast")
assert_eq(args[1], "input.txt")
assert_eq(args[2], "--jit")
assert_eq(args[3], "한글")
parsed is cli_parse(args.join(" "))
assert_eq(parsed.mode, "fast")
assert_eq(parsed.args[0], "input.txt")
assert(parsed.jit)
assert_eq(parsed.args[1], "한글")
print "cli_args_smoke: PASS"
"@

    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Engine $script --mode=fast input.txt -- --jit "한글" 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    $text = $out -join "`n"
    if ($code -ne 0 -or $text -notmatch "cli_args_smoke: PASS") {
        Write-Output $text
        throw "expected CLI argv smoke to pass"
    }

    "cli_args_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
