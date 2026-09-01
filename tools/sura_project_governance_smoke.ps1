param([string]$RepoRoot = ".")

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $RepoRoot).Path

function Read-Required([string]$RelativePath) {
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "required project-governance file is missing: $RelativePath"
    }
    return [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
}

$scope = Read-Required "SCOPE.md"
$contributing = Read-Required "CONTRIBUTING.md"
$compatibility = (Read-Required "compatibility.json") | ConvertFrom-Json
$bug = Read-Required ".github/ISSUE_TEMPLATE/bug_report.yml"
$feature = Read-Required ".github/ISSUE_TEMPLATE/feature_request.yml"
$package = Read-Required ".github/ISSUE_TEMPLATE/package_submission.yml"
$pullRequest = Read-Required ".github/PULL_REQUEST_TEMPLATE.md"

$scopeHeadings = [regex]::Matches($scope, '(?m)^## ').Count
if ($scopeHeadings -lt 5) { throw "SCOPE.md is missing required policy sections" }
if ($scope -notmatch "register VM fallback" -or $scope -notmatch "compatibility.json" -or
    $scope -notmatch "public syntax" -or $scope -notmatch "public reference") {
    throw "SCOPE.md must state fallback and the support-tier source"
}
if ($contributing -notmatch "result is value \* 2" -or
    $contributing -match '(?m)^\s*(let|var|const)\s+' -or
    $contributing -match '(?m)^\s*fn\s+') {
    throw "CONTRIBUTING.md does not use the current Sura grammar"
}
foreach ($command in @("build.bat portable", "run_stable_tests.ps1", "sura_compatibility_gate.ps1", "publish .\my_package --dry-run")) {
    if ($contributing -notmatch [regex]::Escape($command)) { throw "CONTRIBUTING.md is missing command: $command" }
}
foreach ($tier in @("stable", "platform_limited", "experimental")) {
    if (@($compatibility.support_tiers.$tier).Count -eq 0) { throw "empty compatibility support tier: $tier" }
}
if ($feature -notmatch "Proposed support tier" -or $feature -notmatch "Compatibility impact" -or
    $feature -notmatch "Unsupported-platform and failure behavior") {
    throw "feature request template is missing scope gates"
}
if ($bug -notmatch "Minimal reproduction" -or $package -notmatch "publish --dry-run" -or
    $pullRequest -notmatch "Compatibility and support-tier impact") {
    throw "project contribution templates are incomplete"
}

Write-Host "sura_project_governance_smoke: PASS"
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
