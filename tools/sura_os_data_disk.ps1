param(
    [string]$Path = (Join-Path (Split-Path -Parent $PSScriptRoot) "build/os/SuraData.img"),
    [switch]$Force,
    [switch]$VerifyOnly
)

$ErrorActionPreference = "Stop"

$sectorSize = 512
$legacyTotalSectors = 131072
$totalSectors = 262144
$partitionFirstLba = 2048
$partitionSectors = $legacyTotalSectors - $partitionFirstLba
$suraFsFirstLba = $legacyTotalSectors
$suraFsSectors = $totalSectors - $suraFsFirstLba
$reservedSectors = 32
$fatCount = 2
$fatSectors = 1000
$dataFirstLba = $partitionFirstLba + $reservedSectors + ($fatCount * $fatSectors)
$expectedLength = [int64]$totalSectors * $sectorSize
$legacyLength = [int64]$legacyTotalSectors * $sectorSize

function Set-U16 {
    param([byte[]]$Buffer, [int]$Offset, [uint16]$Value)
    $Buffer[$Offset] = [byte]($Value -band 0xff)
    $Buffer[$Offset + 1] = [byte](($Value -shr 8) -band 0xff)
}

function Set-U32 {
    param([byte[]]$Buffer, [int]$Offset, [uint32]$Value)
    for ($index = 0; $index -lt 4; $index++) {
        $Buffer[$Offset + $index] = [byte](($Value -shr ($index * 8)) -band 0xff)
    }
}

function Set-U64 {
    param([byte[]]$Buffer, [int]$Offset, [uint64]$Value)
    for ($index = 0; $index -lt 8; $index++) {
        $Buffer[$Offset + $index] = [byte](($Value -shr ($index * 8)) -band 0xff)
    }
}

function Get-U16 {
    param([byte[]]$Buffer, [int]$Offset)
    return [uint16](
        [uint16]$Buffer[$Offset] -bor
        ([uint16]$Buffer[$Offset + 1] -shl 8)
    )
}

function Get-U32 {
    param([byte[]]$Buffer, [int]$Offset)
    return [uint32](
        [uint32]$Buffer[$Offset] -bor
        ([uint32]$Buffer[$Offset + 1] -shl 8) -bor
        ([uint32]$Buffer[$Offset + 2] -shl 16) -bor
        ([uint32]$Buffer[$Offset + 3] -shl 24)
    )
}

function Set-Ascii {
    param([byte[]]$Buffer, [int]$Offset, [string]$Text)
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Text)
    [Array]::Copy($bytes, 0, $Buffer, $Offset, $bytes.Length)
}

function Write-At {
    param(
        [System.IO.FileStream]$Stream,
        [int64]$Offset,
        [byte[]]$Buffer
    )
    $Stream.Position = $Offset
    $Stream.Write($Buffer, 0, $Buffer.Length)
}

function New-DirectoryEntry {
    param(
        [string]$ShortName,
        [byte]$Attributes,
        [uint32]$Cluster,
        [uint32]$Size
    )
    if ($ShortName.Length -ne 11) {
        throw "FAT32 short name must contain exactly 11 characters: '$ShortName'"
    }
    $entry = New-Object byte[] 32
    Set-Ascii $entry 0 $ShortName
    $entry[11] = $Attributes
    Set-U16 $entry 20 ([uint16]($Cluster -shr 16))
    Set-U16 $entry 26 ([uint16]($Cluster -band 0xffff))
    Set-U32 $entry 28 $Size
    return $entry
}

function Set-DirectoryEntry {
    param(
        [byte[]]$Sector,
        [int]$Index,
        [byte[]]$Entry
    )
    [Array]::Copy($Entry, 0, $Sector, $Index * 32, 32)
}

function Get-ClusterOffset {
    param([uint32]$Cluster)
    return [int64]($dataFirstLba + ($Cluster - 2)) * $sectorSize
}

