param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/hello_uefi.sura")
)

$ErrorActionPreference = "Stop"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_uefi_" + [guid]::NewGuid().ToString("N"))

try {
    if (-not (Test-Path -LiteralPath $Engine)) { throw "Sura engine not found: $Engine" }
    if (-not (Test-Path -LiteralPath $Source)) { throw "Sura UEFI source not found: $Source" }
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

    "sura_uefi_target_smoke: PASS ($($bytes.Length) bytes)"
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
