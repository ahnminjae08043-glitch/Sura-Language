param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
[Console]::InputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$EnginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_inline_if_else_" + [System.Guid]::NewGuid().ToString("N"))

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
    $script = Join-Path $temp "inline_if_else_smoke.sura"
    Write-Text $script @'
score is 0
if true then score is 1 else score is 2
assert_eq(score, 1)

if false then score is 3 else score is 4
assert_eq(score, 4)

if true then score is score + 1
assert_eq(score, 5)

if false then print "inline-if-bad" else print "inline-if-command-ok"

branch is ""
if false then branch is "first" else if true then branch is "second" else branch is "third"
assert_eq(branch, "second")

block_branch is ""
if false then
    block_branch is "first"
else if true then
    block_branch is "second"
else
    block_branch is "third"
end
assert_eq(block_branch, "second")

chain_branch is ""
if false then
    chain_branch is "first"
else if false then
    chain_branch is "second"
else if true then
    chain_branch is "third"
else
    chain_branch is "fourth"
end
assert_eq(chain_branch, "third")

func hp_bar(cur, mx, bw) do
    filled is floor(cur * bw / mx)
    if filled < 0 then filled is 0
    if filled > bw then filled is bw
    bar is ""
    i is 0
    while i < bw do
        if i < filled then bar is bar + "=" else bar is bar + "."
        i is i + 1
    end
    return bar
end

assert_eq(hp_bar(3, 5, 5), "===..")
print "inline_if_else_smoke: PASS"
'@

    $output = & $EnginePath $script 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $output | Out-String | Write-Host
        throw "inline_if_else_smoke failed with exit code $code"
    }
    $text = ($output | Out-String)
    if ($text -notmatch "inline_if_else_smoke: PASS" -or $text -notmatch "inline-if-command-ok") {
        $text | Write-Host
        throw "inline_if_else_smoke did not print expected output"
    }
    Write-Host "inline_if_else_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