function Test-SuraDataDisk {
    param([string]$DiskPath)
    if (-not (Test-Path -LiteralPath $DiskPath -PathType Leaf)) {
        throw "Sura OS data disk was not found: $DiskPath"
    }
    $item = Get-Item -LiteralPath $DiskPath
    if ($item.Length -ne $expectedLength -and $item.Length -ne $legacyLength) {
        throw "Sura OS data disk has an unexpected size: $($item.Length)"
    }
    $stream = [System.IO.File]::Open(
        $item.FullName,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite
    )
    try {
        $mbr = New-Object byte[] $sectorSize
        $read = $stream.Read($mbr, 0, $mbr.Length)
        if ($read -ne $sectorSize -or (Get-U16 $mbr 510) -ne 0xaa55) {
            throw "Sura OS data disk has an invalid MBR"
        }
        if ($mbr[450] -ne 0xef -or
            (Get-U32 $mbr 454) -ne $partitionFirstLba -or
            (Get-U32 $mbr 458) -ne $partitionSectors) {
            throw "Sura OS data disk partition entry is invalid"
        }
        if ($item.Length -eq $expectedLength -and
            ($mbr[466] -ne 0x7f -or
             (Get-U32 $mbr 470) -ne $suraFsFirstLba -or
             (Get-U32 $mbr 474) -ne $suraFsSectors)) {
            throw "Sura OS SuraFS partition entry is invalid"
        }

        $boot = New-Object byte[] $sectorSize
        $stream.Position = [int64]$partitionFirstLba * $sectorSize
        $read = $stream.Read($boot, 0, $boot.Length)
        if ($read -ne $sectorSize -or (Get-U16 $boot 510) -ne 0xaa55) {
            throw "Sura OS data disk has an invalid FAT32 boot sector"
        }
        if ((Get-U16 $boot 11) -ne $sectorSize -or
            $boot[13] -ne 1 -or
            (Get-U32 $boot 36) -ne $fatSectors) {
            throw "Sura OS data disk FAT32 geometry is invalid"
        }

        $root = New-Object byte[] $sectorSize
        $stream.Position = Get-ClusterOffset 2
        $read = $stream.Read($root, 0, $root.Length)
        if ($read -ne $sectorSize) {
            throw "Sura OS data disk root directory is truncated"
        }
        $marker = [System.Text.Encoding]::ASCII.GetString($root, 0, 11)
        if ($marker -ne "SURA    DSK") {
            throw "Sura OS data disk marker file is missing"
        }

        $settings = New-Object byte[] $sectorSize
        $stream.Position = Get-ClusterOffset 5
        $read = $stream.Read($settings, 0, $settings.Length)
        $settingsPrefix = [System.Text.Encoding]::ASCII.GetString($settings, 0, 8)
        if ($read -ne $sectorSize -or
            ($settingsPrefix -ne "SURASET1" -and $settingsPrefix -ne "version=")) {
            throw "Sura OS settings record is missing"
        }

        $desktop = New-Object byte[] $sectorSize
        $stream.Position = Get-ClusterOffset 6
        $read = $stream.Read($desktop, 0, $desktop.Length)
        $desktopPrefix = [System.Text.Encoding]::ASCII.GetString($desktop, 0, 8)
        if ($read -ne $sectorSize -or
            ($desktopPrefix -ne "SURADSK1" -and $desktopPrefix -ne "version=")) {
            throw "Sura OS desktop-state record is missing"
        }
    }
    finally {
        $stream.Dispose()
    }
    return $item
}

