param(
    [string]$RepoRoot = ".",
    [string]$Surapkg = "",
    [string]$ContractPath = "",
    [string]$Out = "",
    [switch]$Check
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$root = (Resolve-Path -LiteralPath $RepoRoot).Path

function Resolve-FromRoot([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $root $Path
}

function Read-Utf8([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "required file not found: $Path" }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Resolve-Tool([string]$ExplicitPath, [string]$WindowsName, [string]$UnixName) {
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }
    $leaf = if ($env:OS -eq "Windows_NT") { $WindowsName } else { $UnixName }
    $candidate = Join-Path $root $leaf
    if (-not (Test-Path -LiteralPath $candidate)) {
        throw "$UnixName was not found; build it or pass -Surapkg"
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

$contractFile = if ([string]::IsNullOrWhiteSpace($ContractPath)) {
    Join-Path $root "compatibility.json"
} else {
    Resolve-FromRoot $ContractPath
}
$contract = (Read-Utf8 $contractFile) | ConvertFrom-Json
if ($contract.schema -ne "sura.compatibility.v1") { throw "invalid compatibility schema" }

$moduleNames = @($contract.stable_api.modules | ForEach-Object { [string]$_ })
if ($moduleNames.Count -eq 0) { throw "stable API module list is empty" }
if (@($moduleNames | Sort-Object -Unique).Count -ne $moduleNames.Count) {
    throw "stable API module list contains duplicates"
}

if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = [string]$contract.stable_api.snapshot
}
if ([string]::IsNullOrWhiteSpace($Out)) { throw "stable API snapshot output path is empty" }
$outPath = Resolve-FromRoot $Out
$surapkgPath = Resolve-Tool $Surapkg "surapkg.exe" "surapkg"
$fixture = Join-Path $root "tests/compat/api_fixture"
if (-not (Test-Path -LiteralPath (Join-Path $fixture "sura.pkg.json"))) {
    throw "stable API fixture package is missing"
}

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
$temp = Join-Path $tempRoot ("sura_compat_api_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        Push-Location $fixture
        try {
            $commandOutput = (& $surapkgPath docs $temp 2>&1 | ForEach-Object { "$_" }) -join "`n"
            $exitCode = $LASTEXITCODE
        } finally {
            Pop-Location
        }
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($exitCode -ne 0) { throw "surapkg docs failed: $commandOutput" }

    $apiPath = Join-Path $temp "api.json"
    $api = (Read-Utf8 $apiPath) | ConvertFrom-Json
    $moduleRecords = New-Object System.Collections.Generic.List[object]
    foreach ($moduleName in $moduleNames) {
        $matches = @($api.stdlibModules | Where-Object { [string]$_.name -eq $moduleName })
        if ($matches.Count -ne 1) {
            throw "expected exactly one stdlib module in generated docs: $moduleName"
        }
        $symbols = @($matches[0].symbols | Sort-Object @{ Expression = { [string]$_.name } }, @{ Expression = { [string]$_.signature } } | ForEach-Object {
            [ordered]@{
                kind = [string]$_.kind
                name = [string]$_.name
                signature = [string]$_.signature
            }
        })
        if ($symbols.Count -eq 0) { throw "stable API module has no symbols: $moduleName" }
        $moduleRecords.Add([ordered]@{
            name = $moduleName
            symbols = $symbols
        })
    }

    $snapshot = [ordered]@{
        schema = "sura.stable_api.v1"
        series = [string]$contract.stable_series
        baseline_version = [string]$contract.source.guarantee_starts_at
        modules = @($moduleRecords | ForEach-Object { $_ })
    }
    $json = ($snapshot | ConvertTo-Json -Depth 10) + "`n"
    $encoding = [System.Text.UTF8Encoding]::new($false)

    if ($Check) {
        $existing = Read-Utf8 $outPath
        if ($existing -ne $json) {
            throw "stable API snapshot is not canonical or does not match generated docs: $outPath"
        }
        $symbolCount = @($moduleRecords | ForEach-Object { @($_.symbols).Count } | Measure-Object -Sum).Sum
        Write-Host "sura_compatibility_api_snapshot: PASS ($($moduleRecords.Count) modules, $symbolCount signatures)"
    } else {
        $parent = Split-Path -Parent $outPath
        if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        [System.IO.File]::WriteAllText($outPath, $json, $encoding)
        $symbolCount = @($moduleRecords | ForEach-Object { @($_.symbols).Count } | Measure-Object -Sum).Sum
        Write-Host "sura_compatibility_api_snapshot: WROTE $outPath ($($moduleRecords.Count) modules, $symbolCount signatures)"
    }
} finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    $leaf = [System.IO.Path]::GetFileName($resolvedTemp)
    $parent = [System.IO.Path]::GetDirectoryName($resolvedTemp).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    if ($parent -eq $tempRoot -and $leaf -match '^sura_compat_api_[0-9a-f]{32}$' -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
