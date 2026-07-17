param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ExtensionsRoot = "",
    [string]$BackupRoot = "",
    [switch]$NoBuild,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

function Resolve-FullPath {
    param([string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-PathInside {
    param([string]$Path, [string]$Root, [string]$Label)

    $fullPath = Resolve-FullPath $Path
    $fullRoot = (Resolve-FullPath $Root).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    if ($fullPath -ne $fullRoot -and -not $fullPath.StartsWith($fullRoot + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label path is outside the expected root: $fullPath"
    }
    return $fullPath
}

function Copy-ExtensionItem {
    param([string]$Source, [string]$Destination, [switch]$Directory)

    if ($DryRun) {
        Write-Host "DRYRUN copy $Source -> $Destination"
        return
    }
    if ($Directory) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
    } else {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
}

$root = Resolve-FullPath $RepoRoot
$extensionSource = Join-Path $root "sura-vscode"
$packagePath = Join-Path $extensionSource "package.json"
if (-not (Test-Path -LiteralPath $packagePath)) {
    throw "Sura VS Code package.json not found: $packagePath"
}

if ([string]::IsNullOrWhiteSpace($ExtensionsRoot)) {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) { throw "USERPROFILE is not set; pass -ExtensionsRoot" }
    $ExtensionsRoot = Join-Path $env:USERPROFILE ".vscode\extensions"
}
if ([string]::IsNullOrWhiteSpace($BackupRoot)) {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) { throw "USERPROFILE is not set; pass -BackupRoot" }
    $BackupRoot = Join-Path $env:USERPROFILE ".vscode\sura-extension-backups"
}

$extensionsFull = Resolve-FullPath $ExtensionsRoot
$backupFull = Resolve-FullPath $BackupRoot
New-Item -ItemType Directory -Force -Path $extensionsFull | Out-Null
New-Item -ItemType Directory -Force -Path $backupFull | Out-Null

$pkg = [System.IO.File]::ReadAllText($packagePath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$publisher = [string]$pkg.publisher
$name = [string]$pkg.name
$version = [string]$pkg.version
if ([string]::IsNullOrWhiteSpace($publisher) -or [string]::IsNullOrWhiteSpace($name) -or [string]::IsNullOrWhiteSpace($version)) {
    throw "Sura VS Code package must define publisher, name, and version"
}
if ($publisher -eq "user") {
    throw "Refusing to install a VS Code extension whose publisher is still 'user'"
}
if ($publisher -ne "sura-team" -or $name -ne "sura-language") {
    throw "Refusing to install an unexpected VS Code extension identity: $publisher.$name"
}

if (-not $NoBuild) {
    $npm = Get-Command npm -ErrorAction SilentlyContinue
    if (-not $npm) {
        throw "npm is required to rebuild sura-vscode; rerun with -NoBuild only if out/ is already current"
    }
    Push-Location $extensionSource
    try {
        & $npm.Source run esbuild
        if ($LASTEXITCODE -ne 0) { throw "npm run esbuild failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
}

foreach ($required in @("out\extension.js", "out\debugAdapter.js", "syntaxes\sura.tmLanguage.json", "language-configuration.json", "assets\icon.png")) {
    $requiredPath = Join-Path $extensionSource $required
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Sura VS Code extension build output is missing: $required"
    }
}

$officialId = "$publisher.$name"
$officialDirName = "$officialId-$version"
$officialTarget = Assert-PathInside (Join-Path $extensionsFull $officialDirName) $extensionsFull "official extension"
$legacyStamp = [DateTime]::UtcNow.ToString("yyyyMMddHHmmss")
$legacyBackupDir = Join-Path $backupFull $legacyStamp
$movedLegacy = New-Object System.Collections.Generic.List[string]

$legacyDirs = @()
if (Test-Path -LiteralPath $extensionsFull) {
    $legacyDirs = Get-ChildItem -LiteralPath $extensionsFull -Directory | Where-Object {
        if ($_.Name -eq $officialDirName) { return $false }
        if ($_.Name -like "$officialId-*") { return $true }
        if ($_.Name -eq "sura" -or $_.Name -like "user.sura-*") { return $true }
        $candidatePackage = Join-Path $_.FullName "package.json"
        if (-not (Test-Path -LiteralPath $candidatePackage)) { return $false }
        try {
            $candidate = [System.IO.File]::ReadAllText($candidatePackage, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
            return ([string]$candidate.publisher -eq "user" -and @($candidate.contributes.languages | ForEach-Object { [string]$_.id }) -contains "sura")
        } catch {
            return $false
        }
    }
}

foreach ($dir in @($legacyDirs)) {
    $source = Assert-PathInside $dir.FullName $extensionsFull "legacy extension"
    $destName = ($dir.Name -replace '[^\w.\-]', '_')
    $dest = Join-Path $legacyBackupDir $destName
    if ($DryRun) {
        Write-Host "DRYRUN move legacy $source -> $dest"
    } else {
        New-Item -ItemType Directory -Force -Path $legacyBackupDir | Out-Null
        Move-Item -LiteralPath $source -Destination $dest -Force
    }
    $movedLegacy.Add($dir.Name) | Out-Null
}

if (Test-Path -LiteralPath $officialTarget) {
    $checkedTarget = Assert-PathInside $officialTarget $extensionsFull "official extension"
    if ($DryRun) {
        Write-Host "DRYRUN remove existing $checkedTarget"
    } else {
        Remove-Item -LiteralPath $checkedTarget -Recurse -Force
    }
}

if (-not $DryRun) {
    New-Item -ItemType Directory -Force -Path $officialTarget | Out-Null
}

foreach ($file in @("package.json", "README.md", "language-configuration.json")) {
    Copy-ExtensionItem (Join-Path $extensionSource $file) (Join-Path $officialTarget $file)
}
foreach ($dir in @("assets", "out", "syntaxes")) {
    Copy-ExtensionItem (Join-Path $extensionSource $dir) (Join-Path $officialTarget $dir) -Directory
}

$report = [ordered]@{
    schema = "sura.vscode.install.v1"
    installed = (-not $DryRun)
    extension_id = $officialId
    version = $version
    target = $officialTarget
    moved_legacy = @($movedLegacy.ToArray())
    backup_dir = if ($movedLegacy.Count -gt 0) { $legacyBackupDir } else { "" }
    next_action = "Reload VS Code: Developer: Reload Window"
}

$report | ConvertTo-Json -Depth 5
