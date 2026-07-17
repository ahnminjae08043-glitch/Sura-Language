param(
    [string]$RepoRoot = ".",
    [string]$Engine = "",
    [string]$OutDir = "",
    [string]$ZipOut = "",
    [string]$ReportOut = "",
    [string]$SummaryOut = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$root = (Resolve-Path -LiteralPath $RepoRoot).Path

function Resolve-FromRoot {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $root $Path))
}

function Ensure-Parent {
    param([string]$Path)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
}

function Normalize-Relative {
    param([string]$Path)
    return $Path.Replace('\', '/')
}

function Get-RelativeToRoot {
    param([string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    $rootPrefix = $root.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "audit source escapes repository root: $full"
    }
    return Normalize-Relative $full.Substring($rootPrefix.Length)
}

function Get-PortableDisplayPath {
    param([string]$Path)
    try {
        return Get-RelativeToRoot $Path
    } catch {
        return Normalize-Relative ([System.IO.Path]::GetFullPath($Path))
    }
}

function Get-HashLower {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$versionPath = Join-Path $root "version.json"
$compatibilityPath = Join-Path $root "compatibility.json"
$securityPath = Join-Path $root "SECURITY.md"
$auditGuidePath = Join-Path $root "SECURITY_AUDIT.md"
foreach ($required in @($versionPath, $compatibilityPath, $securityPath, $auditGuidePath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "required audit handoff source is missing: $required"
    }
}

$version = [System.IO.File]::ReadAllText($versionPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$compatibility = [System.IO.File]::ReadAllText($compatibilityPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$securityText = [System.IO.File]::ReadAllText($securityPath, [System.Text.Encoding]::UTF8)
$auditGuideText = [System.IO.File]::ReadAllText($auditGuidePath, [System.Text.Encoding]::UTF8)
if ($compatibility.language_version -ne $version.version) {
    throw "version.json and compatibility.json disagree"
}
foreach ($fact in @(
    "no report from an independent external security audit",
    "not an operating-system sandbox",
    "FFI and native plugins load native code into the runtime process"
)) {
    if (-not $securityText.Contains($fact)) {
        throw "SECURITY.md is missing required audit boundary: $fact"
    }
}
foreach ($fact in @(
    "audit report, certification",
    "independent external security audit.",
    "manifest.json"
)) {
    if (-not $auditGuideText.Contains($fact)) {
        throw "SECURITY_AUDIT.md is missing required handoff boundary: $fact"
    }
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = "artifacts/security-audit-bundle-$($version.version)"
}
if ([string]::IsNullOrWhiteSpace($ZipOut)) {
    $ZipOut = "artifacts/sura-security-audit-$($version.version).zip"
}
if ([string]::IsNullOrWhiteSpace($ReportOut)) {
    $ReportOut = "artifacts/security_audit_bundle.json"
}
if ([string]::IsNullOrWhiteSpace($SummaryOut)) {
    $SummaryOut = "artifacts/security_audit_bundle.md"
}

$outDirFull = Resolve-FromRoot $OutDir
$zipFull = Resolve-FromRoot $ZipOut
$reportFull = Resolve-FromRoot $ReportOut
$summaryFull = Resolve-FromRoot $SummaryOut
if ($outDirFull -eq $root -or $outDirFull.Length -lt ($root.Length + 8)) {
    throw "unsafe audit output directory: $outDirFull"
}
foreach ($target in @($outDirFull, $zipFull, $reportFull, $summaryFull)) {
    if (Test-Path -LiteralPath $target) {
        if (-not $Force) { throw "audit output already exists; pass -Force to replace: $target" }
        $item = Get-Item -LiteralPath $target -Force
        if ($item.PSIsContainer) {
            Remove-Item -LiteralPath $target -Recurse -Force
        } else {
            Remove-Item -LiteralPath $target -Force
        }
    }
}
New-Item -ItemType Directory -Force -Path $outDirFull | Out-Null
Ensure-Parent $zipFull
Ensure-Parent $reportFull
Ensure-Parent $summaryFull

$relativeFiles = New-Object System.Collections.Generic.List[string]
foreach ($path in @(
    "version.json", "compatibility.json", "COMPATIBILITY.md", "SCOPE.md",
    "CONTRIBUTING.md", "SECURITY.md", "SECURITY_AUDIT.md", "Makefile",
    "build.bat", "main.cpp", "gc.cpp", "platform.cpp", "surapkg.cpp",
    "sura_ffi.cpp", "sura_ffi.hpp", "sura_plugin.h", "run_stable_tests.ps1",
    ".github/ISSUE_TEMPLATE/security_contact.yml",
    ".github/workflows/cross-platform-smoke.yml",
    ".github/workflows/runtime-soak.yml",
    "tools/sura_security_audit_bundle.ps1",
    "tools/sura_security_audit_bundle_smoke.ps1",
    "tools/sura_security_policy_smoke.ps1",
    "tools/sura_untrusted_input_smoke.ps1",
    "tools/sura_gc_memory_safety_smoke.ps1",
    "tools/sura_ffi_safety_smoke.ps1",
    "tools/sura_async_runtime_concurrency_smoke.ps1",
    "tools/sura_runtime_soak.ps1",
    "tools/sura_test_process.ps1"
)) {
    $relativeFiles.Add($path)
}
Get-ChildItem -LiteralPath $root -File -Filter "*.hpp" | ForEach-Object {
    $relativeFiles.Add((Get-RelativeToRoot $_.FullName))
}
$testsRoot = Join-Path $root "tests"
Get-ChildItem -LiteralPath $testsRoot -Recurse -File | Where-Object {
    $_.Extension -in @(".sura", ".cpp", ".json")
} | ForEach-Object {
    $relativeFiles.Add((Get-RelativeToRoot $_.FullName))
}
$relativeFiles = @($relativeFiles | ForEach-Object { Normalize-Relative $_ } | Sort-Object -Unique)

$sourceEntries = New-Object System.Collections.Generic.List[object]
foreach ($relative in $relativeFiles) {
    if ([System.IO.Path]::IsPathRooted($relative) -or $relative.Contains("..")) {
        throw "unsafe audit source path: $relative"
    }
    $source = Join-Path $root ($relative.Replace('/', [System.IO.Path]::DirectorySeparatorChar))
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "required audit source file is missing: $relative"
    }
    $destination = Join-Path $outDirFull (Join-Path "review" ($relative.Replace('/', [System.IO.Path]::DirectorySeparatorChar)))
    Ensure-Parent $destination
    [System.IO.File]::Copy($source, $destination, $false)
    $item = Get-Item -LiteralPath $source
    $sourceEntries.Add([pscustomobject]@{
        path = $relative
        bytes = [int64]$item.Length
        sha256 = Get-HashLower $source
    })
}

$engineEvidence = $null
if (-not [string]::IsNullOrWhiteSpace($Engine)) {
    $engineFull = Resolve-FromRoot $Engine
    if (-not (Test-Path -LiteralPath $engineFull -PathType Leaf)) {
        throw "engine evidence file not found: $engineFull"
    }
    $engineItem = Get-Item -LiteralPath $engineFull
    $engineEvidence = [pscustomobject]@{
        path = Get-RelativeToRoot $engineFull
        bytes = [int64]$engineItem.Length
        sha256 = Get-HashLower $engineFull
        included_in_bundle = $false
    }
}

$commands = @(
    'make clean',
    'make CXX=g++ CXXFLAGS="-std=c++17 -O2 -DNDEBUG -Wall"',
    'pwsh -NoProfile -File ./run_stable_tests.ps1 -Engine ./SuraLanguage',
    'pwsh -NoProfile -File ./tools/sura_untrusted_input_smoke.ps1 -Cxx g++',
    'pwsh -NoProfile -File ./tools/sura_gc_memory_safety_smoke.ps1 -Cxx g++ -Sanitize',
    'pwsh -NoProfile -File ./tools/sura_ffi_safety_smoke.ps1 -Cxx g++ -Sanitize',
    'pwsh -NoProfile -File ./tools/sura_runtime_soak.ps1 -Engine ./SuraLanguage -DurationSeconds 60 -PerRunTimeoutSeconds 120 -JsonOut artifacts/runtime_soak_audit.json'
)

$manifest = [ordered]@{
    schema = "sura.security.audit.handoff.v1"
    version = [string]$version.version
    series = [string]$version.series
    compatibility_schema = [string]$compatibility.schema
    purpose = "independent security review preparation"
    independent_external_audit = [ordered]@{
        performed = $false
        report_in_repository = $false
        certification_claimed = $false
    }
    trust_boundaries = @(
        "Sura is not an operating-system sandbox",
        "FFI and plugins execute native code in the runtime process",
        "tool policies do not cover every direct host API",
        "external tools and drivers retain their own security boundary"
    )
    reproduction_commands = $commands
    engine_evidence = $engineEvidence
    source_file_count = $sourceEntries.Count
    source_files = @($sourceEntries | ForEach-Object { $_ })
}
$manifestPath = Join-Path $outDirFull "manifest.json"
[System.IO.File]::WriteAllText(
    $manifestPath,
    (($manifest | ConvertTo-Json -Depth 8) + "`n"),
    $utf8NoBom
)

$readme = @"
# Sura security review handoff $($version.version)

This source bundle is review preparation, not an independent audit report or
security certification. `manifest.json` records the SHA-256 and byte count of
every copied source. The engine binary is not included. If engine evidence is
present in the manifest, it records only the local binary hash and byte count.

Read `review/SECURITY.md` and `review/SECURITY_AUDIT.md` before testing. Report
findings through the private process documented in `review/SECURITY.md`.
"@
[System.IO.File]::WriteAllText((Join-Path $outDirFull "README.md"), ($readme.TrimEnd() + "`n"), $utf8NoBom)

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $outDirFull,
    $zipFull,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false
)

$manifestItem = Get-Item -LiteralPath $manifestPath
$zipItem = Get-Item -LiteralPath $zipFull
$report = [ordered]@{
    schema = "sura.security.audit.bundle.report.v1"
    version = [string]$version.version
    status = "HANDOFF_CREATED"
    independent_external_audit_performed = $false
    source_file_count = $sourceEntries.Count
    bundle_directory = Get-PortableDisplayPath $outDirFull
    archive = [ordered]@{
        path = Get-PortableDisplayPath $zipFull
        bytes = [int64]$zipItem.Length
        sha256 = Get-HashLower $zipFull
    }
    manifest = [ordered]@{
        path = Get-PortableDisplayPath $manifestPath
        bytes = [int64]$manifestItem.Length
        sha256 = Get-HashLower $manifestPath
    }
    engine_evidence = $engineEvidence
}
[System.IO.File]::WriteAllText($reportFull, (($report | ConvertTo-Json -Depth 6) + "`n"), $utf8NoBom)

$engineSummary = if ($null -eq $engineEvidence) {
    "- Engine evidence: not supplied"
} else {
    "- Engine evidence: $($engineEvidence.bytes) bytes, SHA-256 $($engineEvidence.sha256) (binary not included)"
}
$zipDisplay = Get-PortableDisplayPath $zipFull
$summary = @"
# Sura security audit handoff

- Status: handoff package created for external source review
- Independent external audit performed: no
- Version: $($version.version)
- Source files: $($sourceEntries.Count)
- Archive: $zipDisplay
- Archive SHA-256: $($report.archive.sha256)
$engineSummary

This package is not a security certification and does not prove the absence of
vulnerabilities.
"@
[System.IO.File]::WriteAllText($summaryFull, ($summary.TrimEnd() + "`n"), $utf8NoBom)

Write-Host ("sura_security_audit_bundle: PASS ({0} files, {1} bytes, no external-audit claim)" -f $sourceEntries.Count, $zipItem.Length)
