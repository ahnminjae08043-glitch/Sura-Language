param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 60,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dataDisk = Join-Path $root "build/os/SuraData.img"

if (-not (Test-Path -LiteralPath $dataDisk -PathType Leaf)) {
    & (Join-Path $PSScriptRoot "sura_os_data_disk.ps1") -Path $dataDisk
    if (-not $?) { throw "Sura OS reboot gate data disk creation failed" }
}

$qemuArguments = @(
    "-device", "virtio-gpu-pci,disable-legacy=on,edid=off,xres=1280,yres=800",
    "-device", "qemu-xhci,id=sura-xhci",
    "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
    "-device", "usb-mouse,id=sura-mouse,bus=sura-xhci.0",
    "-audiodev", "none,id=sura-audio",
    "-device", "ich9-intel-hda,id=sura-hda,msi=off",
    "-device", "hda-output,audiodev=sura-audio"
)

& (Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1") `
    -Engine $Engine `
    -Source (Join-Path $root "os/sura_os.sura") `
    -DataDisk $dataDisk `
    -EnableNetwork `
    -Qemu $Qemu `
    -Firmware $Firmware `
    -TimeoutSeconds $TimeoutSeconds `
    -ExpectedEfiText "Sura OS virtual machine" `
    -ExpectedMarker "SURA_OS_RESTART" `
    -ExpectedExitCode 0 `
    -SerialInputLines @("reboot") `
    -SerialInputDelayMilliseconds 8000 `
    -AdditionalExpectedSerialMarkers @(
        "SURA_OS_ACPI_POWER_READY",
        "SURA_OS_ACPI_RESET_ARMED",
        "SURA_OS_KERNEL_READY"
    ) `
    -AdditionalQemuArguments $qemuArguments `
    -CompileOnly:$CompileOnly
if (-not $?) {
    throw "Sura OS integrated ACPI reboot QEMU gate failed"
}

if ($CompileOnly) {
    "sura_os_reboot_qemu_gate: COMPILE PASS"
}
else {
    "sura_os_reboot_qemu_gate: PASS (Ring-3 reboot command, OS power service, FADT reset)"
}
