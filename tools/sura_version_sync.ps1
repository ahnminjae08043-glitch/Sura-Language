param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$Apply,
    [switch]$SkipEngine,
    [switch]$SkipPublicRelease,
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$versionPath = Join-Path $root "version.json"
if (-not (Test-Path -LiteralPath $versionPath)) {
    throw "version contract was not found: $versionPath"
}

# The marketing website lives in its own repository; the language-core tree
# does not ship it. Its version checks apply only when this repository still
# tracks the site, so an untracked local working copy left on disk does not
# resurrect contracts the repository no longer owns.
$hasWebsite = $false
if (Test-Path -LiteralPath (Join-Path $root "sura_presentation") -PathType Container) {
    $hasWebsite = $true
    if (Get-Command git -CommandType Application -ErrorAction SilentlyContinue) {
        $tracked = & git -C $root ls-files "sura_presentation/package.json"
        if ($LASTEXITCODE -eq 0) { $hasWebsite = -not [string]::IsNullOrWhiteSpace(($tracked | Out-String)) }
    }
}

$contract = [System.IO.File]::ReadAllText($versionPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$version = [string]$contract.version
$series = [string]$contract.series
if ($contract.schema -ne "sura.version.v1" -or
    $version -notmatch '^\d+\.\d+\.\d+$' -or
    $series -ne (($version -split '\.')[0..1] -join '.')) {
    throw "version.json does not satisfy sura.version.v1"
}

function Read-RepoText {
    param([string]$RelativePath)
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    return [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
}

function Write-RepoText {
    param([string]$RelativePath, [string]$Text)
    $path = Join-Path $root $RelativePath
    $parent = Split-Path -Parent $path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($path, $Text, $utf8NoBom)
}

function Set-FirstJsonVersions {
    param([string]$RelativePath, [int]$Count)
    $text = Read-RepoText $RelativePath
    $pattern = New-Object System.Text.RegularExpressions.Regex('("version"\s*:\s*")[^"]+("\s*[,}])')
    $matches = @($pattern.Matches($text))
    if ($matches.Count -lt $Count) {
        throw "$RelativePath has fewer than $Count version fields"
    }
    for ($index = $Count - 1; $index -ge 0; $index--) {
        $match = $matches[$index]
        $replacement = $match.Groups[1].Value + $version + $match.Groups[2].Value
        $text = $text.Remove($match.Index, $match.Length).Insert($match.Index, $replacement)
    }
    Write-RepoText $RelativePath $text
}

function Set-ExampleVersion {
    param([string]$RelativePath)
    $text = Read-RepoText $RelativePath
    $pattern = New-Object System.Text.RegularExpressions.Regex('(version:\s*")\d+\.\d+\.\d+(")')
    $match = $pattern.Match($text)
    $updated = $text
    if ($match.Success) {
        $replacement = $match.Groups[1].Value + $version + $match.Groups[2].Value
        $updated = $text.Remove($match.Index, $match.Length).Insert($match.Index, $replacement)
    }
    if ($updated -eq $text -and $text -notmatch ('version:\s*"' + [regex]::Escape($version) + '"')) {
        throw "could not update example version in $RelativePath"
    }
    Write-RepoText $RelativePath $updated
}

if ($Apply) {
    Write-RepoText "sura_version.hpp" ("#pragma once`r`n`r`n// Generated from version.json by tools/sura_version_sync.ps1.`r`nstatic constexpr const char* SURA_LANGUAGE_VERSION = `"$version`";`r`n")
    Set-FirstJsonVersions "sura-vscode/package.json" 1
    Set-FirstJsonVersions "sura-vscode/package-lock.json" 2
    Set-ExampleVersion "examples/starter/05_collections.sura"
    if ($hasWebsite) {
        Write-RepoText "sura_presentation/src/version.js" ("// Generated from repository version.json by tools/sura_version_sync.ps1.`r`nexport const VERSION = `"$version`";`r`n")
        Set-FirstJsonVersions "sura_presentation/package.json" 1
        Set-FirstJsonVersions "sura_presentation/package-lock.json" 2
        Set-ExampleVersion "sura_presentation/public/examples/starter/05_collections.sura"
    }
}

$checks = New-Object System.Collections.Generic.List[object]
function Add-Check {
    param([string]$Name, [bool]$Passed, [string]$Detail)
    $checks.Add([pscustomobject]@{ name = $Name; passed = $Passed; detail = $Detail })
}

$headerText = Read-RepoText "sura_version.hpp"
Add-Check "runtime header" ($headerText -match ('SURA_LANGUAGE_VERSION\s*=\s*"' + [regex]::Escape($version) + '"')) "sura_version.hpp must match version.json"

$mainText = Read-RepoText "main.cpp"
Add-Check "runtime source" ($mainText.Contains('#include "sura_version.hpp"') -and $mainText -notmatch 'SURA_LANGUAGE_VERSION\s*=') "main.cpp must consume the generated version header"

$onnxText = Read-RepoText "onnx_weights.hpp"
Add-Check "ONNX producer" ($onnxText.Contains('model.string(3, SURA_LANGUAGE_VERSION)')) "ONNX producer metadata must use the runtime version constant"

$packageChecks = @(
    @("VS Code package", "sura-vscode/package.json", 1),
    @("VS Code lockfile", "sura-vscode/package-lock.json", 2)
)
if ($hasWebsite) {
    $packageChecks += , @("website package", "sura_presentation/package.json", 1)
    $packageChecks += , @("website lockfile", "sura_presentation/package-lock.json", 2)
}
foreach ($item in $packageChecks) {
    $label = [string]$item[0]
    $relative = [string]$item[1]
    $count = [int]$item[2]
    $text = Read-RepoText $relative
    if ($count -eq 1) {
        $parsed = $text | ConvertFrom-Json
        $ok = ([string]$parsed.version -eq $version)
    } else {
        $versionMatches = @([regex]::Matches($text, '"version"\s*:\s*"([^"]+)"'))
        $ok = ($versionMatches.Count -ge 2 -and
            $versionMatches[0].Groups[1].Value -eq $version -and
            $versionMatches[1].Groups[1].Value -eq $version)
    }
    Add-Check $label $ok "$relative must match version.json"
}

$extensionPackage = (Read-RepoText "sura-vscode/package.json") | ConvertFrom-Json
Add-Check "dynamic VSIX filename" ([string]$extensionPackage.scripts."package:vsix" -eq 'npm run check && node scripts/package-vsix.js') "VSIX packaging must derive its filename from package.json"
$vsixScript = Read-RepoText "sura-vscode/scripts/package-vsix.js"
Add-Check "VSIX packaging script" ($vsixScript.Contains('SuraLanguage-VSCode-${pkg.version}.vsix')) "package-vsix.js must use the extension package version"
$extensionSmoke = Read-RepoText "sura-vscode/scripts/smoke.js"
Add-Check "VS Code version test" ($extensionSmoke.Contains("path.join(repo, 'version.json')")) "extension smoke must read version.json"

if ($hasWebsite) {
    $siteVersion = Read-RepoText "sura_presentation/src/version.js"
    Add-Check "website version module" ($siteVersion -match ('export const VERSION\s*=\s*"' + [regex]::Escape($version) + '"')) "website version module must match version.json"
    $siteMain = Read-RepoText "sura_presentation/src/main.jsx"
    Add-Check "website version consumer" ($siteMain.Contains('import { VERSION } from "./version.js";') -and $siteMain.Contains('import release from "./release.json";') -and $siteMain -notmatch 'const VERSION\s*=') "website must consume generated version and release metadata"
    $siteMetadata = (Read-RepoText "sura_presentation/index.html") + (Read-RepoText "sura_presentation/app/layout.jsx")
    Add-Check "website evergreen metadata" ($siteMetadata -notmatch 'Sura Language \d+\.\d+\.\d+ 공식') "static SEO text must not embed a release version"
}

$referenceGenerator = Read-RepoText "tools/sura_reference_generate.mjs"
Add-Check "reference version source" ($referenceGenerator.Contains('path.join(root, "version.json")') -and $referenceGenerator -notmatch 'const expectedVersion\s*=\s*"\d') "reference generator must read version.json"
$installerMaker = Read-RepoText "tools/sura_make_installer.ps1"
Add-Check "installer version source" ($installerMaker.Contains('Join-Path $root "version.json"') -and $installerMaker -match '\[string\]\$Version\s*=\s*""') "installer must default to version.json"
$installerSmoke = Read-RepoText "tools/sura_installer_smoke.ps1"
Add-Check "installer test version source" ($installerSmoke.Contains('Join-Path $root "version.json"') -and $installerSmoke -match '\[string\]\$Version\s*=\s*""') "installer smoke must default to version.json"
$storeMaker = Read-RepoText "tools/sura_store_msix.ps1"
Add-Check "Store MSIX version source" ($storeMaker.Contains('Join-Path $root "version.json"') -and $storeMaker.Contains('([string]$versionContract.version) + ".0"') -and $storeMaker -match '\[string\]\$Version\s*=\s*""') "Store MSIX builder must default to version.json plus a zero revision"

$starterExamples = @("examples/starter/05_collections.sura")
if ($hasWebsite) { $starterExamples += "sura_presentation/public/examples/starter/05_collections.sura" }
foreach ($relative in $starterExamples) {
    Add-Check "starter example: $relative" ((Read-RepoText $relative) -match ('version:\s*"' + [regex]::Escape($version) + '"')) "$relative must match version.json"
}

$releasePath = "sura_presentation/public/downloads/release-$version.json"
$verificationPath = "sura_presentation/public/downloads/verification-$version.json"
if ($hasWebsite -and -not $SkipPublicRelease) {
foreach ($item in @(
    @("public release manifest", $releasePath, "sura.public.release.v1"),
    @("public verification manifest", $verificationPath, "sura.public.verification.v1")
)) {
    $text = Read-RepoText ([string]$item[1])
    $ok = -not [string]::IsNullOrWhiteSpace($text)
    if ($ok) {
        $manifest = $text | ConvertFrom-Json
        $ok = ([string]$manifest.schema -eq [string]$item[2] -and [string]$manifest.version -eq $version)
    }
    Add-Check ([string]$item[0]) $ok ([string]$item[1] + " must exist and match version.json")
}

$siteReleaseText = Read-RepoText "sura_presentation/src/release.json"
$publicReleaseText = Read-RepoText $releasePath
$siteReleaseOk = -not [string]::IsNullOrWhiteSpace($siteReleaseText) -and -not [string]::IsNullOrWhiteSpace($publicReleaseText)
if ($siteReleaseOk) {
    $siteRelease = $siteReleaseText | ConvertFrom-Json
    $publicRelease = $publicReleaseText | ConvertFrom-Json
    $siteArtifacts = @($siteRelease.artifacts)
    $publicArtifacts = @($publicRelease.artifacts)
    $siteReleaseOk = ($siteRelease.schema -eq "sura.site.release.v1" -and
        $siteRelease.version -eq $version -and
        $siteRelease.store.product_id -eq "9P5JFKSWTP0P" -and
        $siteRelease.store.url -eq "https://apps.microsoft.com/detail/9P5JFKSWTP0P" -and
        $siteRelease.store.availability -eq "public" -and
        $siteRelease.store.product_id -eq $publicRelease.store.product_id -and
        $siteRelease.store.url -eq $publicRelease.store.url -and
        $siteArtifacts.Count -eq $publicArtifacts.Count)
    for ($index = 0; $siteReleaseOk -and $index -lt $siteArtifacts.Count; $index++) {
        $siteReleaseOk = ($siteArtifacts[$index].name -eq $publicArtifacts[$index].name -and
            [int64]$siteArtifacts[$index].bytes -eq [int64]$publicArtifacts[$index].bytes -and
            [string]$siteArtifacts[$index].sha256 -eq [string]$publicArtifacts[$index].sha256)
        $artifactPath = Join-Path $root ("sura_presentation/public/downloads/" + [string]$siteArtifacts[$index].name)
        if ($siteReleaseOk) {
            $siteReleaseOk = (Test-Path -LiteralPath $artifactPath -PathType Leaf) -and
                [int64](Get-Item -LiteralPath $artifactPath).Length -eq [int64]$siteArtifacts[$index].bytes -and
                (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant() -eq ([string]$siteArtifacts[$index].sha256).ToLowerInvariant()
        }
    }
}
Add-Check "website release metadata" $siteReleaseOk "website release.json, public manifest, and downloadable file hashes must agree"
}

if (-not $SkipEngine) {
    $enginePath = Join-Path $root "SuraLanguage.exe"
    $engineOk = Test-Path -LiteralPath $enginePath
    $engineOutput = "engine not built"
    if ($engineOk) {
        $engineOutput = (& $enginePath --version 2>&1 | Out-String).Trim()
        $engineOk = ($LASTEXITCODE -eq 0 -and $engineOutput -eq "Sura Language $version")
    }
    Add-Check "built engine" $engineOk "expected Sura Language $version; got $engineOutput"
}

$failed = @($checks | Where-Object { -not $_.passed })
$checkArray = @()
foreach ($check in $checks) { $checkArray += $check }
$reportStatus = if ($failed.Count -eq 0) { "pass" } else { "fail" }
$report = [ordered]@{
    schema = "sura.version.contract.report.v1"
    checked_utc = [DateTime]::UtcNow.ToString("o")
    version = $version
    series = $series
    applied = [bool]$Apply
    status = $reportStatus
    checks = $checkArray
}

if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $jsonPath = [System.IO.Path]::GetFullPath($JsonOut)
    $parent = Split-Path -Parent $jsonPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 6), $utf8NoBom)
}

if ($failed.Count -gt 0) {
    foreach ($failure in $failed) { Write-Output ("FAIL: {0}: {1}" -f $failure.name, $failure.detail) }
    throw "version contract failed: $($failed.Count) check(s)"
}

"sura_version_sync: PASS ($version, $($checks.Count) checks)"
