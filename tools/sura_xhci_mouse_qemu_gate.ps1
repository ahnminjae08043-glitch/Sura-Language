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
$source = Join-Path $root "examples/os/xhci_mouse_qemu_gate.sura"

& $bootGate `
    -Engine $Engine `
    -Source $source `
    -Qemu $Qemu `
    -Firmware $Firmware `
    -TimeoutSeconds $TimeoutSeconds `
    -ExpectedEfiText "Sura xHCI mouse executed gate" `
    -ExpectedMarker "SURA_XHCI_MOUSE_EXECUTED_OK" `
    -AdditionalExpectedSerialMarkers @(
        "SURA_XHCI_MOUSE_PCI_OK",
        "SURA_XHCI_MOUSE_RESET_OK",
        "SURA_XHCI_MOUSE_PORT_OK",
        "SURA_XHCI_MOUSE_ADDRESS_OK",
        "SURA_XHCI_MOUSE_DESCRIPTOR_OK",
        "SURA_XHCI_MOUSE_CONFIGURED_OK",
        "SURA_XHCI_MOUSE_ENDPOINT_RUNNING",
        "SURA_XHCI_MOUSE_READY",
        "SURA_XHCI_MOUSE_REPORT_OK",
        "SURA_XHCI_MOUSE_EVENT_OK",
        "SURA_XHCI_MOUSE_COMMAND_OK"
    ) `
    -AdditionalQemuArguments @(
        "-device", "qemu-xhci,id=sura-xhci",
        "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
        "-device", "usb-mouse,id=sura-mouse,bus=sura-xhci.0"
    ) `
    -QmpMouseDeltaX 40 `
    -QmpMouseDeltaY 20 `
    -QmpInputDelayMilliseconds 5000 `
    -DisablePs2 `
    -HeadlessVnc `
    -CompileOnly:$CompileOnly
if (-not $?) {
    throw "Sura xHCI mouse executed QEMU gate failed"
}

if ($CompileOnly) {
    "sura_xhci_mouse_qemu_gate: COMPILE PASS"
}
else {
    "sura_xhci_mouse_qemu_gate: PASS (second port, Address Device, boot-mouse endpoint, injected USB movement report, Disable Slot)"
}
