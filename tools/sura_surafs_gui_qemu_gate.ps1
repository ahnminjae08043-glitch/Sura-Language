param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dataDiskTool = Join-Path $PSScriptRoot "sura_os_data_disk.ps1"
$screenshotGate = Join-Path $PSScriptRoot "sura_os_screenshot.ps1"
$bootGate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"
$source = Join-Path $root "os/sura_os.sura"
$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$temp = Join-Path $tempRoot ("sura_surafs_gui_" + [guid]::NewGuid().ToString("N"))
$baseDisk = Join-Path $temp "base.img"
$persistedDisk = Join-Path $temp "persisted.img"
$suraFsOffset = [int64]131072 * 512
$suraFsBytes = [int64]131072 * 512
$expectedPrefix = [System.Text.Encoding]::ASCII.GetBytes(
    "Welcome to Sura OS Notes.sura notes"
)
# UTF-8 for "한글 " as numeric bytes keeps Windows PowerShell 5.1 independent
# from the script source file's encoding.
[byte[]]$expectedSuffix = @(10, 237, 149, 156, 234, 184, 128, 32)
$expectedNotes = New-Object byte[] ($expectedPrefix.Length + $expectedSuffix.Length)
[Array]::Copy($expectedPrefix, 0, $expectedNotes, 0, $expectedPrefix.Length)
[Array]::Copy(
    $expectedSuffix,
    0,
    $expectedNotes,
    $expectedPrefix.Length,
    $expectedSuffix.Length
)
$expectedNotesHex = [BitConverter]::ToString($expectedNotes)
$expectedCode = [System.Text.Encoding]::UTF8.GetBytes(
    "message is `"Hello from Sura`"`ncount is 3 # demo`nprint(message)`n"
)
$expectedCodeHex = [BitConverter]::ToString($expectedCode)
[uint64]$expectedChecksum = 0
for ($index = 0; $index -lt $expectedNotes.Length; $index++) {
    $expectedChecksum += [uint64](($index + 1) * [int]$expectedNotes[$index])
}
$expectedRestoreMarker =
    "SURA_OS_SURAFS_EDITOR_RESTORED bytes=$($expectedNotes.Length) checksum=$expectedChecksum"

