param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_jit_vm_safety_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $source = Join-Path $temp "frame_overflow.sura"
    [System.IO.File]::WriteAllText($source, @'
func descend(n) do
    # MAKE_ARRAY keeps this function on the iterative VM path even with --jit.
    marker is [n]
    return descend(n + 1)
end

descend(0)
'@, $utf8NoBom)

    foreach ($mode in @("vm", "jit")) {
        [string[]]$engineArgs = if ($mode -eq "jit") { @("--jit", $source) } else { @(,$source) }
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $out = & $enginePath @engineArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $oldPreference
        $text = $out -join "`n"

        if ($code -eq 0 -or $text -notmatch "\[E500\]") {
            Write-Output $text
            throw "expected graceful [E500] frame overflow in $mode mode (exit=$code)"
        }
    }

    # A strict-loop candidate with observable constructor work must side-exit.
    # The optimized physics loop is allowed only for the proven plain record
    # constructor; otherwise every source-level constructor call must run.
    $strictCtor = Join-Path $temp "strict_constructor_semantics.sura"
    [System.IO.File]::WriteAllText($strictCtor, @'
ctor_calls is 0

class Vec2 do
    x is 0
    y is 0

    func init(x, y) do
        global ctor_calls
        ctor_calls += 1
        self.x is x
        self.y is y
    end

    func add(other) do
        return Vec2(self.x + other.x, self.y + other.y)
    end

    func scale(k) do
        return Vec2(self.x * k, self.y * k)
    end
end

func step(pos, vel, dt) do
    return pos.add(vel.scale(dt))
end

p is Vec2(0, 0)
v is Vec2(2, 4)
dt is 0.5
i is 0
while i < 3 do
    p is step(p, v, dt)
    i is i + 1
end

assert_eq(p.x, 3)
assert_eq(p.y, 6)
assert_eq(ctor_calls, 8)
print "strict constructor semantics: PASS"
'@, $utf8NoBom)

    foreach ($disableOsr in @($false, $true)) {
        if ($disableOsr) { $env:SURA_JIT_DISABLE_OSR = "1" }
        else { Remove-Item Env:\SURA_JIT_DISABLE_OSR -ErrorAction SilentlyContinue }
        try {
            $oldPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $out = & $enginePath --jit $strictCtor 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
            $ErrorActionPreference = $oldPreference
            $text = $out -join "`n"
            if ($code -ne 0 -or $text -notmatch "strict constructor semantics: PASS") {
                Write-Output $text
                throw "strict-loop constructor semantics failed (disable_osr=$disableOsr, exit=$code)"
            }
        } finally {
            Remove-Item Env:\SURA_JIT_DISABLE_OSR -ErrorAction SilentlyContinue
        }
    }

    "jit_vm_safety_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
