param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$securityPath = Join-Path $root "SECURITY.md"
$auditGuidePath = Join-Path $root "SECURITY_AUDIT.md"
$compatibilityPath = Join-Path $root "compatibility.json"
$versionPath = Join-Path $root "version.json"
$templatePath = Join-Path $root ".github/ISSUE_TEMPLATE/security_contact.yml"
$bundlePath = Join-Path $root "tools/sura_security_audit_bundle.ps1"
$bundleSmokePath = Join-Path $root "tools/sura_security_audit_bundle_smoke.ps1"

foreach ($path in @($securityPath, $auditGuidePath, $compatibilityPath, $versionPath, $templatePath, $bundlePath, $bundleSmokePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "required security-policy file is missing: $path"
    }
}

$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$security = $utf8Strict.GetString([System.IO.File]::ReadAllBytes($securityPath))
$auditGuide = $utf8Strict.GetString([System.IO.File]::ReadAllBytes($auditGuidePath))
$template = $utf8Strict.GetString([System.IO.File]::ReadAllBytes($templatePath))
$compatibility = [System.IO.File]::ReadAllText($compatibilityPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$version = [System.IO.File]::ReadAllText($versionPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json

$active = @($compatibility.support.stable_series | Where-Object { $_.status -eq "active" })
if ($active.Count -ne 1) {
    throw "security policy expects exactly one active supported series"
}
$requiredSecurityText = @(
    [string]$version.version,
    [string]$active[0].series,
    [string]$active[0].first_supported_patch,
    [string]$active[0].maintenance_not_before,
    "not an operating-system sandbox",
    "FFI and native plugins load native code into the runtime process",
    "Protected release packages provide tamper checks and practical source non-disclosure",
    "no report from an independent external security audit",
    "Report a vulnerability",
    "Private security contact request"
    "source-review handoff bundle with per-file SHA-256 and byte counts"
    "Creating or publishing that bundle is not an independent audit"
)
foreach ($needle in $requiredSecurityText) {
    if (-not $security.Contains($needle)) {
        throw "SECURITY.md is missing required factual text: $needle"
    }
}

$forbiddenClaims = @(
    "independently audited",
    "externally audited",
    "completely secure",
    "unbreakable",
    "fully sandboxed",
    "zero vulnerabilities"
)
foreach ($claim in $forbiddenClaims) {
    if ($security.IndexOf($claim, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $auditGuide.IndexOf($claim, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "public security documentation contains an unsupported assurance claim: $claim"
    }
}

foreach ($needle in @(
    "This document prepares Sura for an independent source review",
    "independent external security audit",
    "not include the engine binary, secrets, registry data, signing keys",
    "manifest.json"
)) {
    if (-not $auditGuide.Contains($needle)) {
        throw "SECURITY_AUDIT.md is missing required factual text: $needle"
    }
}

foreach ($needle in @("This issue is public", "Do not include exploit details", "affected_version", "required: true")) {
    if (-not $template.Contains($needle)) {
        throw "security contact issue template is missing required warning or field: $needle"
    }
}

Write-Host ("sura_security_policy_smoke: PASS (version {0}, active series {1}, maintenance not before {2})" -f $version.version, $active[0].series, $active[0].maintenance_not_before)
