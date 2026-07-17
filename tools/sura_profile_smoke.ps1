param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_profile_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $script = Join-Path $temp "profile_smoke.sura"
    $json = Join-Path $temp "profile.json"

    Write-Text $script @"
func add(a, b) do
  return a + b
end

total is 0
i is 0
while i < 80 do
  total is add(total, i)
  if total > -1 then
    total is total + 1
  end
  i is i + 1
end

print total
"@

    $out = & $EnginePath --profile-json $json $script 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $text = $out -join "`n"
    if ($code -ne 0 -or $text -notmatch "Sura Runtime Profile Report" -or $text -notmatch "\[profile\] wrote") {
        Write-Output $text
        throw "expected profile run to pass and print report"
    }
    if (-not (Test-Path $json)) {
        throw "expected profile JSON to be written"
    }

    $profile = Get-Content -Raw -Path $json | ConvertFrom-Json
    if ($profile.summary.arithmetic_sites -lt 1) {
        throw "expected arithmetic profile sites"
    }
    if ($profile.summary.branch_sites -lt 1) {
        throw "expected branch profile sites"
    }
    if ($profile.summary.call_sites -lt 1) {
        throw "expected call profile sites"
    }

    "profile_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
