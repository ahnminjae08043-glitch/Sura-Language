param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string[]]$Files = @(),
    [string]$ReleaseManifest = "",
    [switch]$RequireSigned,
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$version = [string](([System.IO.File]::ReadAllText((Join-Path $root "version.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json).version)

# The published installer and its release manifest live with the website, in
# its own repository. When this tree does not carry them there is nothing to
# audit beyond the executables built here, so they are included only when
# present. An explicitly passed -ReleaseManifest is still required to exist.
# Whether the installer is *expected* follows the same rule as
# sura_version_sync.ps1: only while this repository still tracks the site, so
# an untracked local copy cannot resurrect a contract the tree no longer owns.
$installerExpected = $false
if (Test-Path -LiteralPath (Join-Path $root "sura_presentation") -PathType Container) {
    $installerExpected = $true
    if (Get-Command git -CommandType Application -ErrorAction SilentlyContinue) {
        $tracked = & git -C $root ls-files "sura_presentation/package.json"
        if ($LASTEXITCODE -eq 0) { $installerExpected = -not [string]::IsNullOrWhiteSpace(($tracked | Out-String)) }
    }
}
$installer = Join-Path $root "sura_presentation/public/downloads/SuraLanguageSetup-$version.exe"
$manifestRequired = -not [string]::IsNullOrWhiteSpace($ReleaseManifest)
if ($Files.Count -eq 0) {
    $Files = @(
        (Join-Path $root "SuraLanguage.exe"),
        (Join-Path $root "surapkg.exe")
    )
    if ($installerExpected -or (Test-Path -LiteralPath $installer -PathType Leaf)) { $Files += $installer }
}
if (-not $manifestRequired) {
    $defaultManifest = Join-Path $root "sura_presentation/public/downloads/release-$version.json"
    if (Test-Path -LiteralPath $defaultManifest -PathType Leaf) { $ReleaseManifest = $defaultManifest }
}

$results = @()
$failures = @()
foreach ($file in $Files) {
    $fullPath = if ([System.IO.Path]::IsPathRooted($file)) { $file } else { Join-Path $root $file }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        $failures += "file not found: $fullPath"
        continue
    }
    $fullPath = (Resolve-Path -LiteralPath $fullPath).Path
    $signature = Get-AuthenticodeSignature -LiteralPath $fullPath
    $status = [string]$signature.Status
    $signed = $status -ne "NotSigned"
    $valid = $status -eq "Valid"
    if ($signed -and -not $valid) {
        $failures += "invalid Authenticode signature: $fullPath ($status)"
    }
    if ($RequireSigned -and -not $valid) {
        $failures += "trusted Authenticode signature required: $fullPath ($status)"
    }
    $results += [pscustomobject]@{
        file = $fullPath
        bytes = [int64](Get-Item -LiteralPath $fullPath).Length
        sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        status = $status
        signed = $signed
        valid = $valid
        signer_subject = $(if ($null -ne $signature.SignerCertificate) { [string]$signature.SignerCertificate.Subject } else { "" })
        signer_thumbprint = $(if ($null -ne $signature.SignerCertificate) { [string]$signature.SignerCertificate.Thumbprint } else { "" })
        timestamp_subject = $(if ($null -ne $signature.TimeStamperCertificate) { [string]$signature.TimeStamperCertificate.Subject } else { "" })
    }
}

$validCount = @($results | Where-Object { $_.valid }).Count
$unsignedCount = @($results | Where-Object { -not $_.signed }).Count
$manifestSigning = ""
if ([string]::IsNullOrWhiteSpace($ReleaseManifest)) {
    # No published manifest in this tree: the executable audit above stands on
    # its own, and the release pipeline checks the manifest where it lives.
    $manifestSigning = "none"
} elseif (Test-Path -LiteralPath $ReleaseManifest -PathType Leaf) {
    $manifest = [System.IO.File]::ReadAllText($ReleaseManifest, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ([string]$manifest.version -ne $version) {
        $failures += "release manifest version does not match version.json: $ReleaseManifest"
    }
    $manifestSigning = [string]$manifest.signing.authenticode
    $observedSigning = if ($results.Count -gt 0 -and $validCount -eq $results.Count) { "Valid" } elseif ($unsignedCount -gt 0) { "NotSigned" } else { "Invalid" }
    if ($manifestSigning -ne $observedSigning) {
        $failures += "release manifest says Authenticode=$manifestSigning but observed $observedSigning"
    }
} else {
    $failures += "release manifest not found: $ReleaseManifest"
}

$report = [ordered]@{
    schema = "sura.windows.signature.report.v1"
    checked_utc = [DateTime]::UtcNow.ToString("o")
    version = $version
    require_signed = [bool]$RequireSigned
    status = $(if ($failures.Count -eq 0) { "pass" } else { "fail" })
    valid_count = $validCount
    unsigned_count = $unsignedCount
    direct_download_warning_expected = ($unsignedCount -gt 0)
    installer_expected = [bool]$installerExpected
    installer_audited = [bool](@($results | Where-Object { $_.file -eq $installer }).Count -gt 0)
    release_manifest = $ReleaseManifest
    release_manifest_authenticode = $manifestSigning
    files = $results
    next_action = $(if ($unsignedCount -gt 0) { "Publish the Store-certified MSIX, or sign every direct-download executable with a publicly trusted code-signing certificate and timestamp it." } else { "Keep signature verification in the release gate." })
    failures = $failures
}

if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $jsonPath = [System.IO.Path]::GetFullPath($JsonOut)
    $parent = Split-Path -Parent $jsonPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 7), $utf8NoBom)
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    throw "Windows signature gate failed: $($failures.Count) issue(s)"
}

"sura_windows_signature_gate: PASS (valid=$validCount, unsigned=$unsignedCount, warning_expected=$($report.direct_download_warning_expected))"
