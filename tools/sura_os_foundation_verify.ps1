param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
    throw "Sura engine was not found: $Engine"
}

& (Join-Path $PSScriptRoot "sura_uefi_target_smoke.ps1") -Engine $Engine
if (-not $?) { throw "UEFI target smoke failed" }

$common = @{
    Engine = $Engine
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
}
if ($CompileOnly) {
    $common.CompileOnly = $true
}

foreach ($gate in @(
    "sura_integer_semantics_qemu_gate.ps1",
    "sura_ring3_qemu_gate.ps1",
    "sura_ipc_qemu_gate.ps1",
    "sura_process_syscall_qemu_gate.ps1",
    "sura_process_wait_qemu_gate.ps1",
    "sura_user_process_qemu_gate.ps1",
    "sura_user_process_kernel_slice_qemu_gate.ps1",
    "sura_persistent_calculator_qemu_gate.ps1",
    "sura_persistent_desktop_apps_qemu_gate.ps1",
    "sura_window_server_qemu_gate.ps1",
    "sura_text_input_qemu_gate.ps1",
    "sura_memfs_qemu_gate.ps1",
    "sura_fat32_mutation_qemu_gate.ps1",
    "sura_surafs_qemu_gate.ps1",
    "sura_http1_qemu_gate.ps1",
    "sura_gzip_qemu_gate.ps1",
    "sura_png_qemu_gate.ps1",
    "sura_jpeg_qemu_gate.ps1",
    "sura_html_dom_qemu_gate.ps1",
    "sura_css_style_qemu_gate.ps1",
    "sura_css_box_qemu_gate.ps1",
    "sura_virtio_gpu_qemu_gate.ps1",
    "sura_hda_qemu_gate.ps1",
    "sura_nvme_qemu_gate.ps1",
    "sura_os_nvme_qemu_gate.ps1",
    "sura_power_shutdown_qemu_gate.ps1",
    "sura_power_reset_qemu_gate.ps1",
    "sura_os_reboot_qemu_gate.ps1",
    "sura_xhci_qemu_gate.ps1",
    "sura_xhci_mouse_qemu_gate.ps1"
)) {
    & (Join-Path $PSScriptRoot $gate) @common
    if (-not $?) { throw "OS foundation gate failed: $gate" }
}

if ($CompileOnly) {
    "sura_os_foundation_verify: COMPILE PASS"
}
else {
    & (Join-Path $PSScriptRoot "sura_surafs_gui_qemu_gate.ps1") `
        -Engine $Engine `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds ([Math]::Max(60, $TimeoutSeconds))
    if (-not $?) { throw "OS foundation gate failed: sura_surafs_gui_qemu_gate.ps1" }
    "sura_os_foundation_verify: PASS"
}
