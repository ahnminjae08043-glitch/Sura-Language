param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "os/sura_os.sura"
$outputDirectory = Join-Path $root "build/os"
$efi = Join-Path $outputDirectory "SuraOS.efi"
$disk = Join-Path $outputDirectory "SuraOS.img"

try {
    if (-not (Test-Path -LiteralPath $Engine -PathType Leaf)) {
        throw "Sura engine was not found: $Engine"
    }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Sura OS source was not found: $source"
    }
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $compileOutput = & $Engine --target uefi-x86_64 --out $efi --disk-image $disk $source 2>&1
    $compileExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference
    if ($compileExitCode -ne 0) {
        throw "Sura OS compile failed:`n$($compileOutput -join "`n")"
    }

    & (Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1") `
        -Engine $Engine `
        -Source $source `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura OS virtual machine" `
        -ExpectedMarker "SURA_OS_KERNEL_READY" `
        -ExpectedExitCode 33 `
        -CompileOnly:$CompileOnly
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $efiLength = (Get-Item -LiteralPath $efi).Length
    $diskLength = (Get-Item -LiteralPath $disk).Length
    if ($CompileOnly) {
        "sura_os_vm: COMPILE PASS (efi=$efiLength, disk=$diskLength bytes)"
    }
    else {
        "sura_os_vm: PASS (efi=$efiLength, disk=$diskLength bytes)"
    }
}
catch {
    Write-Error $_
    exit 1
}
