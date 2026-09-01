param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/ap_startup_features.sura")
)

$ErrorActionPreference = "Stop"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_ap_startup_" + [guid]::NewGuid().ToString("N"))

function Test-ByteSequence {
    param([byte[]]$Bytes, [byte[]]$Needle)
    if ($Needle.Length -eq 0 -or $Bytes.Length -lt $Needle.Length) { return $false }
    for ($i = 0; $i -le $Bytes.Length - $Needle.Length; ++$i) {
        $matched = $true
        for ($j = 0; $j -lt $Needle.Length; ++$j) {
            if ($Bytes[$i + $j] -ne $Needle[$j]) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $true }
    }
    return $false
}

function Convert-HexBytes {
    param([string]$Text)
    [byte[]]$result = @(
        $Text.Trim() -split "\s+" |
            ForEach-Object { [Convert]::ToByte($_, 16) }
    )
    return $result
}

try {
    if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
        throw "Sura engine was not found: $Engine"
    }
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Sura AP-startup source was not found: $Source"
    }

    New-Item -ItemType Directory -Path $temp | Out-Null
    $efi = Join-Path $temp "AP_STARTUP.EFI"
    $output = & $Engine --target uefi-x86_64 --out $efi $Source 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "AP-startup feature compile failed:`n$($output -join "`n")"
    }
    if (-not (Test-Path -LiteralPath $efi -PathType Leaf)) {
        throw "AP-startup feature compile did not create an EFI image"
    }

    $bytes = [System.IO.File]::ReadAllBytes($efi)
    if ($bytes.Length -lt 35000 -or
        $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw "AP-startup feature output is not a valid-sized MZ image"
    }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ($bytes[$pe] -ne 0x50 -or $bytes[$pe + 1] -ne 0x45 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) {
        throw "AP-startup feature output is not an x86-64 PE image"
    }
    $utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)
    if ($utf16 -notmatch "Sura AP startup feature test") {
        throw "AP-startup feature output is missing its diagnostic"
    }

    # Exact 230-byte template assembled from the checked x86-64 trampoline.
    $template = Convert-HexBytes @'
fa fc 8c c8 8e d8 8e c0 8e d0 bc f0 0f 66 0f 01
16 b8 00 0f 20 e0 66 83 c8 20 0f 22 e0 66 a1 be
00 0f 22 d8 66 b9 80 00 00 c0 0f 32 66 0d 00 01
00 00 0f 30 0f 20 c0 66 0d 01 00 00 80 0f 22 c0
66 ea 50 00 00 00 08 00 90 89 f6 2e 8d b4 00 00
66 b8 10 00 8e d8 8e c0 8e d0 8e e0 8e e8 48 8b
25 61 00 00 00 48 83 e4 f0 48 8b 05 5e 00 00 00
48 8b 0d 5f 00 00 00 48 8b 15 60 00 00 00 49 c7
c0 01 00 00 00 4c 87 02 48 83 ec 20 ff d0 48 83
c4 20 fa f4 eb fc 66 2e 0f 1f 84 00 00 00 00 00
00 00 00 00 00 00 00 00 ff ff 00 00 00 9a af 00
ff ff 00 00 00 92 cf 00 17 00 a0 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00
'@
    if ($template.Length -ne 230) {
        throw "Internal AP trampoline fixture has $($template.Length) bytes instead of 230"
    }
    if (-not (Test-ByteSequence $bytes $template)) {
        throw "AP-startup feature output does not contain the exact trampoline template"
    }

    Write-Host "sura_ap_startup_smoke: PASS (image=$($bytes.Length), trampoline=$($template.Length) bytes)"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