function Get-RangeHash {
    param(
        [string]$Path,
        [int64]$Offset,
        [int64]$Count
    )
    $stream = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Path).Path)
    try {
        $stream.Position = $Offset
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $buffer = New-Object byte[] 1048576
            $remaining = $Count
            while ($remaining -gt 0) {
                $take = [int][Math]::Min($buffer.Length, $remaining)
                $read = $stream.Read($buffer, 0, $take)
                if ($read -ne $take) { throw "SuraFS range is truncated" }
                [void]$sha.TransformBlock($buffer, 0, $read, $buffer, 0)
                $remaining -= $read
            }
            [void]$sha.TransformFinalBlock((New-Object byte[] 0), 0, 0)
            return ([BitConverter]::ToString($sha.Hash) -replace "-", "")
        }
        finally {
            $sha.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-SuraFsSnapshot {
    param([string]$Path)

    $stream = [System.IO.File]::OpenRead((Resolve-Path -LiteralPath $Path).Path)
    try {
        $mbr = New-Object byte[] 512
        if ($stream.Read($mbr, 0, $mbr.Length) -ne $mbr.Length) {
            throw "SuraFS GUI disk has a truncated MBR"
        }
        if ($mbr[466] -ne 0x7f -or
            [BitConverter]::ToUInt32($mbr, 470) -ne 131072 -or
            [BitConverter]::ToUInt32($mbr, 474) -ne 131072) {
            throw "SuraFS GUI disk is missing its type 0x7f partition"
        }

        $super = New-Object byte[] 1024
        $stream.Position = $suraFsOffset
        if ($stream.Read($super, 0, $super.Length) -ne $super.Length) {
            throw "SuraFS superblocks are truncated"
        }
        $selectedOffset = -1
        $selectedGeneration = [uint64]0
        foreach ($offset in @(0, 512)) {
            if ([System.Text.Encoding]::ASCII.GetString($super, $offset, 7) -ne "SURAFS1") {
                continue
            }
            $generation = [BitConverter]::ToUInt64($super, $offset + 16)
            if ($generation -gt $selectedGeneration) {
                $selectedGeneration = $generation
                $selectedOffset = $offset
            }
        }
        if ($selectedOffset -lt 0 -or $selectedGeneration -eq 0) {
            throw "SuraFS has no published generation"
        }

        $bank = [BitConverter]::ToUInt64($super, $selectedOffset + 24)
        $bankSectors = [BitConverter]::ToUInt64($super, $selectedOffset + 32)
        if ($bank -gt 1 -or $bankSectors -eq 0 -or $bankSectors -gt 65536) {
            throw "SuraFS superblock bank geometry is invalid"
        }
        $bankBytes = [int64]$bankSectors * 512
        $bankBuffer = New-Object byte[] ([int]$bankBytes)
        $bankLba = [int64]131072 + 2 + ([int64]$bank * [int64]$bankSectors)
        $stream.Position = $bankLba * 512
        if ($stream.Read($bankBuffer, 0, $bankBuffer.Length) -ne $bankBuffer.Length) {
            throw "SuraFS active bank is truncated"
        }
        if ([System.Text.Encoding]::ASCII.GetString($bankBuffer, 0, 8) -ne "SURABANK") {
            throw "SuraFS active bank magic is invalid"
        }
        if ([BitConverter]::ToUInt64($bankBuffer, 16) -ne $selectedGeneration) {
            throw "SuraFS bank generation does not match its superblock"
        }

        $nodeCapacity = [BitConverter]::ToUInt64($bankBuffer, 24)
        $nameStride = [BitConverter]::ToUInt64($bankBuffer, 32)
        $dataStride = [BitConverter]::ToUInt64($bankBuffer, 40)
        if ($nodeCapacity -ne 32 -or $nameStride -ne 64 -or $dataStride -ne 4096) {
            throw "SuraFS GUI layout does not match the mounted format"
        }
        $nodeBytes = 88
        $namesOffset = [int]((512 + $nodeCapacity * $nodeBytes + 7) -band -8)
        $dataOffset = [int](($namesOffset + $nodeCapacity * $nameStride + 511) -band -512)
        $documentsIndex = -1
        $trashIndex = -1
        $notesIndex = -1
        $mainIndex = -1
        $mainParent = -1
        $mainHex = ""
        $renamedIndex = -1
        $renamedParent = -1
        $tempIndex = -1
        $tempParent = -1
        $koreanFileIndex = -1
        $koreanFileParent = -1
        $copyIndex = -1
        $copyParent = -1
        $copyHex = ""
        $moveboxIndex = -1
        $moveboxParent = -1
        $moveboxCopyIndex = -1
        $moveboxCopyParent = -1
        $movedCopyParents = @()
        $movedCopyHexes = @()
        $extraParents = @{}
        $agentSeen = $false
        $notesHex = ""
        $documentsName = ([string][char]0xbb38) + ([string][char]0xc11c)
        $trashName = ([string][char]0xd734) + ([string][char]0xc9c0) + ([string][char]0xd1b5)
        $notesName = ([string][char]0xba54) + ([string][char]0xbaa8) + ".txt"
        $koreanFileName = ([string][char]0xd55c) + ([string][char]0xae00) + ".sura"
        for ($index = 1; $index -lt $nodeCapacity; $index++) {
            $nodeOffset = 512 + $index * $nodeBytes
            if ([BitConverter]::ToUInt64($bankBuffer, $nodeOffset + 16) -eq 0) {
                continue
            }
            $kind = [BitConverter]::ToUInt64($bankBuffer, $nodeOffset + 24)
            $parent = [BitConverter]::ToUInt64($bankBuffer, $nodeOffset + 32)
            $nameLength = [BitConverter]::ToUInt64($bankBuffer, $nodeOffset + 40)
            $size = [BitConverter]::ToUInt64($bankBuffer, $nodeOffset + 48)
            if ($nameLength -gt $nameStride -or $size -gt $dataStride) {
                throw "SuraFS GUI node exceeds its fixed slot"
            }
            $name = [System.Text.Encoding]::UTF8.GetString(
                $bankBuffer,
                [int]($namesOffset + $index * $nameStride),
                [int]$nameLength
            )
            if ($kind -eq 2 -and $parent -eq 0 -and $name -eq $documentsName) {
                $documentsIndex = $index
            }
            if ($kind -eq 2 -and $parent -eq 0 -and $name -eq $trashName) {
                $trashIndex = $index
            }
            if ($name -eq "agent.sura") {
                $agentSeen = $true
            }
            if ($kind -eq 1 -and $name -eq "renamed.sura") {
                $renamedIndex = $index
                $renamedParent = $parent
            }
            if ($kind -eq 2 -and $name -eq "temp") {
                $tempIndex = $index
                $tempParent = $parent
            }
            if ($kind -eq 1 -and $name -eq $koreanFileName) {
                $koreanFileIndex = $index
                $koreanFileParent = $parent
            }
            if ($kind -eq 1 -and $name -eq "copy.sura") {
                $copyIndex = $index
                $copyParent = $parent
                $copyBytes = New-Object byte[] ([int]$size)
                [Array]::Copy(
                    $bankBuffer,
                    [int]($dataOffset + $index * $dataStride),
                    $copyBytes,
                    0,
                    [int]$size
                )
                $copyHex = [BitConverter]::ToString($copyBytes)
            }
            if ($kind -eq 1 -and $name -eq "main.sura") {
                $mainIndex = $index
                $mainParent = $parent
                $mainBytes = New-Object byte[] ([int]$size)
                [Array]::Copy(
                    $bankBuffer,
                    [int]($dataOffset + $index * $dataStride),
                    $mainBytes,
                    0,
                    [int]$size
                )
                $mainHex = [BitConverter]::ToString($mainBytes)
            }
            if ($kind -eq 2 -and $name -eq "movebox") {
                $moveboxIndex = $index
                $moveboxParent = $parent
            }
            if ($kind -eq 2 -and $name -eq "movebox - Copy") {
                $moveboxCopyIndex = $index
                $moveboxCopyParent = $parent
            }
            if ($kind -eq 1 -and $name -eq ($notesName + " - Copy")) {
                $movedCopyParents += $parent
                $movedCopyBytes = New-Object byte[] ([int]$size)
                [Array]::Copy(
                    $bankBuffer,
                    [int]($dataOffset + $index * $dataStride),
                    $movedCopyBytes,
                    0,
                    [int]$size
                )
                $movedCopyHexes += [BitConverter]::ToString($movedCopyBytes)
            }
            if ($kind -eq 1 -and
                ($name -eq "extra1" -or $name -eq "extra2" -or $name -eq "extra3")) {
                $extraParents[$name] = $parent
            }
            if ($kind -eq 1 -and $name -eq $notesName) {
                $notesIndex = $index
                $notesBytes = New-Object byte[] ([int]$size)
                [Array]::Copy(
                    $bankBuffer,
                    [int]($dataOffset + $index * $dataStride),
                    $notesBytes,
                    0,
                    [int]$size
                )
                $notesHex = [BitConverter]::ToString($notesBytes)
                $notesParent = $parent
            }
        }
        if ($documentsIndex -lt 0 -or $notesIndex -lt 0 -or $notesParent -ne $documentsIndex) {
            throw "SuraFS GUI UTF-8 document tree is missing"
        }
        if ($trashIndex -lt 0 -or $tempIndex -lt 0 -or $tempParent -ne $trashIndex) {
            throw "SuraFS GUI Explorer did not move the created directory into the recycle bin"
        }
        if ($agentSeen -or $renamedIndex -lt 0 -or $renamedParent -ne $documentsIndex) {
            throw "SuraFS GUI Explorer did not persist the created file under its renamed path"
        }
        if ($koreanFileIndex -lt 0 -or $koreanFileParent -ne $documentsIndex) {
            throw "SuraFS GUI Explorer did not persist the Korean filename created through the input method"
        }
        if ($copyIndex -lt 0 -or $copyParent -ne $documentsIndex -or $copyHex -ne $expectedNotesHex) {
            throw "SuraFS GUI Text Editor Save As did not persist an exact document copy (copy=$copyIndex, parent=$copyParent, documents=$documentsIndex, bytes=$(([Math]::Max(0, $copyHex.Length) + 2) / 3), expected=$($expectedNotes.Length))"
        }
        if ($mainIndex -lt 0 -or $mainParent -ne $documentsIndex -or $mainHex -ne $expectedCodeHex) {
            throw "SuraFS GUI seed did not preserve the exact 62-byte main.sura starter"
        }
        if ($moveboxIndex -lt 0 -or $moveboxParent -ne $documentsIndex -or
            $moveboxCopyIndex -lt 0 -or $moveboxCopyParent -ne $documentsIndex) {
            throw "SuraFS GUI Explorer did not persist the source and recursively copied movebox directories"
        }
        if ($movedCopyParents.Count -ne 2 -or
            -not $movedCopyParents.Contains([uint64]$moveboxIndex) -or
            -not $movedCopyParents.Contains([uint64]$moveboxCopyIndex)) {
            throw "SuraFS GUI Explorer did not move the cut file and recursively duplicate it with its folder (movebox=$moveboxIndex, duplicate=$moveboxCopyIndex, childParents=$($movedCopyParents -join ','))"
        }
        if ($movedCopyHexes.Count -ne 2 -or
            @($movedCopyHexes | Where-Object { $_ -ne $expectedNotesHex }).Count -ne 0) {
            throw "SuraFS GUI Explorer file/tree copies did not preserve the exact 43-byte UTF-8 document"
        }
        foreach ($extraName in @("extra1", "extra2", "extra3")) {
            if (-not $extraParents.ContainsKey($extraName) -or
                $extraParents[$extraName] -ne $documentsIndex) {
                throw "SuraFS GUI Explorer did not persist the entries used by the scrolling gate: $extraName"
            }
        }
        if ($notesHex -ne $expectedNotesHex) {
            throw "SuraFS GUI editor did not commit the exact ASCII and Korean UTF-8 payload"
        }
        return [pscustomobject]@{
            Generation = $selectedGeneration
            Bank = $bank
            NotesHex = $notesHex
            MutationShape = "$documentsIndex/$trashIndex/$renamedIndex/$tempIndex/$koreanFileIndex/$copyIndex/$moveboxIndex/$moveboxCopyIndex/$($movedCopyParents -join ',')/$($extraParents['extra1'])/$($extraParents['extra2'])/$($extraParents['extra3'])"
        }
    }
    finally {
        $stream.Dispose()
    }
}

try {
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    & $dataDiskTool -Path $baseDisk -Force
    if (-not $?) { throw "Fresh SuraFS GUI data disk creation failed" }

    & $screenshotGate `
        -Engine $Engine `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -DataDisk $baseDisk `
        -DataDiskOutput $persistedDisk `
        -Output (Join-Path $temp "desktop.ppm") `
        -WindowOutput (Join-Path $temp "windows.ppm") `
        -StartOutput (Join-Path $temp "start.ppm") `
        -AppsOutput (Join-Path $temp "apps.ppm") `
        -KoreanOutput (Join-Path $temp "korean.ppm") `
        -BrowserOutput (Join-Path $temp "browser.ppm") `
        -BrowserKoreanOutput (Join-Path $temp "browser-korean.ppm") `
        -SurafsVerificationOnly `
        -TimeoutSeconds $TimeoutSeconds
    if (-not $?) { throw "SuraFS graphical editor/explorer boot failed" }

    $beforeSnapshot = Get-SuraFsSnapshot $persistedDisk
    $beforeHash = Get-RangeHash $persistedDisk $suraFsOffset $suraFsBytes

    & $bootGate `
        -Engine $Engine `
        -Source $source `
        -Qemu $Qemu `
        -Firmware $Firmware `
        -DataDisk $persistedDisk `
        -PersistDataDisk `
        -EnableNetwork `
        -TimeoutSeconds $TimeoutSeconds `
        -ExpectedEfiText "Sura OS virtual machine" `
        -ExpectedMarker "SURA_OS_SHUTDOWN" `
        -ExpectedExitCode 0 `
        -SerialInputLines @("shutdown") `
        -SerialInputDelayMilliseconds 8000 `
        -AdditionalExpectedSerialMarkers @(
            "SURA_OS_ACPI_POWER_READY",
            "SURA_OS_ACPI_POWER_OFF_ARMED",
            "SURA_OS_STORAGE_READY",
            "SURA_OS_SURAFS_READY",
            $expectedRestoreMarker
        )
    if (-not $?) { throw "SuraFS graphical persistence reboot failed" }

    $afterSnapshot = Get-SuraFsSnapshot $persistedDisk
    $afterHash = Get-RangeHash $persistedDisk $suraFsOffset $suraFsBytes
    if ($beforeHash -ne $afterHash -or
        $beforeSnapshot.Generation -ne $afterSnapshot.Generation -or
        $beforeSnapshot.NotesHex -ne $afterSnapshot.NotesHex -or
        $beforeSnapshot.MutationShape -ne $afterSnapshot.MutationShape) {
        throw ("SuraFS graphical content changed during a remount-only reboot " +
            "(before generation=$($beforeSnapshot.Generation), bank=$($beforeSnapshot.Bank), hash=$beforeHash, shape=$($beforeSnapshot.MutationShape); " +
            "after generation=$($afterSnapshot.Generation), bank=$($afterSnapshot.Bank), hash=$afterHash, shape=$($afterSnapshot.MutationShape))")
    }

    "sura_surafs_gui_qemu_gate: PASS (bytes=$($expectedNotes.Length), checksum=$expectedChecksum, generation=$($afterSnapshot.Generation), bank=$($afterSnapshot.Bank), sha256=$afterHash)"
}
finally {
    if (Test-Path -LiteralPath $temp -PathType Container) {
        $resolved = [System.IO.Path]::GetFullPath($temp)
        if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolved).StartsWith("sura_surafs_gui_")) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
