param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$Interactive,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "os/sura_os.sura"
$outputDirectory = Join-Path $root "build/os"
$efi = Join-Path $outputDirectory "SuraOS.efi"
$disk = Join-Path $outputDirectory "SuraOS.img"

function Resolve-OsQemu {
    param([string]$Requested)
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        if (-not (Test-Path -LiteralPath $Requested -PathType Leaf)) {
            throw "QEMU was not found: $Requested"
        }
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    $command = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $standard = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (Test-Path -LiteralPath $standard -PathType Leaf) { return $standard }
    throw "QEMU x86-64 was not found. Install QEMU or pass -Qemu <path>."
}

function Resolve-OsFirmware {
    param([string]$Requested, [string]$QemuPath)
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        if (-not (Test-Path -LiteralPath $Requested -PathType Leaf)) {
            throw "UEFI firmware was not found: $Requested"
        }
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    $qemuRoot = Split-Path -Parent $QemuPath
    foreach ($candidate in @(
        (Join-Path $qemuRoot "share/edk2-x86_64-code.fd"),
        (Join-Path $qemuRoot "share/OVMF_CODE.fd"),
        "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
        "C:\Program Files\qemu\share\OVMF_CODE.fd"
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "OVMF/EDK2 x86-64 firmware was not found. Pass -Firmware <path>."
}

try {
    if ($Interactive -and $CompileOnly) {
        throw "Interactive and CompileOnly cannot be used together"
    }
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

    if ($Interactive) {
        $qemuPath = Resolve-OsQemu $Qemu
        $firmwarePath = Resolve-OsFirmware $Firmware $qemuPath
        $interactiveDisk = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_os_interactive_" + [guid]::NewGuid().ToString("N") + ".img")
        Copy-Item -LiteralPath $disk -Destination $interactiveDisk -Force
        try {
            Write-Host "Sura OS interactive shell"
            Write-Host "Type help for commands. Type shutdown to close QEMU."
            & $qemuPath `
                -machine "q35,accel=tcg" `
                -m "256M" `
                -display "none" `
                -monitor "none" `
                -serial "stdio" `
                -no-reboot `
                -drive "if=pflash,format=raw,readonly=on,file=$firmwarePath" `
                -drive "file=$interactiveDisk,format=raw,if=ide" `
                -device "isa-debug-exit,iobase=0xf4,iosize=0x04" `
                -boot "c"
            $qemuExitCode = $LASTEXITCODE
            if ($qemuExitCode -ne 33) {
                throw "Interactive QEMU closed with unexpected exit code $qemuExitCode"
            }
            "sura_os_vm: INTERACTIVE CLOSED (exit=$qemuExitCode)"
        }
        finally {
            if (Test-Path -LiteralPath $interactiveDisk -PathType Leaf) {
                $resolvedInteractiveDisk = [System.IO.Path]::GetFullPath($interactiveDisk)
                $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
                if ($resolvedInteractiveDisk.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
                    (Split-Path -Leaf $resolvedInteractiveDisk).StartsWith("sura_os_interactive_")) {
                    Remove-Item -LiteralPath $resolvedInteractiveDisk -Force
                }
            }
        }
        return
    }

    & (Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1") `
        -Engine $Engine `
        -Source $source `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura OS virtual machine" `
        -ExpectedMarker "SURA_OS_SHUTDOWN" `
        -ExpectedExitCode 33 `
        -SerialInputLines @("status", "mem", "shutdown") `
        -AdditionalExpectedSerialMarkers @("kernel: ready", "free physical pages: ") `
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
