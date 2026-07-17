param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/hello_uefi.sura"),
    [string]$FeatureSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/freestanding_features.sura")
)

$ErrorActionPreference = "Stop"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_uefi_" + [guid]::NewGuid().ToString("N"))

function Test-ByteSequence {
    param([byte[]]$Bytes, [byte[]]$Needle)
    if ($Needle.Length -eq 0 -or $Bytes.Length -lt $Needle.Length) { return $false }
    for ($i = 0; $i -le $Bytes.Length - $Needle.Length; $i++) {
        $matched = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Bytes[$i + $j] -ne $Needle[$j]) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $true }
    }
    return $false
}

try {
    if (-not (Test-Path -LiteralPath $Engine)) { throw "Sura engine not found: $Engine" }
    if (-not (Test-Path -LiteralPath $Source)) { throw "Sura UEFI source not found: $Source" }
    if (-not (Test-Path -LiteralPath $FeatureSource)) {
        throw "Sura freestanding feature source not found: $FeatureSource"
    }
    New-Item -ItemType Directory -Path $temp | Out-Null
    $efi = Join-Path $temp "BOOTX64.EFI"

    $output = & $Engine --target uefi-x86_64 --out $efi $Source 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "UEFI compile failed:`n$($output -join "`n")"
    }
    if (-not (Test-Path -LiteralPath $efi)) { throw "UEFI compiler did not create BOOTX64.EFI" }

    $bytes = [System.IO.File]::ReadAllBytes($efi)
    if ($bytes.Length -lt 1024 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw "UEFI output is not a valid-sized MZ image"
    }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ($bytes[$pe] -ne 0x50 -or $bytes[$pe + 1] -ne 0x45) {
        throw "UEFI output has no PE signature"
    }
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) {
        throw "UEFI output is not x86-64"
    }
    if ([BitConverter]::ToUInt16($bytes, $pe + 6) -ne 3) {
        throw "UEFI output should have .text, .data, and .reloc sections"
    }
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20b) {
        throw "UEFI output is not PE32+"
    }
    if ([BitConverter]::ToUInt16($bytes, $optional + 68) -ne 10) {
        throw "PE subsystem is not EFI_APPLICATION"
    }
    if ([BitConverter]::ToUInt32($bytes, $optional + 16) -lt 0x1000) {
        throw "UEFI entry point is invalid"
    }
    $relocRva = [BitConverter]::ToUInt32($bytes, $optional + 112 + 5 * 8)
    $relocSize = [BitConverter]::ToUInt32($bytes, $optional + 112 + 5 * 8 + 4)
    if ($relocRva -eq 0 -or $relocSize -lt 8) {
        throw "UEFI output is missing a base-relocation directory"
    }

    $utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)
    if ($utf16 -notmatch "Sura OS" -or $utf16 -notmatch "GOP framebuffer") {
        throw "UEFI output is missing expected UTF-16 firmware strings"
    }

    $featureEfi = Join-Path $temp "FEATURES.EFI"
    $featureOutput = & $Engine --target uefi-x86_64 --out $featureEfi $FeatureSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding feature compile failed:`n$($featureOutput -join "`n")"
    }
    $featureBytes = [System.IO.File]::ReadAllBytes($featureEfi)
    if ($featureBytes.Length -lt 8192) {
        throw "Freestanding feature image did not retain its static page buffer"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0xa2)))) {
        throw "Freestanding feature image is missing CPUID"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0xf0, 0x48, 0x0f, 0xc1, 0x01)))) {
        throw "Freestanding feature image is missing LOCK XADD"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0xae, 0xf0)))) {
        throw "Freestanding feature image is missing MFENCE"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x48, 0xcf)))) {
        throw "Freestanding feature image is missing IRETQ"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x66, 0x45, 0x89, 0x1a)))) {
        throw "Freestanding feature image is missing IDT gate writes"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x6a, 0x00, 0x50, 0x51, 0x52, 0x53)))) {
        throw "Freestanding feature image is missing the normalized no-error interrupt wrapper"
    }
    $ascii = [System.Text.Encoding]::ASCII.GetString($featureBytes)
    if ($ascii -notmatch "sura-device") {
        throw "Freestanding feature image is missing static UTF-8 data"
    }

    $invalidSource = Join-Path $temp "invalid_interrupt_abi.sura"
    $invalidText = @'
idt is static.zero(4096, 16)

func wrong_page_fault(frame: ptr) interrupt do
  return
end

func efi_main(image: u64, system: ptr) -> u64 do
  cpu.idt_set_gate(idt, 14, addr_of(wrong_page_fault), 8, 0, 142)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidSource,
        $invalidText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $invalidOutput = & $Engine --target uefi-x86_64 --out (Join-Path $temp "INVALID.EFI") $invalidSource 2>&1
        $invalidExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($invalidExitCode -eq 0) {
        throw "Mismatched interrupt error-code ABI was accepted"
    }
    if (($invalidOutput -join "`n") -notmatch "pushes an error code") {
        throw "Mismatched interrupt ABI did not produce the expected diagnostic"
    }

    "sura_uefi_target_smoke: PASS (hello=$($bytes.Length), features=$($featureBytes.Length) bytes)"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        $resolved = [System.IO.Path]::GetFullPath($temp)
        $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolved).StartsWith("sura_uefi_")) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
