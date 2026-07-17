param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/hello_uefi.sura"),
    [string]$FeatureSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/freestanding_features.sura"),
    [string]$MemorySource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/memory_kernel.sura"),
    [string]$SchedulerSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/scheduler_features.sura"),
    [string]$PreemptiveSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/preemptive_timer_features.sura"),
    [string]$SyscallSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/syscall_features.sura"),
    [string]$UserModeSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/user_mode_features.sura"),
    [string]$PciSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/pci_features.sura"),
    [string]$AcpiSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/acpi_features.sura"),
    [string]$ApStartupSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/ap_startup_features.sura"),
    [string]$BlockSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/block_features.sura"),
    [string]$Fat32Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/fat32_features.sura"),
    [string]$VfsSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/vfs_features.sura"),
    [string]$GptSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/gpt_features.sura"),
    [string]$PartitionSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/partition_features.sura"),
    [string]$AhciSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/ahci_features.sura"),
    [string]$NvmeSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/nvme_features.sura")
)

$ErrorActionPreference = "Stop"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_uefi_" + [guid]::NewGuid().ToString("N"))

function Test-ByteSequence {
    param([byte[]]$Bytes, [byte[]]$Needle)
    if ($Needle.Length -eq 0 -or $Bytes.Length -lt $Needle.Length) { return $false }
    for ($i = 0; $i -le $Bytes.Length - $Needle.Length; $i++) {
        $matched = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Bytes[$i + $j] -ne $Needle[$j]) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $true }
    }
    return $false
}

function Invoke-ExpectedCompileFailure {
    param(
        [string]$EnginePath,
        [string]$SourcePath,
        [string]$OutputPath,
        [string]$Pattern,
        [string]$Description
    )
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $compilerOutput = & $EnginePath --target uefi-x86_64 --out $OutputPath $SourcePath 2>&1
        $compilerExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($compilerExitCode -eq 0) {
        throw "$Description was accepted"
    }
    if (($compilerOutput -join "`n") -notmatch $Pattern) {
        throw "$Description did not produce the expected diagnostic"
    }
}

