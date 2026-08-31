param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/persistent_calculator_qemu_gate.sura"
$gate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"

if (-not (Test-Path -LiteralPath $gate -PathType Leaf)) {
    throw "Generic QEMU boot gate was not found: $gate"
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Persistent calculator QEMU source was not found: $source"
}

$arguments = @{
    Engine = $Engine
    Source = $source
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
    ExpectedEfiText = "Sura executed persistent calculator QEMU gate"
    ExpectedMarker = "SURA_PERSISTENT_CALCULATOR_READY"
    ExpectedExitCode = 33
    AdditionalExpectedSerialMarkers = @(
        "SURA_PERSISTENT_CALCULATOR_EVENTS_OK",
        "SURA_PERSISTENT_CALCULATOR_RESULT_OK",
        "SURA_PERSISTENT_CALCULATOR_SAME_PROCESS_OK",
        "SURA_PERSISTENT_CALCULATOR_CR3_OK"
    )
}
if ($CompileOnly) {
    $arguments.CompileOnly = $true
}

& $gate @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