function Upgrade-SuraDataDisk {
    param([string]$DiskPath)
    $item = Test-SuraDataDisk $DiskPath
    if ($item.Length -ne $legacyLength) {
        return $item
    }
    $stream = [System.IO.File]::Open(
        $item.FullName,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None
    )
    try {
        $stream.SetLength($expectedLength)
        $mbr = New-Object byte[] $sectorSize
        $stream.Position = 0
        if ($stream.Read($mbr, 0, $mbr.Length) -ne $sectorSize) {
            throw "Sura OS data disk MBR could not be read during upgrade"
        }
        $mbr[462] = 0
        $mbr[466] = 0x7f
        Set-U32 $mbr 470 ([uint32]$suraFsFirstLba)
        Set-U32 $mbr 474 ([uint32]$suraFsSectors)
        Write-At $stream 0 $mbr
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
    return Test-SuraDataDisk $DiskPath
}

try {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if ($VerifyOnly) {
        $verified = Test-SuraDataDisk $fullPath
        "sura_os_data_disk: VERIFY PASS (path=$($verified.FullName), bytes=$($verified.Length))"
        return
    }

    if ((Test-Path -LiteralPath $fullPath -PathType Leaf) -and -not $Force) {
        $beforeLength = (Get-Item -LiteralPath $fullPath).Length
        $verified = Upgrade-SuraDataDisk $fullPath
        if ($beforeLength -eq $legacyLength) {
            "sura_os_data_disk: UPGRADE PASS (path=$($verified.FullName), bytes=$($verified.Length), surafs_lba=$suraFsFirstLba)"
        }
        else {
            "sura_os_data_disk: KEEP (path=$($verified.FullName), bytes=$($verified.Length))"
        }
        return
    }

    $directory = Split-Path -Parent $fullPath
    if (-not [string]::IsNullOrWhiteSpace($directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    $stream = [System.IO.File]::Open(
        $fullPath,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None
    )
    try {
        $stream.SetLength($expectedLength)

        $mbr = New-Object byte[] $sectorSize
        $mbr[446] = 0x80
        $mbr[450] = 0xef
        Set-U32 $mbr 454 ([uint32]$partitionFirstLba)
        Set-U32 $mbr 458 ([uint32]$partitionSectors)
        $mbr[466] = 0x7f
        Set-U32 $mbr 470 ([uint32]$suraFsFirstLba)
        Set-U32 $mbr 474 ([uint32]$suraFsSectors)
        Set-U16 $mbr 510 0xaa55
        Write-At $stream 0 $mbr

        $boot = New-Object byte[] $sectorSize
        $boot[0] = 0xeb
        $boot[1] = 0x58
        $boot[2] = 0x90
        Set-Ascii $boot 3 "SURAOS  "
        Set-U16 $boot 11 ([uint16]$sectorSize)
        $boot[13] = 1
        Set-U16 $boot 14 ([uint16]$reservedSectors)
        $boot[16] = [byte]$fatCount
        $boot[21] = 0xf8
        Set-U16 $boot 24 63
        Set-U16 $boot 26 255
        Set-U32 $boot 28 ([uint32]$partitionFirstLba)
        Set-U32 $boot 32 ([uint32]$partitionSectors)
        Set-U32 $boot 36 ([uint32]$fatSectors)
        Set-U32 $boot 44 2
        Set-U16 $boot 48 1
        Set-U16 $boot 50 6
        $boot[64] = 0x80
        $boot[66] = 0x29
        Set-U32 $boot 67 0x53555241
        Set-Ascii $boot 71 "SURA DATA  "
        Set-Ascii $boot 82 "FAT32   "
        Set-U16 $boot 510 0xaa55
        Write-At $stream ([int64]$partitionFirstLba * $sectorSize) $boot
        Write-At $stream ([int64]($partitionFirstLba + 6) * $sectorSize) $boot

        $clusterCount = $partitionSectors - $reservedSectors - ($fatCount * $fatSectors)
        $fsInfo = New-Object byte[] $sectorSize
        Set-U32 $fsInfo 0 0x41615252
        Set-U32 $fsInfo 484 0x61417272
        Set-U32 $fsInfo 488 ([uint32]($clusterCount - 7))
        Set-U32 $fsInfo 492 9
        Set-U32 $fsInfo 508 2857697280
        Write-At $stream ([int64]($partitionFirstLba + 1) * $sectorSize) $fsInfo
        Write-At $stream ([int64]($partitionFirstLba + 7) * $sectorSize) $fsInfo

        for ($copy = 0; $copy -lt $fatCount; $copy++) {
            $fatSector = New-Object byte[] $sectorSize
            Set-U32 $fatSector 0 0x0ffffff8
            Set-U32 $fatSector 4 0x0fffffff
            for ($cluster = 2; $cluster -le 8; $cluster++) {
                Set-U32 $fatSector ($cluster * 4) 0x0fffffff
            }
            $fatLba = $partitionFirstLba + $reservedSectors + ($copy * $fatSectors)
            Write-At $stream ([int64]$fatLba * $sectorSize) $fatSector
        }

        $root = New-Object byte[] $sectorSize
        Set-DirectoryEntry $root 0 (New-DirectoryEntry "SURA    DSK" 0x20 3 512)
        Set-DirectoryEntry $root 1 (New-DirectoryEntry "NOTES   TXT" 0x20 4 512)
        Set-DirectoryEntry $root 2 (New-DirectoryEntry "SETTINGSCFG" 0x20 5 512)
        Set-DirectoryEntry $root 3 (New-DirectoryEntry "DESKTOP CFG" 0x20 6 512)
        Set-DirectoryEntry $root 4 (New-DirectoryEntry "DOCS       " 0x10 7 0)
        Write-At $stream (Get-ClusterOffset 2) $root

        $marker = New-Object byte[] $sectorSize
        Set-Ascii $marker 0 "SURA DATA DISK V1`r`n"
        Write-At $stream (Get-ClusterOffset 3) $marker

        $notes = New-Object byte[] $sectorSize
        Set-Ascii $notes 0 "Welcome to Sura OS Notes."
        Write-At $stream (Get-ClusterOffset 4) $notes

        $settings = New-Object byte[] $sectorSize
        Set-Ascii $settings 0 "SURASET1"
        Set-U64 $settings 8 1
        Set-U64 $settings 16 0
        Set-U64 $settings 24 1
        Write-At $stream (Get-ClusterOffset 5) $settings

        $desktop = New-Object byte[] $sectorSize
        Set-Ascii $desktop 0 "SURADSK1"
        Set-U64 $desktop 8 2
        Set-U64 $desktop 16 1
        Set-U64 $desktop 24 8
        $desktopDefaults = @(
            @{ Id = 1; X = 320; Y = 209; Z = 7; Visible = 1 },
            @{ Id = 2; X = 840; Y = 96;  Z = 2; Visible = 1 },
            @{ Id = 3; X = 100; Y = 130; Z = 4; Visible = 0 },
            @{ Id = 4; X = 220; Y = 170; Z = 5; Visible = 0 },
            @{ Id = 5; X = 860; Y = 220; Z = 6; Visible = 0 },
            @{ Id = 6; X = 280; Y = 88;  Z = 1; Visible = 0 }
        )
        for ($index = 0; $index -lt $desktopDefaults.Count; $index++) {
            $record = $desktopDefaults[$index]
            $offset = 32 + ($index * 32)
            Set-U64 $desktop $offset ([uint64]$record.Id)
            Set-U32 $desktop ($offset + 8) ([uint32]$record.X)
            Set-U32 $desktop ($offset + 12) ([uint32]$record.Y)
            Set-U64 $desktop ($offset + 16) ([uint64]$record.Z)
            Set-U64 $desktop ($offset + 24) ([uint64]$record.Visible)
        }
        Write-At $stream (Get-ClusterOffset 6) $desktop

        $docs = New-Object byte[] $sectorSize
        Set-DirectoryEntry $docs 0 (New-DirectoryEntry ".          " 0x10 7 0)
        Set-DirectoryEntry $docs 1 (New-DirectoryEntry "..         " 0x10 2 0)
        Set-DirectoryEntry $docs 2 (New-DirectoryEntry "README  TXT" 0x20 8 512)
        Write-At $stream (Get-ClusterOffset 7) $docs

        $readme = New-Object byte[] $sectorSize
        Set-Ascii $readme 0 "Sura OS persistent data volume."
        Write-At $stream (Get-ClusterOffset 8) $readme
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }

    $created = Test-SuraDataDisk $fullPath
    "sura_os_data_disk: CREATE PASS (path=$($created.FullName), bytes=$($created.Length))"
}
catch {
    Write-Error $_
    exit 1
}