try {
    if (-not (Test-Path -LiteralPath $Engine)) { throw "Sura engine not found: $Engine" }
    if (-not (Test-Path -LiteralPath $Source)) { throw "Sura UEFI source not found: $Source" }
    if (-not (Test-Path -LiteralPath $FeatureSource)) {
        throw "Sura freestanding feature source not found: $FeatureSource"
    }
    if (-not (Test-Path -LiteralPath $MemorySource)) {
        throw "Sura freestanding memory source not found: $MemorySource"
    }
    if (-not (Test-Path -LiteralPath $SchedulerSource)) {
        throw "Sura freestanding scheduler source not found: $SchedulerSource"
    }
    if (-not (Test-Path -LiteralPath $PreemptiveSource)) {
        throw "Sura freestanding preemptive-timer source not found: $PreemptiveSource"
    }
    if (-not (Test-Path -LiteralPath $SyscallSource)) {
        throw "Sura freestanding syscall source not found: $SyscallSource"
    }
    if (-not (Test-Path -LiteralPath $UserModeSource)) {
        throw "Sura freestanding user-mode source not found: $UserModeSource"
    }
    if (-not (Test-Path -LiteralPath $PciSource)) {
        throw "Sura freestanding PCI source not found: $PciSource"
    }
    if (-not (Test-Path -LiteralPath $AcpiSource)) {
        throw "Sura freestanding ACPI source not found: $AcpiSource"
    }
    if (-not (Test-Path -LiteralPath $ApStartupSource)) {
        throw "Sura freestanding AP-startup source not found: $ApStartupSource"
    }
    if (-not (Test-Path -LiteralPath $BlockSource)) {
        throw "Sura freestanding block-device source not found: $BlockSource"
    }
    if (-not (Test-Path -LiteralPath $Fat32Source)) {
        throw "Sura freestanding FAT32 source not found: $Fat32Source"
    }
    if (-not (Test-Path -LiteralPath $VfsSource)) {
        throw "Sura freestanding VFS source not found: $VfsSource"
    }
    if (-not (Test-Path -LiteralPath $GptSource)) {
        throw "Sura freestanding GPT source not found: $GptSource"
    }
    if (-not (Test-Path -LiteralPath $PartitionSource)) {
        throw "Sura freestanding partition source not found: $PartitionSource"
    }
    if (-not (Test-Path -LiteralPath $AhciSource)) {
        throw "Sura freestanding AHCI source not found: $AhciSource"
    }
    if (-not (Test-Path -LiteralPath $NvmeSource)) {
        throw "Sura freestanding NVMe source not found: $NvmeSource"
    }
    New-Item -ItemType Directory -Path $temp | Out-Null
    $efi = Join-Path $temp "BOOTX64.EFI"
    $disk = Join-Path $temp "sura-os.img"

    $output = & $Engine --target uefi-x86_64 --out $efi --disk-image $disk $Source 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "UEFI compile failed:`n$($output -join "`n")"
    }
    if (-not (Test-Path -LiteralPath $efi)) { throw "UEFI compiler did not create BOOTX64.EFI" }
    if (-not (Test-Path -LiteralPath $disk)) { throw "UEFI compiler did not create the disk image" }
    if (($output -join "`n") -notmatch "\[UEFI DISK\]") {
        throw "UEFI compiler did not report the disk image"
    }

    $bytes = [System.IO.File]::ReadAllBytes($efi)
    if ($bytes.Length -lt 1024 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw "UEFI output is not a valid-sized MZ image"
    }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3c)
    if ($bytes[$pe] -ne 0x50 -or $bytes[$pe + 1] -ne 0x45) {
        throw "UEFI output has no PE signature"
    }
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) {
        throw "UEFI output is not x86-64"
    }
    if ([BitConverter]::ToUInt16($bytes, $pe + 6) -ne 3) {
        throw "UEFI output should have .text, .data, and .reloc sections"
    }
    $optional = $pe + 24
    if ([BitConverter]::ToUInt16($bytes, $optional) -ne 0x20b) {
        throw "UEFI output is not PE32+"
    }
    if ([BitConverter]::ToUInt16($bytes, $optional + 68) -ne 10) {
        throw "PE subsystem is not EFI_APPLICATION"
    }
    if ([BitConverter]::ToUInt32($bytes, $optional + 16) -lt 0x1000) {
        throw "UEFI entry point is invalid"
    }
    $relocRva = [BitConverter]::ToUInt32($bytes, $optional + 112 + 5 * 8)
    $relocSize = [BitConverter]::ToUInt32($bytes, $optional + 112 + 5 * 8 + 4)
    if ($relocRva -eq 0 -or $relocSize -lt 8) {
        throw "UEFI output is missing a base-relocation directory"
    }

    $utf16 = [System.Text.Encoding]::Unicode.GetString($bytes)
    if ($utf16 -notmatch "Sura OS" -or $utf16 -notmatch "GOP framebuffer") {
        throw "UEFI output is missing expected UTF-16 firmware strings"
    }

    $diskBytes = [System.IO.File]::ReadAllBytes($disk)
    if ($diskBytes.Length -lt 64MB -or
        $diskBytes[510] -ne 0x55 -or $diskBytes[511] -ne 0xaa -or
        $diskBytes[450] -ne 0xee) {
        throw "UEFI disk image is missing its protective MBR"
    }
    if ([System.Text.Encoding]::ASCII.GetString($diskBytes, 512, 8) -ne "EFI PART") {
        throw "UEFI disk image is missing its primary GPT header"
    }
    $partitionLba = [BitConverter]::ToUInt64($diskBytes, 2 * 512 + 32)
    if ($partitionLba -ne 2048) {
        throw "UEFI disk image has an unexpected ESP start"
    }
    $partitionOffset = [int64]$partitionLba * 512
    if ([System.Text.Encoding]::ASCII.GetString($diskBytes, $partitionOffset + 82, 8) -ne "FAT32   " -or
        $diskBytes[$partitionOffset + 510] -ne 0x55 -or
        $diskBytes[$partitionOffset + 511] -ne 0xaa) {
        throw "UEFI disk image is missing a valid FAT32 ESP boot sector"
    }
    $reservedSectors = [BitConverter]::ToUInt16($diskBytes, $partitionOffset + 14)
    $fatCopies = $diskBytes[$partitionOffset + 16]
    $fatSectors = [BitConverter]::ToUInt32($diskBytes, $partitionOffset + 36)
    $dataLba = $partitionLba + $reservedSectors + [int64]$fatCopies * $fatSectors
    $rootOffset = [int64]$dataLba * 512
    $efiDirectoryOffset = ([int64]$dataLba + 1) * 512
    $bootDirectoryOffset = ([int64]$dataLba + 2) * 512
    $bootFileOffset = ([int64]$dataLba + 3) * 512
    if ([System.Text.Encoding]::ASCII.GetString($diskBytes, $rootOffset, 11) -ne "EFI        " -or
        [System.Text.Encoding]::ASCII.GetString($diskBytes, $efiDirectoryOffset + 64, 11) -ne "BOOT       " -or
        [System.Text.Encoding]::ASCII.GetString($diskBytes, $bootDirectoryOffset + 64, 11) -ne "BOOTX64 EFI") {
        throw "UEFI disk image does not contain EFI/BOOT/BOOTX64.EFI"
    }
    $embeddedSize = [BitConverter]::ToUInt32($diskBytes, $bootDirectoryOffset + 64 + 28)
    if ($embeddedSize -ne $bytes.Length) {
        throw "UEFI disk image recorded the wrong BOOTX64.EFI size"
    }
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        if ($diskBytes[$bootFileOffset + $i] -ne $bytes[$i]) {
            throw "UEFI disk image BOOTX64.EFI payload differs at byte $i"
        }
    }

    $featureEfi = Join-Path $temp "FEATURES.EFI"
    $featureOutput = & $Engine --target uefi-x86_64 --out $featureEfi $FeatureSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding feature compile failed:`n$($featureOutput -join "`n")"
    }
    $featureBytes = [System.IO.File]::ReadAllBytes($featureEfi)
    if ($featureBytes.Length -lt 8192) {
        throw "Freestanding feature image did not retain its static page buffer"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0xa2)))) {
        throw "Freestanding feature image is missing CPUID"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0xf0, 0x48, 0x0f, 0xc1, 0x01)))) {
        throw "Freestanding feature image is missing LOCK XADD"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0xae, 0xf0)))) {
        throw "Freestanding feature image is missing MFENCE"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x48, 0xcf)))) {
        throw "Freestanding feature image is missing IRETQ"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x66, 0x45, 0x89, 0x1a)))) {
        throw "Freestanding feature image is missing IDT gate writes"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x6a, 0x00, 0x50, 0x51, 0x52, 0x53)))) {
        throw "Freestanding feature image is missing the normalized no-error interrupt wrapper"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x41, 0xc6, 0x42, 0x05, 0x89)))) {
        throw "Freestanding feature image is missing the 64-bit TSS descriptor"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0x00, 0xd8)))) {
        throw "Freestanding feature image is missing LTR"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0x01, 0xf8)))) {
        throw "Freestanding feature image is missing SWAPGS"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x48, 0x0f, 0xae, 0x00)))) {
        throw "Freestanding feature image is missing FXSAVE64"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x49, 0x0f, 0xae, 0x23)))) {
        throw "Freestanding feature image is missing XSAVE64"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x65, 0x48, 0x8b, 0x00)))) {
        throw "Freestanding feature image is missing GS-relative per-CPU read"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0xb9, 0x30, 0x08, 0x00, 0x00, 0x0f, 0x30)))) {
        throw "Freestanding feature image is missing x2APIC ICR write"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x41, 0x89, 0x82, 0x10, 0x03, 0x00, 0x00)))) {
        throw "Freestanding feature image is missing xAPIC ICR-high write"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x48, 0xc1, 0xe8, 0x27, 0x25, 0xff, 0x01, 0x00, 0x00)))) {
        throw "Freestanding feature image is missing PML4 index lowering"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x48, 0xba, 0x00, 0xf0, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x48, 0x21, 0xd0)))) {
        throw "Freestanding feature image is missing page-entry address masking"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x48, 0x01, 0xd1, 0x48, 0x89, 0x01)))) {
        throw "Freestanding feature image is missing page-table entry write"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0x01, 0x38)))) {
        throw "Freestanding feature image is missing INVLPG"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x0f, 0x20, 0xd8, 0x0f, 0x22, 0xd8)))) {
        throw "Freestanding feature image is missing CR3 TLB flush"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57)))) {
        throw "Freestanding feature image is missing context-switch register save"
    }
    if (-not (Test-ByteSequence $featureBytes ([byte[]](0xfc, 0x4c, 0x89, 0xe9, 0x48, 0x83, 0xec, 0x20, 0x41, 0xff, 0xd4)))) {
        throw "Freestanding feature image is missing task bootstrap"
    }
    $ascii = [System.Text.Encoding]::ASCII.GetString($featureBytes)
    if ($ascii -notmatch "sura-device") {
        throw "Freestanding feature image is missing static UTF-8 data"
    }

    $memoryEfi = Join-Path $temp "MEMORY.EFI"
    $memoryOutput = & $Engine --target uefi-x86_64 --out $memoryEfi $MemorySource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding memory-kernel compile failed:`n$($memoryOutput -join "`n")"
    }
    $memoryBytes = [System.IO.File]::ReadAllBytes($memoryEfi)
    if ($memoryBytes.Length -lt 150000 -or
        $memoryBytes[0] -ne 0x4d -or $memoryBytes[1] -ne 0x5a) {
        throw "Freestanding memory-kernel image did not retain its allocator and page tables"
    }
    $memoryUtf16 = [System.Text.Encoding]::Unicode.GetString($memoryBytes)
    if ($memoryUtf16 -notmatch "Sura memory kernel" -or
        $memoryUtf16 -notmatch "ExitBootServices failed") {
        throw "Freestanding memory-kernel image is missing lifecycle diagnostics"
    }
    if (-not (Test-ByteSequence $memoryBytes ([byte[]](0x0f, 0x01, 0x38)))) {
        throw "Freestanding memory-kernel image is missing page-map INVLPG"
    }
    if (-not (Test-ByteSequence $memoryBytes ([byte[]](0xfa, 0x48, 0xb8, 0x01, 0x00, 0x00, 0x00))) -or
        -not (Test-ByteSequence $memoryBytes ([byte[]](0xf4, 0xe9)))) {
        throw "Freestanding memory-kernel image is missing its post-boot interrupt-disable/halt path"
    }
    if (-not (Test-ByteSequence $memoryBytes ([byte[]](0x48, 0xc1, 0xe8, 0x27, 0x25, 0xff, 0x01, 0x00, 0x00)))) {
        throw "Freestanding memory-kernel image is missing imported page-table walking"
    }

    $schedulerEfi = Join-Path $temp "SCHEDULER.EFI"
    $schedulerOutput = & $Engine --target uefi-x86_64 --out $schedulerEfi $SchedulerSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding scheduler feature compile failed:`n$($schedulerOutput -join "`n")"
    }
    $schedulerBytes = [System.IO.File]::ReadAllBytes($schedulerEfi)
    if ($schedulerBytes.Length -lt 40000 -or
        $schedulerBytes[0] -ne 0x4d -or $schedulerBytes[1] -ne 0x5a) {
        throw "Freestanding scheduler feature image did not retain task tables and stacks"
    }
    $schedulerUtf16 = [System.Text.Encoding]::Unicode.GetString($schedulerBytes)
    if ($schedulerUtf16 -notmatch "Sura scheduler feature test") {
        throw "Freestanding scheduler feature image is missing its diagnostic"
    }
    if (-not (Test-ByteSequence $schedulerBytes ([byte[]](0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x89, 0x21, 0x48, 0x89, 0xd4)))) {
        throw "Freestanding scheduler feature image is missing integer context save/switch"
    }
    if (-not (Test-ByteSequence $schedulerBytes ([byte[]](0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5e, 0x5f, 0x5d, 0x5b, 0xc3)))) {
        throw "Freestanding scheduler feature image is missing integer context restore"
    }
    if (-not (Test-ByteSequence $schedulerBytes ([byte[]](0xfc, 0x4c, 0x89, 0xe9, 0x48, 0x83, 0xec, 0x20, 0x41, 0xff, 0xd4)))) {
        throw "Freestanding scheduler feature image is missing the task bootstrap"
    }

    $preemptiveEfi = Join-Path $temp "PREEMPTIVE_TIMER.EFI"
    $preemptiveOutput = & $Engine --target uefi-x86_64 --out $preemptiveEfi $PreemptiveSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding preemptive-timer feature compile failed:`n$($preemptiveOutput -join "`n")"
    }
    $preemptiveBytes = [System.IO.File]::ReadAllBytes($preemptiveEfi)
    if ($preemptiveBytes.Length -lt 52000 -or
        $preemptiveBytes[0] -ne 0x4d -or $preemptiveBytes[1] -ne 0x5a) {
        throw "Freestanding preemptive-timer feature image is invalid"
    }
    $preemptiveUtf16 = [System.Text.Encoding]::Unicode.GetString($preemptiveBytes)
    if ($preemptiveUtf16 -notmatch "Sura preemptive timer feature test") {
        throw "Freestanding preemptive-timer feature image is missing its diagnostic"
    }
    foreach ($requiredSequence in @(
        ([byte[]](0x48, 0x2d, 0x98, 0x00, 0x00, 0x00)),
        ([byte[]](0xfa, 0x4c, 0x89, 0xd4, 0x41, 0x5f, 0x41, 0x5e)),
        ([byte[]](0x48, 0xcf)),
        ([byte[]](0xcd, 0x81)),
        ([byte[]](0x0f, 0x30)),
        ([byte[]](0x48, 0xb8, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00))
    )) {
        if (-not (Test-ByteSequence $preemptiveBytes $requiredSequence)) {
            throw "Freestanding preemptive-timer image is missing a required timer or frame-switch sequence"
        }
    }

    $syscallEfi = Join-Path $temp "SYSCALL.EFI"
    $syscallOutput = & $Engine --target uefi-x86_64 --out $syscallEfi $SyscallSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding syscall feature compile failed:`n$($syscallOutput -join "`n")"
    }
    $syscallBytes = [System.IO.File]::ReadAllBytes($syscallEfi)
    if ($syscallBytes.Length -lt 7000 -or
        $syscallBytes[0] -ne 0x4d -or $syscallBytes[1] -ne 0x5a) {
        throw "Freestanding syscall feature image is invalid"
    }
    $syscallUtf16 = [System.Text.Encoding]::Unicode.GetString($syscallBytes)
    if ($syscallUtf16 -notmatch "Sura software-interrupt syscall feature test") {
        throw "Freestanding syscall feature image is missing its diagnostic"
    }
    if (-not (Test-ByteSequence $syscallBytes ([byte[]](0xcd, 0x80, 0x5e, 0x5f)))) {
        throw "Freestanding syscall feature image is missing INT 0x80 invocation"
    }
    if (-not (Test-ByteSequence $syscallBytes ([byte[]](0x41, 0xff, 0xd3)))) {
        throw "Freestanding syscall feature image is missing indirect handler call"
    }
    if (-not (Test-ByteSequence $syscallBytes ([byte[]](0x48, 0xcf)))) {
        throw "Freestanding syscall feature image is missing interrupt return"
    }

    $userModeEfi = Join-Path $temp "USER_MODE.EFI"
    $userModeOutput = & $Engine --target uefi-x86_64 --out $userModeEfi $UserModeSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding user-mode feature compile failed:`n$($userModeOutput -join "`n")"
    }
    $userModeBytes = [System.IO.File]::ReadAllBytes($userModeEfi)
    if ($userModeBytes.Length -lt 39000 -or
        $userModeBytes[0] -ne 0x4d -or $userModeBytes[1] -ne 0x5a) {
        throw "Freestanding user-mode feature image is invalid"
    }
    $userModeUtf16 = [System.Text.Encoding]::Unicode.GetString($userModeBytes)
    if ($userModeUtf16 -notmatch "Sura ring-3 and fast syscall feature test") {
        throw "Freestanding user-mode feature image is missing its diagnostic"
    }
    foreach ($requiredSequence in @(
        ([byte[]](0x0f, 0x05, 0x5e, 0x5f)),
        ([byte[]](0x65, 0x48, 0x89, 0x24, 0x25, 0x08, 0x00, 0x00, 0x00)),
        ([byte[]](0x65, 0x48, 0x8b, 0x24, 0x25, 0x00, 0x00, 0x00, 0x00)),
        ([byte[]](0x48, 0x81, 0x65, 0x20, 0xd7, 0x0a, 0x00, 0x00)),
        ([byte[]](0x48, 0x0f, 0x07)),
        ([byte[]](0x68, 0x1b, 0x00, 0x00, 0x00, 0x41, 0x57, 0x68, 0x02, 0x02, 0x00, 0x00, 0x68, 0x23, 0x00, 0x00, 0x00, 0x41, 0x54, 0xfa, 0x0f, 0x01, 0xf8, 0x48, 0xcf))
    )) {
        if (-not (Test-ByteSequence $userModeBytes $requiredSequence)) {
            throw "Freestanding user-mode feature image is missing a required ring-transition sequence"
        }
    }

    $pciEfi = Join-Path $temp "PCI.EFI"
    $pciOutput = & $Engine --target uefi-x86_64 --out $pciEfi $PciSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding PCI feature compile failed:`n$($pciOutput -join "`n")"
    }
    $pciBytes = [System.IO.File]::ReadAllBytes($pciEfi)
    if ($pciBytes.Length -lt 12000 -or
        $pciBytes[0] -ne 0x4d -or $pciBytes[1] -ne 0x5a) {
        throw "Freestanding PCI feature image is invalid"
    }
    $pciUtf16 = [System.Text.Encoding]::Unicode.GetString($pciBytes)
    if ($pciUtf16 -notmatch "Sura PCI configuration feature test") {
        throw "Freestanding PCI feature image is missing its diagnostic"
    }
    if (-not (Test-ByteSequence $pciBytes ([byte[]](0x48, 0xb8, 0xf8, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00))) -or
        -not (Test-ByteSequence $pciBytes ([byte[]](0x48, 0xb8, 0xfc, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)))) {
        throw "Freestanding PCI feature image is missing legacy configuration ports"
    }
    if (-not (Test-ByteSequence $pciBytes ([byte[]](0x66, 0x89, 0xc2, 0x31, 0xc0, 0xed)))) {
        throw "Freestanding PCI feature image is missing configuration input"
    }
    if (-not (Test-ByteSequence $pciBytes ([byte[]](0x66, 0xef))) -or
        -not (Test-ByteSequence $pciBytes ([byte[]](0xee)))) {
        throw "Freestanding PCI feature image is missing narrow configuration output"
    }

    $acpiEfi = Join-Path $temp "ACPI.EFI"
    $acpiOutput = & $Engine --target uefi-x86_64 --out $acpiEfi $AcpiSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding ACPI feature compile failed:`n$($acpiOutput -join "`n")"
    }
    $acpiBytes = [System.IO.File]::ReadAllBytes($acpiEfi)
    if ($acpiBytes.Length -lt 16000 -or
        $acpiBytes[0] -ne 0x4d -or $acpiBytes[1] -ne 0x5a) {
        throw "Freestanding ACPI feature image is invalid"
    }
    $acpiUtf16 = [System.Text.Encoding]::Unicode.GetString($acpiBytes)
    if ($acpiUtf16 -notmatch "Sura ACPI MADT feature test") {
        throw "Freestanding ACPI feature image is missing its diagnostic"
    }
    if (-not (Test-ByteSequence $acpiBytes ([byte[]](0x48, 0xb8, 0x52, 0x53, 0x44, 0x20, 0x50, 0x54, 0x52, 0x20)))) {
        throw "Freestanding ACPI feature image is missing the RSDP signature check"
    }
    if (-not (Test-ByteSequence $acpiBytes ([byte[]](0x48, 0xb8, 0x41, 0x50, 0x49, 0x43, 0x00, 0x00, 0x00, 0x00)))) {
        throw "Freestanding ACPI feature image is missing the MADT signature check"
    }

    $apStartupEfi = Join-Path $temp "AP_STARTUP.EFI"
    $apStartupOutput = & $Engine --target uefi-x86_64 --out $apStartupEfi $ApStartupSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding AP-startup feature compile failed:`n$($apStartupOutput -join "`n")"
    }
    $apStartupBytes = [System.IO.File]::ReadAllBytes($apStartupEfi)
    if ($apStartupBytes.Length -lt 35000 -or
        $apStartupBytes[0] -ne 0x4d -or $apStartupBytes[1] -ne 0x5a) {
        throw "Freestanding AP-startup feature image is invalid"
    }
    $apStartupUtf16 = [System.Text.Encoding]::Unicode.GetString($apStartupBytes)
    if ($apStartupUtf16 -notmatch "Sura AP startup feature test") {
        throw "Freestanding AP-startup feature image is missing its diagnostic"
    }
    foreach ($requiredSequence in @(
        ([byte[]](0xfa, 0xfc, 0x8c, 0xc8, 0x8e, 0xd8, 0x8e, 0xc0, 0x8e, 0xd0)),
        ([byte[]](0x0f, 0x22, 0xd8)),
        ([byte[]](0x0f, 0x30)),
        ([byte[]](0x66, 0xea, 0x50, 0x00, 0x00, 0x00, 0x08, 0x00)),
        ([byte[]](0x4c, 0x87, 0x02))
    )) {
        if (-not (Test-ByteSequence $apStartupBytes $requiredSequence)) {
            throw "Freestanding AP-startup feature image is missing a required trampoline sequence"
        }
    }

    $blockEfi = Join-Path $temp "BLOCK.EFI"
    $blockOutput = & $Engine --target uefi-x86_64 --out $blockEfi $BlockSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding block-device feature compile failed:`n$($blockOutput -join "`n")"
    }
    $blockBytes = [System.IO.File]::ReadAllBytes($blockEfi)
    if ($blockBytes.Length -lt 55000 -or
        $blockBytes[0] -ne 0x4d -or $blockBytes[1] -ne 0x5a) {
        throw "Freestanding block-device feature image is invalid"
    }
    $blockUtf16 = [System.Text.Encoding]::Unicode.GetString($blockBytes)
    if ($blockUtf16 -notmatch "Sura block device feature test") {
        throw "Freestanding block-device feature image is missing its diagnostic"
    }
    $blockIoGuid = [byte[]](33, 91, 78, 150, 89, 100, 210, 17, 142, 57, 0, 160, 201, 105, 114, 59)
    if (-not (Test-ByteSequence $blockBytes $blockIoGuid)) {
        throw "Freestanding block-device feature image is missing the UEFI Block I/O GUID"
    }

    $fat32Efi = Join-Path $temp "FAT32.EFI"
    $fat32Output = & $Engine --target uefi-x86_64 --out $fat32Efi $Fat32Source 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding FAT32 feature compile failed:`n$($fat32Output -join "`n")"
    }
    $fat32Bytes = [System.IO.File]::ReadAllBytes($fat32Efi)
    if ($fat32Bytes.Length -lt 160000 -or
        $fat32Bytes[0] -ne 0x4d -or $fat32Bytes[1] -ne 0x5a) {
        throw "Freestanding FAT32 feature image is invalid"
    }
    $fat32Utf16 = [System.Text.Encoding]::Unicode.GetString($fat32Bytes)
    if ($fat32Utf16 -notmatch "Sura FAT32 feature test") {
        throw "Freestanding FAT32 feature image is missing its diagnostic"
    }
    $fat32ShortName = [System.Text.Encoding]::ASCII.GetBytes("KERNEL  BIN")
    if (-not (Test-ByteSequence $fat32Bytes $fat32ShortName)) {
        throw "Freestanding FAT32 feature image is missing its fixed 8.3 lookup name"
    }

    $vfsEfi = Join-Path $temp "VFS.EFI"
    $vfsOutput = & $Engine --target uefi-x86_64 --out $vfsEfi $VfsSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding VFS feature compile failed:`n$($vfsOutput -join "`n")"
    }
    $vfsBytes = [System.IO.File]::ReadAllBytes($vfsEfi)
    if ($vfsBytes.Length -lt 14000 -or
        $vfsBytes[0] -ne 0x4d -or $vfsBytes[1] -ne 0x5a) {
        throw "Freestanding VFS feature image is invalid"
    }
    $vfsUtf16 = [System.Text.Encoding]::Unicode.GetString($vfsBytes)
    if ($vfsUtf16 -notmatch "Sura VFS feature test") {
        throw "Freestanding VFS feature image is missing its diagnostic"
    }
    $vfsPath = [System.Text.Encoding]::ASCII.GetBytes("/BOOT.BIN")
    if (-not (Test-ByteSequence $vfsBytes $vfsPath)) {
        throw "Freestanding VFS feature image is missing its mount-dispatch path"
    }

    $gptEfi = Join-Path $temp "GPT.EFI"
    $gptOutput = & $Engine --target uefi-x86_64 --out $gptEfi $GptSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding GPT feature compile failed:`n$($gptOutput -join "`n")"
    }
    $gptBytes = [System.IO.File]::ReadAllBytes($gptEfi)
    if ($gptBytes.Length -lt 230000 -or
        $gptBytes[0] -ne 0x4d -or $gptBytes[1] -ne 0x5a) {
        throw "Freestanding GPT feature image is invalid"
    }
    $gptUtf16 = [System.Text.Encoding]::Unicode.GetString($gptBytes)
    if ($gptUtf16 -notmatch "Sura GPT feature test") {
        throw "Freestanding GPT feature image is missing its diagnostic"
    }
    $efiSystemGuid = [byte[]](40, 115, 42, 193, 31, 248, 210, 17, 186, 75, 0, 160, 201, 62, 201, 59)
    if (-not (Test-ByteSequence $gptBytes $efiSystemGuid)) {
        throw "Freestanding GPT feature image is missing the EFI System Partition GUID"
    }

    $partitionEfi = Join-Path $temp "PARTITION.EFI"
    $partitionOutput = & $Engine --target uefi-x86_64 --out $partitionEfi $PartitionSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding partition feature compile failed:`n$($partitionOutput -join "`n")"
    }
    $partitionBytes = [System.IO.File]::ReadAllBytes($partitionEfi)
    if ($partitionBytes.Length -lt 38000 -or
        $partitionBytes[0] -ne 0x4d -or $partitionBytes[1] -ne 0x5a) {
        throw "Freestanding partition feature image is invalid"
    }
    $partitionUtf16 = [System.Text.Encoding]::Unicode.GetString($partitionBytes)
    if ($partitionUtf16 -notmatch "Sura partition feature test") {
        throw "Freestanding partition feature image is missing its diagnostic"
    }
    foreach ($requiredSequence in @(
        ([byte[]](0x20, 0x83, 0xb8, 0xed)),
        ([byte[]](0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11)),
        ([byte[]](0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b))
    )) {
        if (-not (Test-ByteSequence $partitionBytes $requiredSequence)) {
            throw "Freestanding partition feature image is missing a required GPT sequence"
        }
    }

    $ahciEfi = Join-Path $temp "AHCI.EFI"
    $ahciOutput = & $Engine --target uefi-x86_64 --out $ahciEfi $AhciSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding AHCI feature compile failed:`n$($ahciOutput -join "`n")"
    }
    $ahciBytes = [System.IO.File]::ReadAllBytes($ahciEfi)
    if ($ahciBytes.Length -lt 62000 -or
        $ahciBytes[0] -ne 0x4d -or $ahciBytes[1] -ne 0x5a) {
        throw "Freestanding AHCI feature image is invalid"
    }
    $ahciUtf16 = [System.Text.Encoding]::Unicode.GetString($ahciBytes)
    if ($ahciUtf16 -notmatch "Sura AHCI feature test") {
        throw "Freestanding AHCI feature image is missing its diagnostic"
    }
    foreach ($requiredSequence in @(
        ([byte[]](0xff, 0x0f, 0x00, 0x80)),
        ([byte[]](0x00, 0x20, 0x10, 0x00)),
        ([byte[]](0x00, 0x30, 0x10, 0x00))
    )) {
        if (-not (Test-ByteSequence $ahciBytes $requiredSequence)) {
            throw "Freestanding AHCI feature image is missing a required DMA command-layout value"
        }
    }

    $nvmeEfi = Join-Path $temp "NVME.EFI"
    $nvmeOutput = & $Engine --target uefi-x86_64 --out $nvmeEfi $NvmeSource 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Freestanding NVMe feature compile failed:`n$($nvmeOutput -join "`n")"
    }
    $nvmeBytes = [System.IO.File]::ReadAllBytes($nvmeEfi)
    if ($nvmeBytes.Length -lt 88000 -or
        $nvmeBytes[0] -ne 0x4d -or $nvmeBytes[1] -ne 0x5a) {
        throw "Freestanding NVMe feature image is invalid"
    }
    $nvmeUtf16 = [System.Text.Encoding]::Unicode.GetString($nvmeBytes)
    if ($nvmeUtf16 -notmatch "Sura NVMe feature test") {
        throw "Freestanding NVMe feature image is missing its diagnostic"
    }
    foreach ($requiredSequence in @(
        ([byte[]](0x00, 0x08, 0x50, 0x00)),
        ([byte[]](0x00, 0x10, 0x50, 0x00)),
        ([byte[]](0x01, 0x00, 0x46, 0x00))
    )) {
        if (-not (Test-ByteSequence $nvmeBytes $requiredSequence)) {
            throw "Freestanding NVMe feature image is missing a required queue or PRP value"
        }
    }

    $invalidSource = Join-Path $temp "invalid_interrupt_abi.sura"
    $invalidText = @'
