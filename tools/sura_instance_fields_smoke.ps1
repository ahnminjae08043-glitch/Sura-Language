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

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_instance_fields_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $src = Join-Path $temp "instance_fields.sura"
    @'
struct Small do
  x is 0
  y is 0
  func add(other) do
    return Small(self.x + other.x, self.y + other.y)
  end
end

struct Wide do
  a is 0
  b is 0
  c is 0
  d is 0
  e is 0
  f is 0
  func sum() do
    return self.a + self.b + self.c + self.d + self.e + self.f
  end
end

s is Small(2, 3)
t is Small(4, 5)
u is s.add(t)
assert_eq(u.x, 6)
assert_eq(u.y, 8)

j is Small(0, 0)
i is 0
while i < 20 do
  j is Small(i, i + 1)
  i is i + 1
end
jc is clone(j)
assert_eq(j.x, 19)
assert_eq(j.y, 20)
assert_eq(jc.x, 19)
print j
print jc

w is Wide(1, 2, 3, 4, 5, 6)
assert_eq(w.f, 6)
assert_eq(w.sum(), 21)
wc is clone(w)
assert_eq(wc.e, 5)
assert_eq(wc.sum(), 21)
wc.f is 10
assert_eq(w.sum(), 21)
assert_eq(wc.sum(), 25)
print "instance fields smoke: PASS"
'@ | Set-Content -LiteralPath $src -Encoding UTF8

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Engine --jit $src 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    $text = $out -join "`n"
    if ($code -ne 0) {
        Write-Output $text
        throw "instance fields smoke failed with exit code $code"
    }
    if ($text -notmatch "instance fields smoke:\s+PASS" -or
        $text -notmatch "<Instance Small>") {
        Write-Output $text
        throw "expected instance fields smoke PASS output with stable instance type names"
    }

    $gcSrc = Join-Path $temp "instance_gc_reuse.sura"
    @'
struct Pair do
  x is 0
  y is 0
end

i is 0
last is Pair(0, 0)
while i < 35000 do
  p is Pair(i, i + 1)
  last is p
  i is i + 1
end

assert_eq(last.x, 34999)
assert_eq(last.y, 35000)
print "instance gc reuse smoke: PASS"
'@ | Set-Content -LiteralPath $gcSrc -Encoding UTF8

    $ErrorActionPreference = "Continue"
    $gcOut = & $Engine $gcSrc 2>&1 | ForEach-Object { "$_" }
    $gcCode = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    $gcText = $gcOut -join "`n"
    if ($gcCode -ne 0) {
        Write-Output $gcText
        throw "instance GC reuse smoke failed with exit code $gcCode"
    }
    if ($gcText -notmatch "instance gc reuse smoke:\s+PASS") {
        Write-Output $gcText
        throw "expected instance GC reuse smoke PASS output"
    }

    "instance_fields_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
