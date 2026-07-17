param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/hello_uefi.sura"),
    [string]$FeatureSource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/freestanding_features.sura"),
    [string]$MemorySource = (Join-Path (Split-Path -Parent $PSScriptRoot) "examples/os/memory_kernel.sura")
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

    "sura_uefi_target_smoke: PASS (hello=$($bytes.Length), features=$($featureBytes.Length), memory=$($memoryBytes.Length) bytes)"
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
