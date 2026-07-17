param(
    [string]$RepoRoot = ".",
    [string]$Engine = "",
    [string]$Surapkg = "",
    [string]$ContractPath = "",
    [string]$JsonOut = "artifacts\compatibility_report.json"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$root = (Resolve-Path -LiteralPath $RepoRoot).Path

function Resolve-FromRoot([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $root $Path
}

if ([string]::IsNullOrWhiteSpace($Engine)) {
    $candidate = Join-Path $root $(if ($env:OS -eq "Windows_NT") { "SuraLanguage.exe" } else { "SuraLanguage" })
    if (-not (Test-Path -LiteralPath $candidate)) {
        throw "Sura engine not found; build it or pass -Engine"
    }
    $Engine = $candidate
}
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
if ([string]::IsNullOrWhiteSpace($Surapkg)) {
    $candidate = Join-Path $root $(if ($env:OS -eq "Windows_NT") { "surapkg.exe" } else { "surapkg" })
    if (-not (Test-Path -LiteralPath $candidate)) {
        throw "surapkg was not found; build it or pass -Surapkg"
    }
    $Surapkg = $candidate
}
$surapkgPath = (Resolve-Path -LiteralPath $Surapkg).Path

function Read-Utf8([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "required file not found: $Path" }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Invoke-EngineCapture([string]$RuntimePath, [string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = (& $RuntimePath @Arguments 2>&1 | ForEach-Object { "$_" }) -join "`n"
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

function Invoke-Capture([string[]]$Arguments) {
    return (Invoke-EngineCapture $enginePath $Arguments)
}

function Invoke-PackageCapture([string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = (& $surapkgPath @Arguments 2>&1 | ForEach-Object { "$_" }) -join "`n"
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

function Require-Match([string]$Text, [string]$Pattern, [string]$Label) {
    $match = [regex]::Match($Text, $Pattern)
    if (-not $match.Success) { throw "$Label is missing from its authoritative source" }
    return $match.Groups[1].Value
}

function Parse-IsoDate([string]$Value, [string]$Label) {
    $parsed = [DateTime]::MinValue
    $ok = [DateTime]::TryParseExact(
        $Value,
        "yyyy-MM-dd",
        [System.Globalization.CultureInfo]::InvariantCulture,
        [System.Globalization.DateTimeStyles]::None,
        [ref]$parsed
    )
    if (-not $ok) { throw "$Label must use YYYY-MM-DD" }
    return $parsed
}

$contractPath = if ([string]::IsNullOrWhiteSpace($ContractPath)) {
    Join-Path $root "compatibility.json"
} else {
    Resolve-FromRoot $ContractPath
}
$versionPath = Join-Path $root "version.json"
$contract = (Read-Utf8 $contractPath) | ConvertFrom-Json
$version = (Read-Utf8 $versionPath) | ConvertFrom-Json

if ($contract.schema -ne "sura.compatibility.v1") { throw "invalid compatibility schema" }
if ($contract.language_version -ne $version.version) { throw "compatibility version does not match version.json" }
if ($contract.stable_series -ne $version.series) { throw "compatibility series does not match version.json" }
if ($contract.source.guarantee_starts_at -ne "1.11.1") { throw "source guarantee baseline changed unexpectedly" }
if ($contract.source.patch_release_breaking_changes -ne "forbidden") { throw "patch compatibility policy must forbid breaking changes" }
if ([int]$contract.source.minor_release_deprecation_period -lt 1) { throw "minor release deprecation period must be at least one release" }
foreach ($tier in @("stable", "platform_limited", "experimental")) {
    if ($null -eq $contract.support_tiers.$tier -or @($contract.support_tiers.$tier).Count -eq 0) {
        throw "compatibility support tier is missing or empty: $tier"
    }
}

$policyEffective = Parse-IsoDate ([string]$contract.support.policy_effective) "support policy effective date"
$maintenanceUntil = Parse-IsoDate ([string]$contract.support.minimum_maintenance_until) "minimum maintenance date"
$minimumMonths = [int]$contract.support.minimum_maintenance_months
$overlapMonths = [int]$contract.support.next_minor_overlap_months
if ($minimumMonths -lt 12) { throw "minimum maintenance period must be at least 12 months" }
if ($overlapMonths -lt 6) { throw "next minor overlap period must be at least 6 months" }
if ($maintenanceUntil -lt $policyEffective.AddMonths($minimumMonths)) {
    throw "minimum maintenance date is earlier than the declared maintenance period"
}
$versionMajorMatch = [regex]::Match([string]$version.version, '^(\d+)\.')
if (-not $versionMajorMatch.Success -or [int]$contract.support.source_compatibility_major -ne [int]$versionMajorMatch.Groups[1].Value) {
    throw "source compatibility major does not match the language version"
}
$seriesSupport = @($contract.support.stable_series | Where-Object { [string]$_.series -eq [string]$contract.stable_series })
if ($seriesSupport.Count -ne 1) { throw "current stable series must have exactly one support record" }
if ([string]$seriesSupport[0].status -ne "active") { throw "current stable series support status must be active" }
if ([string]$seriesSupport[0].first_supported_patch -ne [string]$contract.source.guarantee_starts_at) {
    throw "stable series first supported patch does not match the source guarantee baseline"
}
$seriesMaintenance = Parse-IsoDate ([string]$seriesSupport[0].maintenance_not_before) "stable series maintenance date"
if ($seriesMaintenance -lt $maintenanceUntil) {
    throw "stable series maintenance date is earlier than the contract minimum"
}

foreach ($deprecation in @($contract.deprecations)) {
    foreach ($field in @("id", "introduced", "removal_not_before", "replacement", "warning")) {
        if ([string]::IsNullOrWhiteSpace([string]$deprecation.$field)) {
            throw "deprecation entry is missing required field: $field"
        }
    }
    if ([string]$deprecation.introduced -notmatch '^\d+\.\d+\.\d+$' -or
        [string]$deprecation.removal_not_before -notmatch '^\d+\.\d+\.\d+$') {
        throw "deprecation versions must use major.minor.patch"
    }
}

$historicalProbes = @($contract.source.historical_probes)
if ($historicalProbes.Count -eq 0) { throw "at least one historical compatibility probe is required" }
$historicalSeries = @{}
foreach ($probe in $historicalProbes) {
    foreach ($field in @("series", "runtime_version", "status", "platform", "archive", "archive_entry", "release_manifest", "verification_manifest", "bytecode_fixture")) {
        if ([string]::IsNullOrWhiteSpace([string]$probe.$field)) {
            throw "historical compatibility probe is missing required field: $field"
        }
    }
    if ([string]$probe.status -ne "verification_only") {
        throw "historical compatibility probes must be marked verification_only"
    }
    if ([string]$probe.platform -ne "windows-x64") {
        throw "unsupported historical compatibility probe platform: $($probe.platform)"
    }
    if ([string]$probe.series -notmatch '^\d+\.\d+$' -or [string]$probe.runtime_version -notmatch '^\d+\.\d+\.\d+$' -or
        -not ([string]$probe.runtime_version).StartsWith(([string]$probe.series) + ".")) {
        throw "historical compatibility probe version/series is invalid"
    }
    if ($historicalSeries.ContainsKey([string]$probe.series)) {
        throw "duplicate historical compatibility probe series: $($probe.series)"
    }
    $historicalSeries[[string]$probe.series] = $true
    if ([version]([string]$probe.runtime_version) -ge [version]([string]$contract.source.guarantee_starts_at)) {
        throw "historical compatibility probe must predate the guaranteed source baseline"
    }
    if (@($probe.fixtures).Count -eq 0) {
        throw "historical compatibility probe has no source fixtures: $($probe.series)"
    }
    $fixturePaths = @($probe.fixtures | ForEach-Object { [string]$_.path })
    if (@($fixturePaths | Sort-Object -Unique).Count -ne $fixturePaths.Count) {
        throw "historical compatibility probe contains duplicate fixture paths: $($probe.series)"
    }
    foreach ($fixture in @($probe.fixtures)) {
        if ([string]::IsNullOrWhiteSpace([string]$fixture.path) -or [string]::IsNullOrWhiteSpace([string]$fixture.marker)) {
            throw "historical compatibility fixture requires path and marker: $($probe.series)"
        }
    }
    if ([string]$probe.bytecode_fixture -notin $fixturePaths) {
        throw "historical bytecode fixture must be one of the source fixtures: $($probe.series)"
    }
    foreach ($pathField in @("archive", "release_manifest", "verification_manifest", "bytecode_fixture")) {
        $requiredPath = Resolve-FromRoot ([string]$probe.$pathField)
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "historical compatibility probe file is missing: $($probe.$pathField)"
        }
    }
}

if ([string]$contract.stable_api.removal_without_deprecation -ne "forbidden" -or
    [string]$contract.stable_api.signature_change_without_deprecation -ne "forbidden") {
    throw "stable API policy must forbid unannounced removal and signature changes"
}
$stableModuleNames = @($contract.stable_api.modules | ForEach-Object { [string]$_ })
if ($stableModuleNames.Count -eq 0) { throw "stable API module list is empty" }
if (@($stableModuleNames | Sort-Object -Unique).Count -ne $stableModuleNames.Count) {
    throw "stable API module list contains duplicates"
}
$stableApiPath = Resolve-FromRoot ([string]$contract.stable_api.snapshot)
$stableApiSnapshot = (Read-Utf8 $stableApiPath) | ConvertFrom-Json
if ($stableApiSnapshot.schema -ne "sura.stable_api.v1") { throw "invalid stable API snapshot schema" }
if ([string]$stableApiSnapshot.series -ne [string]$contract.stable_series) { throw "stable API snapshot series mismatch" }
if ([string]$stableApiSnapshot.baseline_version -ne [string]$contract.source.guarantee_starts_at) {
    throw "stable API snapshot baseline mismatch"
}
$snapshotModuleNames = @($stableApiSnapshot.modules | ForEach-Object { [string]$_.name })
if ((($snapshotModuleNames | Sort-Object) -join "`n") -ne (($stableModuleNames | Sort-Object) -join "`n")) {
    throw "stable API snapshot modules do not match compatibility.json"
}
$stableModuleCount = @($stableApiSnapshot.modules).Count
$stableSymbolCount = 0
foreach ($module in @($stableApiSnapshot.modules)) {
    if (@($module.symbols).Count -eq 0) { throw "stable API snapshot module is empty: $($module.name)" }
    $keys = @($module.symbols | ForEach-Object { "$([string]$_.kind)`n$([string]$_.name)`n$([string]$_.signature)" })
    if (@($keys | Sort-Object -Unique).Count -ne $keys.Count) {
        throw "stable API snapshot contains duplicate symbols: $($module.name)"
    }
    $stableSymbolCount += @($module.symbols).Count
}

$engineVersion = Invoke-Capture @("--version")
if ($engineVersion.ExitCode -ne 0 -or $engineVersion.Output -notmatch [regex]::Escape($version.version)) {
    throw "engine version does not match compatibility contract: $($engineVersion.Output)"
}

$bytecodeHeader = Read-Utf8 (Join-Path $root "bytecode_io.hpp")
$bytecodeCurrent = [int](Require-Match $bytecodeHeader 'BC_VERSION\s*=\s*(\d+)' "BC_VERSION")
$bytecodeLegacy = [int](Require-Match $bytecodeHeader 'BC_VERSION_LEGACY\s*=\s*(\d+)' "BC_VERSION_LEGACY")
$releaseCurrent = [int](Require-Match $bytecodeHeader 'RELEASE_VERSION\s*=\s*(\d+)' "RELEASE_VERSION")
$releaseLegacy = [int](Require-Match $bytecodeHeader 'RELEASE_VERSION_LEGACY\s*=\s*(\d+)' "RELEASE_VERSION_LEGACY")
$releaseKeyed = [int](Require-Match $bytecodeHeader 'RELEASE_VERSION_KEYED\s*=\s*(\d+)' "RELEASE_VERSION_KEYED")
$releaseMetadata = [int](Require-Match $bytecodeHeader 'RELEASE_VERSION_METADATA\s*=\s*(\d+)' "RELEASE_VERSION_METADATA")
$releaseLicensed = [int](Require-Match $bytecodeHeader 'RELEASE_VERSION_LICENSED\s*=\s*(\d+)' "RELEASE_VERSION_LICENSED")

if ($bytecodeCurrent -ne [int]$contract.bytecode.current -or
    $bytecodeLegacy -notin @($contract.bytecode.accepted) -or
    $bytecodeCurrent -notin @($contract.bytecode.accepted)) {
    throw "bytecode constants do not match compatibility.json"
}
$actualReleaseVersions = @($releaseLegacy, $releaseKeyed, $releaseMetadata, $releaseLicensed, $releaseCurrent) | Sort-Object -Unique
$declaredReleaseVersions = @($contract.release_package.accepted | ForEach-Object { [int]$_ }) | Sort-Object -Unique
if (($actualReleaseVersions -join ",") -ne ($declaredReleaseVersions -join ",") -or
    $releaseCurrent -ne [int]$contract.release_package.current) {
    throw "release package constants do not match compatibility.json"
}

function Read-AbiVersion([string]$Path, [string]$Prefix) {
    $text = Read-Utf8 $Path
    $major = Require-Match $text ("#define\s+" + $Prefix + "_ABI_VERSION_MAJOR\s+(\d+)") "$Prefix ABI major"
    $minor = Require-Match $text ("#define\s+" + $Prefix + "_ABI_VERSION_MINOR\s+(\d+)") "$Prefix ABI minor"
    $patch = Require-Match $text ("#define\s+" + $Prefix + "_ABI_VERSION_PATCH\s+(\d+)") "$Prefix ABI patch"
    return "$major.$minor.$patch"
}

$pluginAbi = Read-AbiVersion (Join-Path $root "sura_plugin.h") "SURA_PLUGIN"
$ffiAbi = Read-AbiVersion (Join-Path $root "sura_ffi.hpp") "SURA_FFI"
if ($pluginAbi -ne $contract.plugin_abi) { throw "plugin ABI does not match compatibility.json" }
if ($ffiAbi -ne $contract.ffi_abi) { throw "FFI ABI does not match compatibility.json" }

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_compat_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$fixtureReports = New-Object System.Collections.Generic.List[object]
$historicalProbeReports = New-Object System.Collections.Generic.List[object]
try {
    $apiFixture = Join-Path $root "tests/compat/api_fixture"
    if (-not (Test-Path -LiteralPath (Join-Path $apiFixture "sura.pkg.json"))) {
        throw "stable API fixture package is missing"
    }
    $apiOut = Join-Path $temp "api-docs"
    Push-Location $apiFixture
    try {
        $docs = Invoke-PackageCapture @("docs", $apiOut)
    } finally {
        Pop-Location
    }
    if ($docs.ExitCode -ne 0) { throw "stable API docs generation failed: $($docs.Output)" }
    $currentApi = (Read-Utf8 (Join-Path $apiOut "api.json")) | ConvertFrom-Json

    foreach ($baselineModule in @($stableApiSnapshot.modules)) {
        $moduleName = [string]$baselineModule.name
        $currentModules = @($currentApi.stdlibModules | Where-Object { [string]$_.name -eq $moduleName })
        if ($currentModules.Count -ne 1) { throw "stable API module is missing from generated docs: $moduleName" }
        foreach ($baselineSymbol in @($baselineModule.symbols)) {
            $kind = [string]$baselineSymbol.kind
            $name = [string]$baselineSymbol.name
            $signature = [string]$baselineSymbol.signature
            $sameName = @($currentModules[0].symbols | Where-Object {
                [string]$_.kind -eq $kind -and [string]$_.name -eq $name
            })
            if ($sameName.Count -eq 0) {
                throw "stable API symbol was removed: $moduleName.$name ($kind)"
            }
            $exact = @($sameName | Where-Object { [string]$_.signature -eq $signature })
            if ($exact.Count -eq 0) {
                $currentSignatures = @($sameName | ForEach-Object { [string]$_.signature }) -join " | "
                throw "stable API signature changed for $moduleName.$name; expected '$signature', current '$currentSignatures'"
            }
        }
    }

    foreach ($fixture in @($contract.source.fixtures)) {
        $sourcePath = Join-Path $root ([string]$fixture.path)
        $source = Read-Utf8 $sourcePath
        if ($source -match '(?m)^\s*(let|var|const)\s+' -or $source -match '(?m)^\s*fn\s+') {
            throw "compatibility fixture contains non-Sura declaration syntax: $($fixture.path)"
        }

        $check = Invoke-Capture @("--strict-syntax", "--check", $sourcePath)
        if ($check.ExitCode -ne 0) { throw "strict source check failed for $($fixture.path): $($check.Output)" }

        $vm = Invoke-Capture @($sourcePath)
        if ($vm.ExitCode -ne 0 -or $vm.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
            throw "VM compatibility fixture failed for $($fixture.path): $($vm.Output)"
        }

        $jit = Invoke-Capture @("--jit", $sourcePath)
        if ($jit.ExitCode -ne 0 -or $jit.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
            throw "JIT/fallback compatibility fixture failed for $($fixture.path): $($jit.Output)"
        }

        $bcPath = Join-Path $temp (([System.IO.Path]::GetFileNameWithoutExtension($sourcePath)) + ".sura.bc")
        $compile = Invoke-Capture @("--compile", $sourcePath, "--out", $bcPath)
        if ($compile.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $bcPath)) {
            throw "bytecode compile failed for $($fixture.path): $($compile.Output)"
        }
        $load = Invoke-Capture @("--load", $bcPath)
        if ($load.ExitCode -ne 0 -or $load.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
            throw "current bytecode load failed for $($fixture.path): $($load.Output)"
        }

        if ($fixture.path -eq "tests/compat/1.11/core.sura") {
            $legacyPath = Join-Path $temp "core-v2.sura.bc"
            $bytes = [System.IO.File]::ReadAllBytes($bcPath)
            if ($bytes.Length -lt 5 -or [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne "SURB") {
                throw "compiled bytecode has an invalid header"
            }
            $bytes[4] = [byte]$bytecodeLegacy
            [System.IO.File]::WriteAllBytes($legacyPath, $bytes)
            $legacyLoad = Invoke-Capture @("--load", $legacyPath)
            if ($legacyLoad.ExitCode -ne 0 -or $legacyLoad.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
                throw "legacy bytecode v$bytecodeLegacy load failed: $($legacyLoad.Output)"
            }
        }

        $fixtureReports.Add([pscustomobject]@{
            path = [string]$fixture.path
            marker = [string]$fixture.marker
            strict_check = "passed"
            vm = "passed"
            jit_or_fallback = "passed"
            bytecode = "passed"
        })
    }

    foreach ($probe in $historicalProbes) {
        $currentFixtureReports = New-Object System.Collections.Generic.List[object]
        foreach ($fixture in @($probe.fixtures)) {
            $sourcePath = Resolve-FromRoot ([string]$fixture.path)
            $source = Read-Utf8 $sourcePath
            if ($source -match '(?m)^\s*(let|var|const)\s+' -or $source -match '(?m)^\s*fn\s+') {
                throw "historical compatibility fixture contains non-Sura declaration syntax: $($fixture.path)"
            }

            $check = Invoke-Capture @("--strict-syntax", "--check", $sourcePath)
            if ($check.ExitCode -ne 0) { throw "strict source check failed for historical fixture $($fixture.path): $($check.Output)" }
            $vm = Invoke-Capture @($sourcePath)
            if ($vm.ExitCode -ne 0 -or $vm.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
                throw "current VM failed historical fixture $($fixture.path): $($vm.Output)"
            }
            $jit = Invoke-Capture @("--jit", $sourcePath)
            if ($jit.ExitCode -ne 0 -or $jit.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
                throw "current JIT/fallback failed historical fixture $($fixture.path): $($jit.Output)"
            }
            $safeName = (([string]$fixture.path) -replace '[^A-Za-z0-9._-]', '_')
            $bcPath = Join-Path $temp ("current-" + $safeName + ".sura.bc")
            $compile = Invoke-Capture @("--compile", $sourcePath, "--out", $bcPath)
            if ($compile.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $bcPath)) {
                throw "current bytecode compile failed for historical fixture $($fixture.path): $($compile.Output)"
            }
            $load = Invoke-Capture @("--load", $bcPath)
            if ($load.ExitCode -ne 0 -or $load.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
                throw "current bytecode load failed for historical fixture $($fixture.path): $($load.Output)"
            }
            $currentFixtureReports.Add([pscustomobject]@{
                path = [string]$fixture.path
                marker = [string]$fixture.marker
                strict_check = "passed"
                vm = "passed"
                jit_or_fallback = "passed"
                bytecode = "passed"
            }) | Out-Null
        }

        $archivedRuntime = [ordered]@{
            platform = [string]$probe.platform
            status = "not_applicable"
            reason = "archived Windows x64 runtime execution is only performed on a Windows AMD64 host"
            engine_version = [string]$probe.runtime_version
            engine_sha256 = ""
            archive_sha256 = ""
            fixtures = @()
            bytecode_forward_load = "not_applicable"
        }
        $historicalApplicable = ($env:OS -eq "Windows_NT" -and [string]$env:PROCESSOR_ARCHITECTURE -eq "AMD64")
        if ($historicalApplicable) {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $archivePath = Resolve-FromRoot ([string]$probe.archive)
            $releaseManifest = (Read-Utf8 (Resolve-FromRoot ([string]$probe.release_manifest))) | ConvertFrom-Json
            $verificationManifest = (Read-Utf8 (Resolve-FromRoot ([string]$probe.verification_manifest))) | ConvertFrom-Json
            if ([string]$releaseManifest.version -ne [string]$probe.runtime_version -or
                [string]$verificationManifest.version -ne [string]$probe.runtime_version) {
                throw "historical runtime manifests do not match probe version $($probe.runtime_version)"
            }
            $archiveName = [System.IO.Path]::GetFileName($archivePath)
            $releaseArtifacts = @($releaseManifest.artifacts | Where-Object { [string]$_.name -eq $archiveName })
            if ($releaseArtifacts.Count -ne 1) { throw "historical archive is missing from release manifest: $archiveName" }
            $archiveItem = Get-Item -LiteralPath $archivePath
            $archiveSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ([long]$releaseArtifacts[0].bytes -ne [long]$archiveItem.Length -or
                [string]$releaseArtifacts[0].sha256 -ne $archiveSha256) {
                throw "historical archive does not match its release manifest: $archiveName"
            }

            $historicalDir = Join-Path $temp ("historical-" + [string]$probe.runtime_version)
            New-Item -ItemType Directory -Force -Path $historicalDir | Out-Null
            $archivedEnginePath = Join-Path $historicalDir "SuraLanguage.exe"
            $zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
            try {
                $normalizedEntry = ([string]$probe.archive_entry).Replace("\", "/")
                $entries = @($zip.Entries | Where-Object { ([string]$_.FullName).Replace("\", "/") -eq $normalizedEntry })
                if ($entries.Count -ne 1) { throw "historical engine entry is missing from archive: $($probe.archive_entry)" }
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $archivedEnginePath, $true)
            } finally {
                $zip.Dispose()
            }
            $engineItem = Get-Item -LiteralPath $archivedEnginePath
            $engineSha256 = (Get-FileHash -LiteralPath $archivedEnginePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ([long]$verificationManifest.engine_bytes -ne [long]$engineItem.Length -or
                [string]$verificationManifest.engine_sha256 -ne $engineSha256) {
                throw "historical engine does not match its verification manifest: $($probe.runtime_version)"
            }
            $archivedVersion = Invoke-EngineCapture $archivedEnginePath @("--version")
            if ($archivedVersion.ExitCode -ne 0 -or $archivedVersion.Output -notmatch [regex]::Escape([string]$probe.runtime_version)) {
                throw "historical engine version check failed: $($archivedVersion.Output)"
            }

            $archivedFixtureReports = New-Object System.Collections.Generic.List[object]
            foreach ($fixture in @($probe.fixtures)) {
                $sourcePath = Resolve-FromRoot ([string]$fixture.path)
                $oldVm = Invoke-EngineCapture $archivedEnginePath @($sourcePath)
                if ($oldVm.ExitCode -ne 0 -or $oldVm.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
                    throw "historical VM failed $($fixture.path): $($oldVm.Output)"
                }
                $oldJit = Invoke-EngineCapture $archivedEnginePath @("--jit", $sourcePath)
                if ($oldJit.ExitCode -ne 0 -or $oldJit.Output -notmatch [regex]::Escape([string]$fixture.marker)) {
                    throw "historical JIT/fallback failed $($fixture.path): $($oldJit.Output)"
                }
                $archivedFixtureReports.Add([pscustomobject]@{
                    path = [string]$fixture.path
                    vm = "passed"
                    jit_or_fallback = "passed"
                }) | Out-Null
            }

            $bytecodeFixture = @($probe.fixtures | Where-Object { [string]$_.path -eq [string]$probe.bytecode_fixture })[0]
            $historicalBytecode = Join-Path $historicalDir "historical.sura.bc"
            $oldCompile = Invoke-EngineCapture $archivedEnginePath @("--compile", (Resolve-FromRoot ([string]$bytecodeFixture.path)), "--out", $historicalBytecode)
            if ($oldCompile.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $historicalBytecode)) {
                throw "historical bytecode compile failed: $($oldCompile.Output)"
            }
            $forwardLoad = Invoke-Capture @("--load", $historicalBytecode)
            if ($forwardLoad.ExitCode -ne 0 -or $forwardLoad.Output -notmatch [regex]::Escape([string]$bytecodeFixture.marker)) {
                throw "current engine failed to load historical bytecode: $($forwardLoad.Output)"
            }
            $archivedRuntime = [ordered]@{
                platform = [string]$probe.platform
                status = "passed"
                reason = ""
                engine_version = [string]$probe.runtime_version
                engine_sha256 = $engineSha256
                archive_sha256 = $archiveSha256
                fixtures = @($archivedFixtureReports | ForEach-Object { $_ })
                bytecode_forward_load = "passed"
            }
        }

        $historicalProbeReports.Add([pscustomobject]@{
            series = [string]$probe.series
            runtime_version = [string]$probe.runtime_version
            status = [string]$probe.status
            guarantee = "none; historical verification evidence only"
            current_runtime = [ordered]@{
                status = "passed"
                fixtures = @($currentFixtureReports | ForEach-Object { $_ })
            }
            archived_runtime = $archivedRuntime
        }) | Out-Null
    }
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $tempParent = [System.IO.Path]::GetDirectoryName($resolvedTemp).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $tempLeaf = [System.IO.Path]::GetFileName($resolvedTemp)
    if ($tempParent -eq $tempRoot -and $tempLeaf -match '^sura_compat_[0-9a-f]{32}$' -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$report = [ordered]@{
    schema = "sura.compatibility.report.v1"
    generated_utc = [DateTime]::UtcNow.ToString("o")
    passed = $true
    language_version = [string]$version.version
    stable_series = [string]$version.series
    engine = $enginePath
    surapkg = $surapkgPath
    support = $contract.support
    deprecations = @($contract.deprecations)
    stable_api = [ordered]@{
        snapshot = [string]$contract.stable_api.snapshot
        modules = $stableModuleCount
        signatures = $stableSymbolCount
        checked = "passed"
    }
    bytecode = [ordered]@{ current = $bytecodeCurrent; accepted = @($contract.bytecode.accepted) }
    release_package = [ordered]@{ current = $releaseCurrent; accepted = @($contract.release_package.accepted) }
    plugin_abi = $pluginAbi
    ffi_abi = $ffiAbi
    support_tiers = $contract.support_tiers
    fixtures = @($fixtureReports | ForEach-Object { $_ })
    historical_probes = @($historicalProbeReports | ForEach-Object { $_ })
}

$outPath = if ([System.IO.Path]::IsPathRooted($JsonOut)) { $JsonOut } else { Join-Path $root $JsonOut }
$parent = Split-Path -Parent $outPath
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
[System.IO.File]::WriteAllText($outPath, ($report | ConvertTo-Json -Depth 10) + "`n", [System.Text.UTF8Encoding]::new($false))
$historicalFixtureCount = 0
foreach ($probeReport in @($historicalProbeReports | ForEach-Object { $_ })) { $historicalFixtureCount += @($probeReport.current_runtime.fixtures).Count }
Write-Host "sura_compatibility_gate: PASS ($($fixtureReports.Count) guaranteed source fixtures, $historicalFixtureCount historical source fixtures, $stableModuleCount stable API modules/$stableSymbolCount signatures, bytecode v$bytecodeLegacy/v$bytecodeCurrent, plugin ABI $pluginAbi, FFI ABI $ffiAbi)"