idt is static.zero(4096, 16)

func wrong_page_fault(frame: ptr) interrupt do
  return
end

func efi_main(image: u64, system: ptr) -> u64 do
  cpu.idt_set_gate(idt, 14, addr_of(wrong_page_fault), 8, 0, 142)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidSource,
        $invalidText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidSource `
        -OutputPath (Join-Path $temp "INVALID.EFI") `
        -Pattern "pushes an error code" `
        -Description "Mismatched interrupt error-code ABI"

    $invalidTssSource = Join-Path $temp "invalid_tss_bounds.sura"
    $invalidTssText = @'
gdt is static.zero(16, 16)
tss is static.zero(104, 16)

func efi_main(image: u64, system: ptr) -> u64 do
  cpu.gdt_set_tss(gdt, 1, tss, 103)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidTssSource,
        $invalidTssText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidTssSource `
        -OutputPath (Join-Path $temp "INVALID_TSS.EFI") `
        -Pattern "exceeds static GDT" `
        -Description "Out-of-bounds static TSS descriptor"

    $invalidSipiSource = Join-Path $temp "invalid_sipi_address.sura"
    $invalidSipiText = @'
func efi_main(image: u64, system: ptr) -> u64 do
  apic.send_startup(1, 12345)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidSipiSource,
        $invalidSipiText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidSipiSource `
        -OutputPath (Join-Path $temp "INVALID_SIPI.EFI") `
        -Pattern "4 KiB aligned" `
        -Description "Misaligned constant SIPI trampoline address"

    $invalidPagingIndexSource = Join-Path $temp "invalid_paging_index.sura"
    $invalidPagingIndexText = @'
page_table is static.zero(4096, 4096)

func efi_main(image: u64, system: ptr) -> u64 do
  value: u64 is paging.read(page_table, 512)
  return value
end
'@
    [System.IO.File]::WriteAllText(
        $invalidPagingIndexSource,
        $invalidPagingIndexText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidPagingIndexSource `
        -OutputPath (Join-Path $temp "INVALID_PAGING_INDEX.EFI") `
        -Pattern "page-table index must be 0\.\.511" `
        -Description "Out-of-range constant page-table index"

    $invalidPagingAddressSource = Join-Path $temp "invalid_paging_address.sura"
    $invalidPagingAddressText = @'
