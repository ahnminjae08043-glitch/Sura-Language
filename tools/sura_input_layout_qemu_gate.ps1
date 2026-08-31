param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$screenshotGate = Join-Path $PSScriptRoot "sura_os_screenshot.ps1"
$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$token = [guid]::NewGuid().ToString("N")
$persistedDisk = Join-Path $tempRoot "sura_os_input_layout_$token.img"

try {
    & $screenshotGate `
        -Engine $Engine `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -DataDiskOutput $persistedDisk `
        -TimeoutSeconds $TimeoutSeconds
    if (-not $?) { throw "First Sura OS input-layout boot failed" }

    & $screenshotGate `
        -Engine $Engine `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -DataDisk $persistedDisk `
        -SkipInputVerification `
        -ExpectedSerialMarker "SURA_OS_INPUT_LAYOUT_RESTORED" `
        -TimeoutSeconds $TimeoutSeconds
    if (-not $?) { throw "Second Sura OS input-layout restore boot failed" }

    "sura_input_layout_qemu_gate: PASS (Korean layout persisted across reboot)"
}
finally {
    if (Test-Path -LiteralPath $persistedDisk -PathType Leaf) {
        $resolved = [System.IO.Path]::GetFullPath($persistedDisk)
        if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolved).StartsWith("sura_os_input_layout_")) {
            Remove-Item -LiteralPath $resolved -Force
        }
    }
}
