param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Version = "",
    [string]$OutDir = "",
    [switch]$RunSmoke
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Resolve-ExistingFile {
    param([string]$Path, [string]$Label)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-ChildPath {
    param([string]$Parent, [string]$Child)

    $parentFull = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $childFull = [System.IO.Path]::GetFullPath($Child)
    if (-not $childFull.StartsWith($parentFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "path escaped the expected directory: $childFull"
    }
    return $childFull
}

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
if ([string]::IsNullOrWhiteSpace($Version)) {
    $versionContract = [System.IO.File]::ReadAllText((Join-Path $root "version.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($versionContract.schema -ne "sura.version.v1" -or [string]$versionContract.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        throw "version.json does not satisfy sura.version.v1"
    }
    $Version = ([string]$versionContract.version) + ".0"
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "MSIX version must use Major.Minor.Build.Revision format: $Version"
}
$versionParts = $Version -split '\.'
if ([int]$versionParts[3] -ne 0) {
    throw "Microsoft Store packages require a zero revision number: $Version"
}

$engine = Resolve-ExistingFile (Join-Path $root "SuraLanguage.exe") "Sura Language engine"
$surapkg = Resolve-ExistingFile (Join-Path $root "surapkg.exe") "Sura package manager"
$logo = Resolve-ExistingFile (Join-Path $root "assets/sura-logo.png") "Sura logo"
$manifestTemplate = Resolve-ExistingFile (Join-Path $root "deploy/store-msix/Package.appxmanifest") "Store manifest template"
$stdlib = Join-Path $root "stdlib"
$examples = Join-Path $root "examples"
if (-not (Test-Path -LiteralPath $stdlib -PathType Container)) {
    throw "Sura standard library was not found: $stdlib"
}
if (-not (Test-Path -LiteralPath $examples -PathType Container)) {
    throw "Sura example gallery was not found: $examples"
}

$winapp = Get-Command winapp -ErrorAction SilentlyContinue
if (-not $winapp) {
    throw "Microsoft WinApp CLI was not found. Install it with: winget install Microsoft.WinAppCli"
}

$engineVersionText = (& $engine --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $engineVersionText -notmatch '^Sura Language ([0-9]+\.[0-9]+\.[0-9]+)$') {
    throw "could not read the Sura Language engine version: $engineVersionText"
}
$engineVersion = $Matches[1]
$packageSemVer = ($versionParts[0..2] -join '.')
if ($engineVersion -ne $packageSemVer) {
    throw "MSIX version $Version does not match engine version $engineVersion"
}

$buildBase = Join-Path $root "build/store-msix"
$layout = Assert-ChildPath $buildBase (Join-Path $buildBase "layout")
$looseLayout = Assert-ChildPath $buildBase (Join-Path $buildBase "AppX")
$assetStage = Assert-ChildPath $buildBase (Join-Path $buildBase "Assets")
foreach ($generatedDir in @($layout, $looseLayout, $assetStage)) {
    if (Test-Path -LiteralPath $generatedDir) {
        Remove-Item -LiteralPath $generatedDir -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $layout | Out-Null

Copy-Item -LiteralPath $engine -Destination (Join-Path $layout "SuraLanguage.exe") -Force
Copy-Item -LiteralPath $surapkg -Destination (Join-Path $layout "surapkg.exe") -Force
Copy-Item -LiteralPath $stdlib -Destination (Join-Path $layout "stdlib") -Recurse -Force
Copy-Item -LiteralPath $examples -Destination (Join-Path $layout "examples") -Recurse -Force

$manifest = Assert-ChildPath $buildBase (Join-Path $buildBase "Package.appxmanifest")
if (Test-Path -LiteralPath $manifest) {
    Remove-Item -LiteralPath $manifest -Force
}
$manifestText = [System.IO.File]::ReadAllText($manifestTemplate, [System.Text.Encoding]::UTF8)
if (-not $manifestText.Contains('__VERSION__')) {
    throw "Store manifest template is missing the __VERSION__ placeholder"
}
$manifestText = $manifestText.Replace('__VERSION__', $Version)
[System.IO.File]::WriteAllText($manifest, $manifestText, $utf8NoBom)

& $winapp.Source manifest update-assets $logo --manifest $manifest
if ($LASTEXITCODE -ne 0) {
    throw "WinApp CLI failed to generate MSIX image assets"
}
Copy-Item -LiteralPath $assetStage -Destination (Join-Path $layout "Assets") -Recurse -Force

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $root "dist/store-msix"
}
$outRoot = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$msix = Join-Path $outRoot "SuraLanguage-$Version-x64.msix"
if (Test-Path -LiteralPath $msix) {
    Remove-Item -LiteralPath $msix -Force
}

& $winapp.Source package $layout --manifest $manifest --output $msix --executable "SuraLanguage.exe"
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $msix -PathType Leaf)) {
    throw "WinApp CLI failed to create the Sura Language MSIX package"
}

$smokePassed = $false
$smokeStatus = if ($RunSmoke) { "pending" } else { "not_requested" }
if ($RunSmoke) {
    $developerMode = (Get-ItemProperty -LiteralPath "HKLM:/SOFTWARE/Microsoft/Windows/CurrentVersion/AppModelUnlock" -Name AllowDevelopmentWithoutDevLicense -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
    if ($developerMode -ne 1) {
        $smokeStatus = "skipped_developer_mode_disabled"
        Write-Warning "loose MSIX execution smoke was skipped because Windows Developer Mode is disabled"
    }
    else {
        $registered = $false
        try {
            & $winapp.Source run $layout --manifest $manifest --output-appx-directory $looseLayout --no-launch
            if ($LASTEXITCODE -ne 0) {
                throw "WinApp CLI failed to register the loose MSIX layout"
            }
            $registered = $true

            $aliasRoot = Join-Path $env:LOCALAPPDATA "Microsoft/WindowsApps"
            $suraAlias = Join-Path $aliasRoot "sura.exe"
            $surapkgAlias = Join-Path $aliasRoot "surapkg.exe"
            if (-not (Test-Path -LiteralPath $suraAlias -PathType Leaf)) {
                throw "the MSIX sura.exe execution alias was not registered"
            }
            if (-not (Test-Path -LiteralPath $surapkgAlias -PathType Leaf)) {
                throw "the MSIX surapkg.exe execution alias was not registered"
            }

            $suraOutput = (& $suraAlias --version 2>&1 | Out-String).Trim()
            if ($LASTEXITCODE -ne 0 -or $suraOutput -ne "Sura Language $engineVersion") {
                throw "the MSIX sura.exe alias failed: $suraOutput"
            }

            $surapkgOutput = (& $surapkgAlias --help 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0 -or $surapkgOutput -notmatch 'Sura package manager') {
                throw "the MSIX surapkg.exe alias failed"
            }
            Push-Location $env:TEMP
            try {
                $examplesOutput = (& $surapkgAlias examples --json 2>&1 | Out-String)
                if ($LASTEXITCODE -ne 0) {
                    throw "the MSIX surapkg.exe alias could not discover bundled examples: $examplesOutput"
                }
                $examplesReport = $examplesOutput | ConvertFrom-Json
                if ($examplesReport.schema -ne "sura.package.examples.v1" -or [int]$examplesReport.total_count -lt 40) {
                    throw "the MSIX example gallery report was incomplete"
                }
            }
            finally {
                Pop-Location
            }
            $smokePassed = $true
            $smokeStatus = "passed"
        }
        finally {
            if ($registered) {
                & $winapp.Source unregister --manifest $manifest --force | Out-Host
                if ($LASTEXITCODE -ne 0) {
                    Write-Warning "the Sura Language loose package could not be unregistered automatically"
                }
            }
        }
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($msix)
try {
    $entryNames = @($archive.Entries | ForEach-Object { $_.FullName })
    foreach ($requiredEntry in @(
        "AppxManifest.xml",
        "AppxBlockMap.xml",
        "SuraLanguage.exe",
        "surapkg.exe",
        "stdlib/math.sura",
        "stdlib/freestanding/acpi.sura",
        "stdlib/freestanding/ahci.sura",
        "stdlib/freestanding/ap_startup.sura",
        "stdlib/freestanding/block.sura",
        "stdlib/freestanding/fat32.sura",
        "stdlib/freestanding/gpt.sura",
        "stdlib/freestanding/ioapic.sura",
        "stdlib/freestanding/nvme.sura",
        "stdlib/freestanding/partition.sura",
        "stdlib/freestanding/pci.sura",
        "stdlib/freestanding/pcie.sura",
        "stdlib/freestanding/preempt.sura",
        "stdlib/freestanding/scheduler.sura",
        "stdlib/freestanding/serial.sura",
        "stdlib/freestanding/syscall.sura",
        "stdlib/freestanding/timer.sura",
        "stdlib/freestanding/vfs.sura",
        "examples/starter/01_hello.sura",
        "examples/os/acpi_features.sura",
        "examples/os/ahci_features.sura",
        "examples/os/ap_startup_features.sura",
        "examples/os/block_features.sura",
        "examples/os/fat32_features.sura",
        "examples/os/gpt_features.sura",
        "examples/os/ioapic_features.sura",
        "examples/os/nvme_features.sura",
        "examples/os/partition_features.sura",
        "examples/os/pci_features.sura",
        "examples/os/pcie_features.sura",
        "examples/os/preemptive_timer_features.sura",
        "examples/os/qemu_boot_gate.sura",
        "examples/os/scheduler_features.sura",
        "examples/os/syscall_features.sura",
        "examples/os/user_mode_features.sura",
        "examples/os/vfs_features.sura",
        "Assets/StoreLogo.png"
    )) {
        if ($entryNames -notcontains $requiredEntry) {
            throw "MSIX package is missing required entry: $requiredEntry"
        }
    }
    if ($entryNames -contains "SuraFinal.exe") {
        throw "MSIX package contains the removed SuraFinal.exe alias"
    }
    if ($entryNames -contains "Package.appxmanifest") {
        throw "MSIX package unexpectedly contains its source manifest as payload"
    }

    foreach ($payloadCheck in @(
        @{ Entry = "SuraLanguage.exe"; Source = $engine },
        @{ Entry = "surapkg.exe"; Source = $surapkg }
    )) {
        $payloadEntry = $archive.GetEntry($payloadCheck.Entry)
        $payloadStream = $payloadEntry.Open()
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $payloadHashBytes = $sha256.ComputeHash($payloadStream)
            $payloadHash = -join ($payloadHashBytes | ForEach-Object { $_.ToString('x2') })
        }
        finally {
            $sha256.Dispose()
            $payloadStream.Dispose()
        }
        $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $payloadCheck.Source).Hash.ToLowerInvariant()
        if ($payloadHash -ne $sourceHash) {
            throw "MSIX payload hash does not match source file: $($payloadCheck.Entry)"
        }
    }

    $appxManifestEntry = $archive.GetEntry("AppxManifest.xml")
    $reader = New-Object System.IO.StreamReader($appxManifestEntry.Open(), [System.Text.Encoding]::UTF8)
    try {
        $packedManifestText = $reader.ReadToEnd()
    }
    finally {
        $reader.Dispose()
    }
    foreach ($requiredManifestValue in @(
        'Name="SuraTeam.SuraLanguage"',
        'Publisher="CN=7D09337E-F8F3-4455-BD86-A6928DC8F552"',
        'Alias="sura.exe"',
        'Alias="surapkg.exe"'
    )) {
        if (-not $packedManifestText.Contains($requiredManifestValue)) {
            throw "packed AppxManifest.xml is missing: $requiredManifestValue"
        }
    }
    if ($packedManifestText.Contains('AppListEntry="none"')) {
        throw 'packed AppxManifest.xml marks an application as headless (AppListEntry="none")'
    }
}
finally {
    $archive.Dispose()
}

$msixItem = Get-Item -LiteralPath $msix
$report = [ordered]@{
    schema = "sura.store.msix.v1"
    product_name = "Sura Language"
    store_id = "9P5JFKSWTP0P"
    store_url = "https://apps.microsoft.com/detail/9P5JFKSWTP0P"
    package_identity_name = "SuraTeam.SuraLanguage"
    package_identity_publisher = "CN=7D09337E-F8F3-4455-BD86-A6928DC8F552"
    package_family_name = "SuraTeam.SuraLanguage_skvn0agb8ca3y"
    publisher_display_name = "SuraTeam"
    version = $Version
    architecture = "x64"
    msix_path = $msixItem.FullName
    msix_bytes = $msixItem.Length
    msix_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $msixItem.FullName).Hash.ToLowerInvariant()
    engine_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $engine).Hash.ToLowerInvariant()
    surapkg_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $surapkg).Hash.ToLowerInvariant()
    example_count = @(Get-ChildItem -LiteralPath $examples -Recurse -File -Filter *.sura).Count
    store_submission_signature = "Microsoft signs the package after Store certification"
    package_content_smoke_passed = $true
    local_loose_layout_smoke_passed = $smokePassed
    local_loose_layout_smoke_status = $smokeStatus
}
$reportPath = "$msix.json"
[System.IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 5), $utf8NoBom)

Write-Host "sura_store_msix: PASS"
Write-Host "msix: $msix"
Write-Host "report: $reportPath"
