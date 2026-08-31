param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 60,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_nvme_gate_" + [guid]::NewGuid().ToString("N"))
$disk = Join-Path $temp "nvme.img"

try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    $stream = [System.IO.File]::Open(
        $disk,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::Read
    )
    try {
        $stream.SetLength(64MB)
        $initial = New-Object byte[] 512
        for ($index = 0; $index -lt $initial.Length; $index++) {
            $initial[$index] = [byte](($index * 7 + 3) -band 0xff)
        }
        $stream.Position = 8 * 512
        $stream.Write($initial, 0, $initial.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }

    $qemuArguments = @(
        "-drive", "file=$disk,if=none,format=raw,id=sura-nvme-disk,cache=writeback",
        "-device", "nvme,drive=sura-nvme-disk,serial=SURA0001"
    )

    & (Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1") `
        -Engine $Engine `
        -Source (Join-Path $root "examples/os/nvme_qemu_gate.sura") `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura NVMe executed gate" `
        -ExpectedMarker "SURA_NVME_EXECUTED_OK" `
        -ExpectedExitCode 33 `
        -AdditionalExpectedSerialMarkers @(
            "SURA_NVME_PCI_OK",
            "SURA_NVME_ADMIN_QUEUE_OK",
            "SURA_NVME_IDENTIFY_CONTROLLER_OK",
            "SURA_NVME_IDENTIFY_NAMESPACE_OK",
            "SURA_NVME_IO_QUEUE_OK",
            "SURA_NVME_INITIAL_READ_OK",
            "SURA_NVME_WRITE_OK",
            "SURA_NVME_FLUSH_OK",
            "SURA_NVME_READBACK_OK"
        ) `
        -AdditionalQemuArguments $qemuArguments `
        -CompileOnly:$CompileOnly
    if (-not $?) { throw "Sura NVMe QEMU gate failed" }

    if ($CompileOnly) {
        "sura_nvme_qemu_gate: COMPILE PASS"
        return
    }

    $expected = New-Object byte[] 8192
    for ($index = 0; $index -lt $expected.Length; $index++) {
        $expected[$index] = [byte](($index * 13 + 11) -band 0xff)
    }
    $actual = New-Object byte[] 8192
    $stream = [System.IO.File]::Open(
        $disk,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    try {
        $stream.Position = 16 * 512
        $read = $stream.Read($actual, 0, $actual.Length)
    }
    finally {
        $stream.Dispose()
    }
    if ($read -ne $actual.Length) {
        throw "NVMe host disk verification read $read bytes, expected $($actual.Length)"
    }
    for ($index = 0; $index -lt $actual.Length; $index++) {
        if ($actual[$index] -ne $expected[$index]) {
            throw "NVMe host disk verification mismatch at byte $index"
        }
    }
    "sura_nvme_qemu_gate: PASS (PCI/MMIO, admin and I/O queues, Identify, PRP1/PRP2 8-KiB write, flush, device and host readback)"
}
finally {
    if (Test-Path -LiteralPath $temp -PathType Container) {
        $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
        $resolvedSystemTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if (-not $resolvedTemp.StartsWith($resolvedSystemTemp, [System.StringComparison]::OrdinalIgnoreCase) -or
            -not (Split-Path -Leaf $resolvedTemp).StartsWith("sura_nvme_gate_")) {
            throw "Refusing to remove unexpected NVMe gate path: $resolvedTemp"
        }
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
