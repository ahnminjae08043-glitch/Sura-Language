param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 45,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1") `
    -Engine $Engine `
    -Source (Join-Path $root "examples/os/power_shutdown_qemu_gate.sura") `
    -Qemu $Qemu `
    -Firmware $Firmware `
    -TimeoutSeconds $TimeoutSeconds `
    -ExpectedEfiText "Sura ACPI power-off executed gate" `
    -ExpectedMarker "SURA_ACPI_POWER_OFF_ARMED" `
    -ExpectedExitCode 0 `
    -AdditionalExpectedSerialMarkers @(
        "SURA_ACPI_FADT_OK",
        "SURA_ACPI_S5_OK"
    ) `
    -CompileOnly:$CompileOnly
if (-not $?) {
    throw "Sura ACPI power-off QEMU gate failed"
}

if ($CompileOnly) {
    "sura_power_shutdown_qemu_gate: COMPILE PASS"
}
else {
    "sura_power_shutdown_qemu_gate: PASS (FADT, DSDT _S5 package, PM1 control, ACPI S5 power-off)"
}
