param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$temp = Join-Path $tempRoot ("sura_checkpoint_dtype_range_" + [System.Guid]::NewGuid().ToString("N"))
$scriptPath = Join-Path $temp "checkpoint_dtype_range.sura"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-CheckpointV2 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte]$DType,
        [Parameter(Mandatory = $true)][double]$Value
    )

    # Checkpoint v2 stores tensor payload values as little-endian f64 even
    # when the record dtype is float16 or bfloat16. The SHA-256 footer covers
    # every preceding byte and is not included in its own digest.
    $stream = New-Object System.IO.MemoryStream
    $writer = [System.IO.BinaryWriter]::new($stream, $utf8NoBom, $true)
    try {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("SURACKPT"))
        $writer.Write([uint16]2) # version
        $writer.Write([uint16]0) # global flags
        $writer.Write([uint32]1) # tensor count

        $nameBytes = [System.Text.Encoding]::UTF8.GetBytes("probe")
        $writer.Write([uint32]$nameBytes.Length)
        $writer.Write($nameBytes)
        $writer.Write([byte]1)      # rank
        $writer.Write([byte]0)      # requires_grad
        $writer.Write([byte]0)      # optimizer state flags
        $writer.Write($DType)       # TensorDType record flag
        $writer.Write([uint64]1)    # numel
        $writer.Write([uint64]1)    # shape[0]
        $writer.Write($Value)       # v2 data payload is f64
        $writer.Flush()

        $payload = $stream.ToArray()
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $digest = $sha256.ComputeHash($payload)
        }
        finally {
            $sha256.Dispose()
        }

        $file = New-Object byte[] ($payload.Length + $digest.Length)
        [System.Array]::Copy($payload, 0, $file, 0, $payload.Length)
        [System.Array]::Copy($digest, 0, $file, $payload.Length, $digest.Length)
        [System.IO.File]::WriteAllBytes($Path, $file)
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Assert-ValidCheckpointFooter {
    param([Parameter(Mandatory = $true)][string]$Path)

    $file = [System.IO.File]::ReadAllBytes($Path)
    if ($file.Length -lt 33) {
        throw "generated checkpoint is too short: $Path"
    }
    $payloadLength = $file.Length - 32
    $payload = New-Object byte[] $payloadLength
    $footer = New-Object byte[] 32
    [System.Array]::Copy($file, 0, $payload, 0, $payloadLength)
    [System.Array]::Copy($file, $payloadLength, $footer, 0, 32)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $actual = $sha256.ComputeHash($payload)
    }
    finally {
        $sha256.Dispose()
    }
    for ($i = 0; $i -lt 32; ++$i) {
        if ($actual[$i] -ne $footer[$i]) {
            throw "generated checkpoint has an invalid SHA-256 footer: $Path"
        }
    }
}

function ConvertTo-SuraStringLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value.Replace("\", "/").Replace('"', '\"')
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $normalPath = Join-Path $temp "normal-f16.surackpt"
    $overflowF16Path = Join-Path $temp "overflow-f16.surackpt"
    $overflowBf16Path = Join-Path $temp "overflow-bf16.surackpt"

    # TensorDType: FLOAT64=0, FLOAT32=1, FLOAT16=2, BFLOAT16=3.
    Write-CheckpointV2 -Path $normalPath -DType 2 -Value 1.5
    Write-CheckpointV2 -Path $overflowF16Path -DType 2 -Value 70000.0
    # 3.4e38 is finite as f64 and below float32 max, but above the actual
    # BF16 maximum finite value (approximately 3.38953139e38).
    Write-CheckpointV2 -Path $overflowBf16Path -DType 3 -Value ([double]3.4e38)

    Assert-ValidCheckpointFooter -Path $normalPath
    Assert-ValidCheckpointFooter -Path $overflowF16Path
    Assert-ValidCheckpointFooter -Path $overflowBf16Path

    $normalSura = ConvertTo-SuraStringLiteral $normalPath
    $overflowF16Sura = ConvertTo-SuraStringLiteral $overflowF16Path
    $overflowBf16Sura = ConvertTo-SuraStringLiteral $overflowBf16Path
    $source = @"
use autograd

func expect_dtype_rejection(path, dtype_name) do
  before_bytes is autograd.limits().memory_used_bytes
  rejected is false
  message is ""
  try
    ignored is autograd.load_checkpoint(path)
  catch error
    rejected is true
    message is error
  end
  assert(rejected)
  assert(message.contains("value overflows " + dtype_name))
  assert(not message.contains("SHA-256"))
  # A rejected materialization must release any staged tensor reservation;
  # no invalid TensorBuffer containing infinity may escape the loader.
  assert_eq(autograd.limits().memory_used_bytes, before_bytes)
end

normal is autograd.load_checkpoint("$normalSura")
assert_eq(autograd.dtype(normal.probe), "float16")
assert_eq(autograd.data(normal.probe)[0], 1.5)

expect_dtype_rejection("$overflowF16Sura", "float16")
expect_dtype_rejection("$overflowBf16Sura", "bfloat16")

print "checkpoint_dtype_range_smoke: PASS"
"@
    [System.IO.File]::WriteAllText($scriptPath, ($source.Trim() + "`n"), $utf8NoBom)

    foreach ($engineArgs in @(@($scriptPath), @("--jit", $scriptPath))) {
        $output = & $enginePath @engineArgs 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch "checkpoint_dtype_range_smoke: PASS") {
            Write-Output ($output -join "`n")
            throw "checkpoint dtype range smoke failed for: $($engineArgs -join ' ')"
        }
    }

    "sura_checkpoint_dtype_range_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
        $leaf = Split-Path -Leaf $resolvedTemp
        if ($resolvedTemp.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) `
            -and $leaf.StartsWith("sura_checkpoint_dtype_range_", [System.StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
        }
    }
}
