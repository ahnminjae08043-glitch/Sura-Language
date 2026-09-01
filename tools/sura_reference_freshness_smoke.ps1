param(
    [string]$RepoRoot = ".",
    [string]$Surapkg = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$root = (Resolve-Path -LiteralPath $RepoRoot).Path

function Read-Utf8Strict {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw "required file not found: $Path" }
    return $utf8Strict.GetString([System.IO.File]::ReadAllBytes($Path))
}

function Get-Sha256 {
    param([byte[]]$Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Project-ApiModules {
    param($Modules)
    $projected = @()
    foreach ($module in @($Modules | Sort-Object -Property name)) {
        $symbols = @()
        foreach ($symbol in @($module.symbols | Sort-Object -Property line, name)) {
            $symbols += [ordered]@{
                kind = [string]$symbol.kind
                name = [string]$symbol.name
                signature = [string]$symbol.signature
                source = [string]$symbol.source
                line = [int]$symbol.line
            }
        }
        $projected += [ordered]@{
            name = [string]$module.name
            symbol_count = [int]$module.symbol_count
            symbols = $symbols
        }
    }
    return $projected
}

function Normalize-Json {
    param($Value, [int]$Depth = 64)
    return ($Value | ConvertTo-Json -Depth $Depth -Compress)
}

if ([string]::IsNullOrWhiteSpace($Surapkg)) {
    $suffix = if ($IsWindows -or $env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $Surapkg = Join-Path $root ("surapkg" + $suffix)
}
$surapkgPath = (Resolve-Path -LiteralPath $Surapkg).Path

$rootReferencePath = Join-Path $root "reference.html"
$rootBytes = [System.IO.File]::ReadAllBytes($rootReferencePath)
# The public website tree is not part of the language-core repository. When a
# website checkout is present alongside, its copy must stay byte-identical.
$publicReferencePath = Join-Path $root "sura_presentation/public/reference.html"
if (Test-Path -LiteralPath $publicReferencePath) {
    $publicBytes = [System.IO.File]::ReadAllBytes($publicReferencePath)
    if ((Get-Sha256 $rootBytes) -ne (Get-Sha256 $publicBytes)) {
        throw "root and website reference files differ"
    }
}

$referenceText = $utf8Strict.GetString($rootBytes)
$machineMatch = [regex]::Match(
    $referenceText,
    '<script id=[''\"]sura-reference-data[''\"][^>]*>(?<json>[\s\S]*?)</script>'
)
if (-not $machineMatch.Success) { throw "reference machine-readable JSON block is missing" }
$machine = $machineMatch.Groups["json"].Value | ConvertFrom-Json

$versionContract = (Read-Utf8Strict (Join-Path $root "version.json")) | ConvertFrom-Json
$compatibilityContract = (Read-Utf8Strict (Join-Path $root "compatibility.json")) | ConvertFrom-Json
if ($machine.schema -ne "sura.public.reference.v1" -or $machine.version -ne $versionContract.version) {
    throw "reference schema or version is stale"
}
if ((Normalize-Json $machine.compatibility) -ne (Normalize-Json $compatibilityContract)) {
    throw "reference compatibility contract is stale"
}

$requiredFacts = @(
    "tokenizer.train_bpe",
    "autograd.run_onnx",
    "arm64-aapcs-baseline"
)
foreach ($fact in $requiredFacts) {
    if (-not $referenceText.Contains($fact)) { throw "reference fact is missing: $fact" }
}
if ($referenceText.Contains("ONNX graph execution, BPE")) {
    throw "reference still claims that bounded BPE and ONNX execution are unimplemented"
}
if (-not $machine.tokenizer.bpe -or
    $machine.tokenizer.external_tokenizer_format_compatibility -ne $false -or
    $machine.onnx_execution.api -ne "autograd.run_onnx(path, inputs, [options])" -or
    $machine.onnx_execution.device -ne "cpu" -or
    @($machine.onnx_execution.supported_operators) -notcontains "Transpose" -or
    @($machine.onnx_execution.supported_operators) -notcontains "Flatten" -or
    @($machine.onnx_execution.supported_operators) -notcontains "Reshape" -or
    -not $machine.onnx_execution.reshape_shape_initializer -or
    @($machine.onnx_execution.rejected_or_unsupported) -notcontains "Conv" -or
    @($machine.onnx_execution.rejected_or_unsupported) -notcontains "dynamic shapes") {
    throw "reference BPE or bounded ONNX capability/limitation contract is stale"
}

$tempParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/')
$temp = Join-Path $tempParent ("sura_reference_freshness_" + [Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path (Join-Path $temp "src") | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $temp "sura.pkg.json"),
        "{`n  `"name`": `"reference_probe`",`n  `"version`": `"0.0.0`",`n  `"main`": `"src/main.sura`",`n  `"dependencies`": {}`n}`n",
        $utf8NoBom
    )
    [System.IO.File]::WriteAllText((Join-Path $temp "src/main.sura"), "print `"reference probe`"`n", $utf8NoBom)

    Push-Location $temp
    try {
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $commandOutput = & $surapkgPath docs docs 2>&1 | ForEach-Object { "$_" }
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = $previousPreference
        if ($exitCode -ne 0) {
            throw "surapkg docs failed while checking reference freshness: $($commandOutput -join "`n")"
        }
    }
    finally {
        Pop-Location
    }

    $currentApi = (Read-Utf8Strict (Join-Path $temp "docs/api.json")) | ConvertFrom-Json
    $currentModules = Project-ApiModules $currentApi.stdlibModules
    $publishedModules = Project-ApiModules $machine.stdlib.api_modules
    if ((Normalize-Json $currentModules) -ne (Normalize-Json $publishedModules)) {
        $currentCount = @($currentModules | ForEach-Object { $_.symbol_count } | Measure-Object -Sum).Sum
        $publishedCount = @($publishedModules | ForEach-Object { $_.symbol_count } | Measure-Object -Sum).Sum
        throw "reference stdlib API is stale (runtime=$currentCount signatures, published=$publishedCount signatures)"
    }

    $signatureCount = @($currentModules | ForEach-Object { $_.symbol_count } | Measure-Object -Sum).Sum
    if ([int]$machine.stdlib.api_signature_count -ne [int]$signatureCount -or
        ($null -ne $machine.api_signature_count -and
         [int]$machine.api_signature_count -ne [int]$signatureCount)) {
        throw "reference API signature totals are stale"
    }

    $tokenizer = @($publishedModules | Where-Object { $_.name -eq "tokenizer" })
    $autograd = @($publishedModules | Where-Object { $_.name -eq "autograd" })
    if ($tokenizer.Count -ne 1 -or @($tokenizer[0].symbols | Where-Object { $_.name -eq "train_bpe" }).Count -ne 1) {
        throw "published tokenizer.train_bpe signature is missing"
    }
    if ($autograd.Count -ne 1 -or @($autograd[0].symbols | Where-Object { $_.name -eq "run_onnx" }).Count -ne 1) {
        throw "published autograd.run_onnx signature is missing"
    }

    Write-Host "sura_reference_freshness_smoke: PASS ($signatureCount signatures)"
}
finally {
    $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
    $parent = [System.IO.Path]::GetFullPath((Split-Path -Parent $resolvedTemp)).TrimEnd('\', '/')
    $leaf = Split-Path -Leaf $resolvedTemp
    if ($parent -eq $tempParent -and $leaf -match '^sura_reference_freshness_[0-9a-f]{32}$' -and
        (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
