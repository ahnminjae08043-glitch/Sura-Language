param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 45,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bootGate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"
$source = Join-Path $root "examples/os/xhci_qemu_gate.sura"

& $bootGate `
    -Engine $Engine `
    -Source $source `
    -Qemu $Qemu `
    -Firmware $Firmware `
    -TimeoutSeconds $TimeoutSeconds `
    -ExpectedEfiText "Sura xHCI executed gate" `
    -ExpectedMarker "SURA_XHCI_EXECUTED_OK" `
    -AdditionalExpectedSerialMarkers @(
        "SURA_XHCI_PCI_OK",
        "SURA_XHCI_RESET_OK",
        "SURA_XHCI_PORT_OK",
        "SURA_XHCI_ADDRESS_OK",
        "SURA_XHCI_DESCRIPTOR_OK",
        "SURA_XHCI_HID_DESCRIPTOR_OK",
        "SURA_XHCI_HID_CONFIGURED_OK",
        "SURA_XHCI_HID_ENDPOINT_RUNNING",
        "SURA_XHCI_HID_READY",
        "SURA_XHCI_HID_REPORT_OK",
        "SURA_XHCI_HID_EVENT_OK",
        "SURA_XHCI_COMMAND_OK"
    ) `
    -AdditionalQemuArguments @(
        "-device", "qemu-xhci,id=sura-xhci",
        "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
        "-device", "usb-tablet,bus=sura-xhci.0"
    ) `
    -QmpSendKey "a 10000" `
    -QmpInputDelayMilliseconds 5000 `
    -DisablePs2 `
    -HeadlessVnc `
    -CompileOnly:$CompileOnly
if (-not $?) {
    throw "Sura xHCI executed QEMU gate failed"
}

if ($CompileOnly) {
    "sura_xhci_qemu_gate: COMPILE PASS"
}
else {
    "sura_xhci_qemu_gate: PASS (PCI, reset/run, Address Device, HID endpoint configuration, injected USB key report, Disable Slot)"
}
