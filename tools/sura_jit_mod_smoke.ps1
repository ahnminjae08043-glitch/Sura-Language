param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe")
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

if (-not (Test-Path -LiteralPath $Engine)) {
    throw "Sura engine not found: $Engine"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_jit_mod_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $src = Join-Path $temp "jit_mod.sura"
    @'
func route_score(i) do
  latency is (i * 37) % 120
  cost is (i * 17 + 11) % 90
  risk is (i * i + 19) % 70
  score is 1000 - latency * 3 - cost * 2 - risk * 5
  if risk > 50 then
    score is score - 160
  end
  return score
end

func interp_line(i) do
  second is i % 60
  level is "ERROR"
  status is 500
  return "sec={second} level={level} status={status}"
end

i is 0
checksum is 0
while i < 200 do
  checksum is checksum + route_score(i)
  i is i + 1
end
print checksum

j is 0
line is ""
while j < 200 do
  line is interp_line(j)
  j is j + 1
end
print line
assert_eq(line, "sec=19 level=ERROR status=500")
'@ | Set-Content -LiteralPath $src -Encoding UTF8

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Engine --jit $src 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    $text = $out -join "`n"
    if ($code -ne 0) {
        Write-Output $text
        throw "JIT MOD smoke failed with exit code $code"
    }
    if ($text -notmatch "2 function\(s\).*compiled") {
        Write-Output $text
        throw "expected route_score and interp_line functions to be native-compiled"
    }
    if ($text -notmatch "100820") {
        Write-Output $text
        throw "unexpected JIT MOD checksum"
    }
    if ($text -notmatch "sec=19 level=ERROR status=500") {
        Write-Output $text
        throw "unexpected JIT interpolated string result"
    }

    "jit_mod_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
