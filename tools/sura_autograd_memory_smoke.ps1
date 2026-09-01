param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_autograd_memory_" + [System.Guid]::NewGuid().ToString("N"))
$script = Join-Path $temp "memory.sura"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$source = @'
use autograd

func allocate_once() do
  value is autograd.zeros([100000])
end

# Each 800 KB Tensor becomes unreachable on return. Allocation-pressure GC
# must reclaim it instead of reporting a false live-memory overflow.
repeat 80 do
  allocate_once()
end

live is autograd.zeros([80000])

# clone() must honor the same configurable 1 MiB limit as autograd tensors.
clone_rejected is false
try
  clone(live)
catch error
  clone_rejected is true
end
assert(clone_rejected)

# The limit is checked before constructing this 1.6 MB vector.
allocation_rejected is false
try
  autograd.zeros([200000])
catch error
  allocation_rejected is true
end
assert(allocation_rejected)

assert_eq(autograd.limits().memory_limit_bytes, 1048576)
print "autograd_memory_smoke: PASS"
'@

$oldLimit = $env:SURA_TENSOR_MEMORY_LIMIT_MB
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    [System.IO.File]::WriteAllText($script, ($source.Trim() + "`n"), $utf8NoBom)
    $env:SURA_TENSOR_MEMORY_LIMIT_MB = "1"
    foreach ($args in @(@($script), @("--jit", $script))) {
        $output = & $enginePath @args 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch "autograd_memory_smoke: PASS") {
            Write-Output ($output -join "`n")
            throw "autograd memory smoke failed for: $($args -join ' ')"
        }
    }
    "sura_autograd_memory_smoke: PASS"
}
finally {
    if ($null -eq $oldLimit) {
        Remove-Item Env:SURA_TENSOR_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    } else {
        $env:SURA_TENSOR_MEMORY_LIMIT_MB = $oldLimit
    }
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
