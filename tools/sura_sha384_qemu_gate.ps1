param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/SuraLanguage_os_next.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/sha384_qemu_gate.sura"
$gate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"

$arguments = @{
    Engine = $Engine
    Source = $source
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
    ExpectedEfiText = "Sura SHA-384 test"
    ExpectedMarker = "SURA_SHA384_OK"
    ExpectedExitCode = 33
}
if ($CompileOnly) {
    $arguments.CompileOnly = $true
}

& $gate @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
