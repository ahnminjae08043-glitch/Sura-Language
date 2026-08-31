param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 90,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_os_nvme_" + [guid]::NewGuid().ToString("N"))
$dataDisk = Join-Path $temp "SuraData-NVMe.img"

try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    & (Join-Path $PSScriptRoot "sura_os_data_disk.ps1") -Path $dataDisk -Force
    if (-not $?) { throw "Sura OS NVMe data disk creation failed" }

    $qemuArguments = @(
        "-device", "virtio-gpu-pci,disable-legacy=on,edid=off,xres=1280,yres=800",
        "-device", "qemu-xhci,id=sura-xhci",
        "-device", "usb-kbd,id=sura-kbd,bus=sura-xhci.0",
        "-device", "usb-mouse,id=sura-mouse,bus=sura-xhci.0",
        "-audiodev", "none,id=sura-audio",
        "-device", "ich9-intel-hda,id=sura-hda,msi=off",
        "-device", "hda-output,audiodev=sura-audio",
        "-drive", "file=$dataDisk,if=none,format=raw,id=sura-os-nvme-data,cache=writeback",
        "-device", "nvme,drive=sura-os-nvme-data,serial=SURAOSNVME"
    )

    & (Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1") `
        -Engine $Engine `
        -Source (Join-Path $root "os/sura_os.sura") `
        -EnableNetwork `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura OS virtual machine" `
        -ExpectedMarker "SURA_OS_SHUTDOWN" `
        -ExpectedExitCode 0 `
        -SerialInputLines @("status", "shutdown") `
        -SerialInputDelayMilliseconds 9000 `
        -SerialInputIntervalMilliseconds 500 `
        -AdditionalExpectedSerialMarkers @(
            "SURA_OS_ACPI_POWER_READY",
            "SURA_OS_STORAGE_READY",
            "SURA_OS_STORAGE_NVME_READY",
            "SURA_OS_STORAGE_READ_OK",
            "SURA_OS_SURAFS_READY",
            "SURA_OS_SETTINGS_READY",
            "SURA_OS_DESKTOP_STATE_READY",
            "SURA_OS_KERNEL_READY",
            "kernel: ready",
            "SURA_OS_ACPI_POWER_OFF_ARMED"
        ) `
        -AdditionalQemuArguments $qemuArguments `
        -CompileOnly:$CompileOnly
    if (-not $?) { throw "Sura OS NVMe integration gate failed" }

    if ($CompileOnly) {
        "sura_os_nvme_qemu_gate: COMPILE PASS"
    }
    else {
        "sura_os_nvme_qemu_gate: PASS (NVMe-first BlockDevice selection, FAT32 settings, SuraFS mount, ACPI shutdown)"
    }
}
finally {
    if (Test-Path -LiteralPath $temp -PathType Container) {
        $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
        $resolvedSystemTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if (-not $resolvedTemp.StartsWith($resolvedSystemTemp, [System.StringComparison]::OrdinalIgnoreCase) -or
            -not (Split-Path -Leaf $resolvedTemp).StartsWith("sura_os_nvme_")) {
            throw "Refusing to remove unexpected Sura OS NVMe path: $resolvedTemp"
        }
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
