param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_random_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $Text = $Text -replace "`r`n", "`n"
    if (-not $Text.EndsWith("`n")) { $Text += "`n" }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Engine {
    param([string]$Script)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location $temp
    try {
        $out = & $EnginePath "--jit" $Script 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        $ErrorActionPreference = $old
    }
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $script = Join-Path $temp "random_smoke.sura"
    Write-Text $script @'
use random

random.seed(12345)
first_int is random.int(1, 1000)
first_float is random.float()
first_bool is random.bool(0.75)
first_choice is random.choice(["sura", "game", "agent"])
first_shuffle is random.shuffle([1, 2, 3, 4, 5])
first_bytes is random.bytes(8)
first_uuid is random.uuid()

random.seed(12345)
assert_eq(random.int(1, 1000), first_int)
assert_eq(random.float(), first_float)
assert_eq(random.bool(0.75), first_bool)
assert_eq(random.choice(["sura", "game", "agent"]), first_choice)
repeat_shuffle is random.shuffle([1, 2, 3, 4, 5])
assert_eq(repeat_shuffle.len(), first_shuffle.len())

i is 0
while i < first_shuffle.len() do
    assert_eq(repeat_shuffle[i], first_shuffle[i])
    i += 1
end

assert_eq(first_bytes.len(), 8)
i is 0
while i < first_bytes.len() do
    assert_between(first_bytes[i], 0, 255)
    i += 1
end

assert(first_uuid.contains("-"))
bounded is random.int(5)
assert(bounded >= 0 and bounded < 5)
span is random.float(10, 20)
assert(span >= 10 and span <= 20)
assert_eq(random.bool(1.0), true)
assert_eq(random.bool(0.0), false)

random_seed(77)
legacy_roll is random_int(1, 6)
random_seed(77)
assert_eq(random_int(1, 6), legacy_roll)
assert_eq(random_bytes(3).len(), 3)
assert(random(10) >= 0)

print "random_smoke: PASS"
'@

    $result = Run-Engine -Script $script
    if ($result.Code -ne 0 -or $result.Output -notmatch "random_smoke: PASS") {
        Write-Output $result.Output
        throw "expected random smoke to pass"
    }

    "random_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
