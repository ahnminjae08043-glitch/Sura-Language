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
    -Source (Join-Path $root "examples/os/power_reset_qemu_gate.sura") `
    -Qemu $Qemu `
    -Firmware $Firmware `
    -TimeoutSeconds $TimeoutSeconds `
    -ExpectedEfiText "Sura ACPI reset executed gate" `
    -ExpectedMarker "SURA_ACPI_RESET_ARMED" `
    -ExpectedExitCode 0 `
    -AdditionalExpectedSerialMarkers @(
        "SURA_ACPI_RESET_FADT_OK",
        "SURA_ACPI_RESET_REGISTER_OK"
    ) `
    -CompileOnly:$CompileOnly
if (-not $?) {
    throw "Sura ACPI reset QEMU gate failed"
}

if ($CompileOnly) {
    "sura_power_reset_qemu_gate: COMPILE PASS"
}
else {
    "sura_power_reset_qemu_gate: PASS (FADT RESET_REG/RESET_VALUE and QEMU -no-reboot reset exit)"
}
