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
$source = Join-Path $root "examples/os/virtio_gpu_qemu_gate.sura"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_virtio_gpu_" + [guid]::NewGuid().ToString("N"))

function Read-PpmImage {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $cursor = 0
    function Read-PpmToken {
        while ($script:cursor -lt $script:bytes.Length) {
            $value = $script:bytes[$script:cursor]
            if ($value -eq 35) {
                while ($script:cursor -lt $script:bytes.Length -and
                       $script:bytes[$script:cursor] -ne 10) {
                    $script:cursor++
                }
            }
            elseif ($value -eq 9 -or $value -eq 10 -or $value -eq 13 -or $value -eq 32) {
                $script:cursor++
            }
            else {
                break
            }
        }
        $start = $script:cursor
        while ($script:cursor -lt $script:bytes.Length) {
            $value = $script:bytes[$script:cursor]
            if ($value -eq 9 -or $value -eq 10 -or $value -eq 13 -or $value -eq 32 -or $value -eq 35) {
                break
            }
            $script:cursor++
        }
        if ($start -eq $script:cursor) { throw "Invalid PPM token stream" }
        return [System.Text.Encoding]::ASCII.GetString($script:bytes, $start, $script:cursor - $start)
    }

    $script:bytes = $bytes
    $script:cursor = $cursor
    $magic = Read-PpmToken
    $width = [int](Read-PpmToken)
    $height = [int](Read-PpmToken)
    $maximum = [int](Read-PpmToken)
    while ($script:cursor -lt $script:bytes.Length -and
           ($script:bytes[$script:cursor] -eq 9 -or
            $script:bytes[$script:cursor] -eq 10 -or
            $script:bytes[$script:cursor] -eq 13 -or
            $script:bytes[$script:cursor] -eq 32)) {
        $script:cursor++
    }
    if ($magic -ne "P6" -or $width -le 0 -or $height -le 0 -or $maximum -ne 255) {
        throw "Unexpected PPM header: magic=$magic width=$width height=$height max=$maximum"
    }
    if ($script:bytes.Length - $script:cursor -lt $width * $height * 3) {
        throw "PPM pixel payload is truncated"
    }
    return @{
        Bytes = $script:bytes
        Offset = $script:cursor
        Width = $width
        Height = $height
    }
}

function Assert-PpmPixel {
    param(
        [hashtable]$Image,
        [int]$X,
        [int]$Y,
        [int]$Red,
        [int]$Green,
        [int]$Blue
    )
    if ($X -lt 0 -or $Y -lt 0 -or $X -ge $Image.Width -or $Y -ge $Image.Height) {
        throw "PPM sample coordinate is outside the image"
    }
    $offset = $Image.Offset + ($Y * $Image.Width + $X) * 3
    $actual = @(
        [int]$Image.Bytes[$offset],
        [int]$Image.Bytes[$offset + 1],
        [int]$Image.Bytes[$offset + 2]
    )
    if ($actual[0] -ne $Red -or $actual[1] -ne $Green -or $actual[2] -ne $Blue) {
        throw "VirtIO GPU screenshot pixel mismatch at ($X,$Y): expected=($Red,$Green,$Blue) actual=($($actual -join ','))"
    }
}

try {
    New-Item -ItemType Directory -Path $temp | Out-Null
    $screenshot = Join-Path $temp "virtio-gpu.ppm"
    & $bootGate `
        -Engine $Engine `
        -Source $source `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura VirtIO GPU executed gate" `
        -ExpectedMarker "SURA_VIRTIO_GPU_EXECUTED_OK" `
        -AdditionalExpectedSerialMarkers @(
            "SURA_VIRTIO_GPU_PCI_OK",
            "SURA_VIRTIO_GPU_TRANSPORT_OK",
            "SURA_VIRTIO_GPU_DISPLAY_INFO_OK",
            "SURA_VIRTIO_GPU_RESOURCE_OK",
            "SURA_VIRTIO_GPU_BACKING_OK",
            "SURA_VIRTIO_GPU_SCANOUT_OK",
            "SURA_VIRTIO_GPU_TRANSFER_OK",
            "SURA_VIRTIO_GPU_SCREEN_READY",
            "SURA_VIRTIO_GPU_RELEASE_OK"
        ) `
        -AdditionalQemuArguments @(
            "-vga", "none",
            "-device", "virtio-gpu-pci,disable-legacy=on,edid=off,xres=640,yres=480"
        ) `
        -QmpScreendumpPath $screenshot `
        -QmpInputDelayMilliseconds 5000 `
        -SerialInputLines @("x") `
        -SerialInputDelayMilliseconds 100 `
        -HeadlessVnc `
        -CompileOnly:$CompileOnly
    if (-not $?) {
        throw "Sura VirtIO GPU executed QEMU gate failed"
    }

    if ($CompileOnly) {
        "sura_virtio_gpu_qemu_gate: COMPILE PASS"
        return
    }
    if (-not (Test-Path -LiteralPath $screenshot -PathType Leaf)) {
        throw "QEMU did not create the VirtIO GPU screenshot"
    }
    $image = Read-PpmImage $screenshot
    if ($image.Width -ne 640 -or $image.Height -ne 480) {
        throw "Unexpected VirtIO GPU screenshot dimensions: $($image.Width)x$($image.Height)"
    }
    Assert-PpmPixel $image 10 10 12 24 48
    Assert-PpmPixel $image 60 60 240 60 40
    Assert-PpmPixel $image 300 220 40 220 100
    Assert-PpmPixel $image 520 380 50 120 250
    Assert-PpmPixel $image 16 16 245 245 245
    "sura_virtio_gpu_qemu_gate: PASS (modern PCI capabilities, 2D resource, backing, scanout, transfer, flush, 640x480 pixel screenshot)"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        $resolved = [System.IO.Path]::GetFullPath($temp)
        $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolved).StartsWith("sura_virtio_gpu_")) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
