param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 60,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/fat32_mutation_qemu_gate.sura"
$gate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"

if (-not (Test-Path -LiteralPath $gate -PathType Leaf)) {
    throw "Generic QEMU boot gate was not found: $gate"
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "FAT32 mutation QEMU source was not found: $source"
}

$arguments = @{
    Engine = $Engine
    Source = $source
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
    ExpectedEfiText = "Sura FAT32 persistent mutation feature test"
    ExpectedMarker = "SURA_FAT32_MUTATION_OK"
    AdditionalExpectedSerialMarkers = @(
        "SURA_FAT32_REMOUNT_OK",
        "SURA_FAT32_LFN_UTF8_OK",
        "SURA_FAT32_LFN_CORRUPT_OK",
        "SURA_FAT32_VFS_UTF8_OK",
        "SURA_FAT32_VFS_REMOUNT_OK"
    )
    ExpectedExitCode = 33
}
if ($CompileOnly) {
    $arguments.CompileOnly = $true
}

& $gate @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