func efi_main(image: u64, system: ptr) -> u64 do
  entry: u64 is paging.entry(123, 3)
  return entry
end
'@
    [System.IO.File]::WriteAllText(
        $invalidPagingAddressSource,
        $invalidPagingAddressText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidPagingAddressSource `
        -OutputPath (Join-Path $temp "INVALID_PAGING_ADDRESS.EFI") `
        -Pattern "physical address must be 4 KiB aligned" `
        -Description "Unaligned constant page-table address"

    $invalidContextStackSource = Join-Path $temp "invalid_context_stack.sura"
    $invalidContextStackText = @'
func task(argument: u64) -> u64 do
  return argument
end

func task_exit(result: u64) do
  return
end

func efi_main(image: u64, system: ptr) -> u64 do
  initial_rsp: ptr is context.init(64, addr_of(task), 0, addr_of(task_exit))
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidContextStackSource,
        $invalidContextStackText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidContextStackSource `
        -OutputPath (Join-Path $temp "INVALID_CONTEXT_STACK.EFI") `
        -Pattern "at least 72 bytes" `
        -Description "Too-small constant task context stack"

    $invalidSyscallVectorSource = Join-Path $temp "invalid_syscall_vector.sura"
    $invalidSyscallVectorText = @'
func efi_main(image: u64, system: ptr) -> u64 do
  result: u64 is syscall.invoke(31, 0)
  return result
end
'@
    [System.IO.File]::WriteAllText(
        $invalidSyscallVectorSource,
        $invalidSyscallVectorText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidSyscallVectorSource `
        -OutputPath (Join-Path $temp "INVALID_SYSCALL_VECTOR.EFI") `
        -Pattern "system-call vector must be 32\.\.255" `
        -Description "Reserved system-call vector"

    $invalidFastMaskSource = Join-Path $temp "invalid_fast_syscall_mask.sura"
    $invalidFastMaskText = @'
func dispatch(frame: ptr) do
  return
end

func bad_return(frame: ptr) do
  return
end

func efi_main(image: u64, system: ptr) -> u64 do
  syscall.fast_configure(addr_of(dispatch), addr_of(bad_return), 8, 35, 0, 0, 8)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidFastMaskSource,
        $invalidFastMaskText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidFastMaskSource `
        -OutputPath (Join-Path $temp "INVALID_FAST_MASK.EFI") `
        -Pattern "must include TF, IF, DF, IOPL, NT, and AC" `
        -Description "Unsafe fast-syscall flags mask"

    $invalidUserSelectorSource = Join-Path $temp "invalid_user_selector.sura"
    $invalidUserSelectorText = @'
func efi_main(image: u64, system: ptr) -> u64 do
  entered: bool is user.enter(4096, 4104, 0, 32, 27)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidUserSelectorSource,
        $invalidUserSelectorText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidUserSelectorSource `
        -OutputPath (Join-Path $temp "INVALID_USER_SELECTOR.EFI") `
        -Pattern "user code selector must be a nonzero 16-bit selector with RPL 3" `
        -Description "Ring-0 selector used for user entry"

    $invalidPreemptSelectorSource = Join-Path $temp "invalid_preempt_selector.sura"
    $invalidPreemptSelectorText = @'
stack is static.zero(4096, 16)

func task(argument: u64) -> u64 do
  return argument
end

func task_exit(result: u64) do
  return
end

func efi_main(image: u64, system: ptr) -> u64 do
  frame: ptr is preempt.init(ptr.add(stack, 4096), addr_of(task), 0, addr_of(task_exit), 11)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidPreemptSelectorSource,
        $invalidPreemptSelectorText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidPreemptSelectorSource `
        -OutputPath (Join-Path $temp "INVALID_PREEMPT_SELECTOR.EFI") `
        -Pattern "preemptive task code selector must be a nonzero 16-bit selector with RPL 0" `
        -Description "Ring-3 selector used for kernel preemption"

    $invalidPreemptResumeSource = Join-Path $temp "invalid_preempt_resume.sura"
    $invalidPreemptResumeText = @'
func efi_main(image: u64, system: ptr) -> u64 do
  resumed: bool is preempt.resume(4096)
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidPreemptResumeSource,
        $invalidPreemptResumeText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidPreemptResumeSource `
        -OutputPath (Join-Path $temp "INVALID_PREEMPT_RESUME.EFI") `
        -Pattern "only available inside an interrupt or interrupt_error function" `
        -Description "Preemptive frame resume outside an interrupt"

    $invalidReservedFunctionSource = Join-Path $temp "invalid_reserved_function.sura"
    $invalidReservedFunctionText = @'
func __sura_context_switch() do
  return
end

func efi_main(image: u64, system: ptr) -> u64 do
  return 0
end
'@
    [System.IO.File]::WriteAllText(
        $invalidReservedFunctionSource,
        $invalidReservedFunctionText,
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $invalidReservedFunctionSource `
        -OutputPath (Join-Path $temp "INVALID_RESERVED_FUNCTION.EFI") `
        -Pattern "are reserved" `
        -Description "Reserved freestanding helper name"

    $cycleASource = Join-Path $temp "cycle_a.sura"
    $cycleBSource = Join-Path $temp "cycle_b.sura"
    [System.IO.File]::WriteAllText(
        $cycleASource,
        "import `"cycle_b.sura`"`nfunc efi_main() -> u64 do`n  return 0`nend`n",
        (New-Object System.Text.UTF8Encoding($false))
    )
    [System.IO.File]::WriteAllText(
        $cycleBSource,
        "import `"cycle_a.sura`"`n",
        (New-Object System.Text.UTF8Encoding($false))
    )
    Invoke-ExpectedCompileFailure `
        -EnginePath $Engine `
        -SourcePath $cycleASource `
        -OutputPath (Join-Path $temp "INVALID_CYCLE.EFI") `
        -Pattern "circular freestanding import" `
        -Description "Circular freestanding import"

    "sura_uefi_target_smoke: PASS (hello=$($bytes.Length), features=$($featureBytes.Length), memory=$($memoryBytes.Length), scheduler=$($schedulerBytes.Length), preempt=$($preemptiveBytes.Length), syscall=$($syscallBytes.Length), user=$($userModeBytes.Length), pci=$($pciBytes.Length), acpi=$($acpiBytes.Length), ap=$($apStartupBytes.Length), block=$($blockBytes.Length), fat32=$($fat32Bytes.Length), vfs=$($vfsBytes.Length), gpt=$($gptBytes.Length), partition=$($partitionBytes.Length), ahci=$($ahciBytes.Length), nvme=$($nvmeBytes.Length) bytes)"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        $resolved = [System.IO.Path]::GetFullPath($temp)
        $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolved).StartsWith("sura_uefi_")) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
