param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/text_input_features.sura"
$gate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"

$arguments = @{
    Engine = $Engine
    Source = $source
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
    ExpectedEfiText = "Sura Unicode and Korean text input test"
    ExpectedMarker = "SURA_TEXT_INPUT_OK"
    ExpectedExitCode = 33
    AdditionalExpectedSerialMarkers = @(
        "SURA_KEY_EVENT_OK",
        "SURA_TEXT_INPUT_ANNYEONGHASEYO_OK"
    )
}
if ($CompileOnly) { $arguments.CompileOnly = $true }

& $gate @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
