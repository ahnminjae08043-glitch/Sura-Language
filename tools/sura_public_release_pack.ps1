param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Version = "",
    [string]$DistDir = "",
    [string]$PublicDir = "",
    [string]$PublishedDate = ([DateTime]::UtcNow.ToString("yyyy-MM-dd")),
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$contract = [System.IO.File]::ReadAllText((Join-Path $root "version.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
if ($contract.schema -ne "sura.version.v1" -or [string]$contract.version -notmatch '^\d+\.\d+\.\d+$') {
    throw "version.json does not satisfy sura.version.v1"
}
if ([string]::IsNullOrWhiteSpace($Version)) { $Version = [string]$contract.version }
if ($Version -ne [string]$contract.version) { throw "release version $Version does not match version.json $($contract.version)" }
if ([string]::IsNullOrWhiteSpace($DistDir)) { $DistDir = Join-Path $root "dist" }
if ([string]::IsNullOrWhiteSpace($PublicDir)) { $PublicDir = Join-Path $root "sura_presentation/public/downloads" }
$dist = [System.IO.Path]::GetFullPath($DistDir)
$public = [System.IO.Path]::GetFullPath($PublicDir)
New-Item -ItemType Directory -Force -Path $public | Out-Null

$engine = Join-Path $root "SuraLanguage.exe"
$surapkg = Join-Path $root "surapkg.exe"
$engineVersion = (& $engine --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $engineVersion -ne "Sura Language $Version") {
    throw "built engine does not match release version: $engineVersion"
}

$specs = @(
    [ordered]@{ name = "SuraLanguageSetup-$Version.exe"; source = Join-Path $dist "SuraLanguageSetup-$Version.exe" },
    [ordered]@{ name = "SuraLanguage-$Version-windows-x64.zip"; source = Join-Path $dist "SuraLanguage-$Version-windows-x64.zip" },
    [ordered]@{ name = "SuraLanguage-VSCode-$Version.vsix"; source = Join-Path $dist "SuraLanguage-VSCode-$Version.vsix" }
)
$artifacts = @()
foreach ($spec in $specs) {
    if (-not (Test-Path -LiteralPath $spec.source -PathType Leaf)) { throw "release artifact not found: $($spec.source)" }
    $item = Get-Item -LiteralPath $spec.source
    $artifacts += [ordered]@{
        name = $spec.name
        bytes = [int64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$setupSignature = Get-AuthenticodeSignature -LiteralPath $specs[0].source
$authenticode = if ([string]$setupSignature.Status -eq "Valid") { "Valid" } elseif ([string]$setupSignature.Status -eq "NotSigned") { "NotSigned" } else { [string]$setupSignature.Status }
$vscodePackage = [System.IO.File]::ReadAllText((Join-Path $root "sura-vscode/package.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$licenseFile = @("LICENSE", "LICENSE.md", "LICENSE.txt") | Where-Object { Test-Path -LiteralPath (Join-Path $root $_) -PathType Leaf } | Select-Object -First 1
$installerManifestPath = Join-Path $dist "SuraLanguage-windows-x64/installer-manifest.json"
if (-not (Test-Path -LiteralPath $installerManifestPath)) { throw "installer manifest not found: $installerManifestPath" }
$installerManifest = [System.IO.File]::ReadAllText($installerManifestPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
if ([string]$installerManifest.version -ne $Version) { throw "installer manifest version does not match release" }

$release = [ordered]@{
    schema = "sura.public.release.v1"
    product = "Sura Language"
    version = $Version
    published = $PublishedDate
    platform = "windows-x64"
    store = [ordered]@{
        product_id = "9P5JFKSWTP0P"
        url = "https://apps.microsoft.com/detail/9P5JFKSWTP0P"
        publisher = "SuraTeam"
        availability = "public"
        pricing = "Free"
    }
    starter_pack = [ordered]@{
        command = "surapkg new <name>"
        generated = @("package manifest", "source", "test", "README", "VS Code settings")
        beginner_examples = @(Get-ChildItem -LiteralPath (Join-Path $root "examples/starter") -Filter "*.sura" -File).Count
    }
    runtime = [ordered]@{
        engine = "register VM"
        optional_jit = [ordered]@{
            flag = "--jit"
            platform_abi = "Windows x64 / Win64"
            mode = "lazy partial compilation inside the register VM"
            fallback = "unsupported bodies remain in the register VM"
        }
        diagnostics_default = "English"
        diagnostics_korean = "--lang ko or SURA_LANG=ko"
    }
    installer_requirements = @($installerManifest.normal_use_dependencies + $installerManifest.installer_dependencies | Select-Object -Unique)
    optional_integrations = [ordered]@{
        ffmpeg = "local media and video decoding"
        curl = "native HTTP, async HTTP, and HTTP-backed LLM requests"
        nodejs = "JavaScript target, VS Code tooling, http.serve_routes, and preferred http.serve_static runtime"
        python = "Python bridge and http.serve_static fallback"
        c_cpp_toolchain = "source build, embedding, FFI bindings, and plugins"
        cuda_driver_and_optional_cublas = "CUDA tensor operations"
    }
    signing = [ordered]@{
        authenticode = $authenticode
        vsix_signature = "Not provided"
    }
    license = [ordered]@{
        root_license_file = $(if ($licenseFile) { [string]$licenseFile } else { "Not provided" })
        vscode_package_field = [string]$vscodePackage.license
    }
    artifacts = $artifacts
}

$siteRelease = [ordered]@{
    schema = "sura.site.release.v1"
    version = $Version
    store = $release.store
    artifacts = $artifacts
}

$stage = Join-Path $public (".release-stage-" + $PID)
try {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    foreach ($spec in $specs) { Copy-Item -LiteralPath $spec.source -Destination (Join-Path $stage $spec.name) -Force }
    [System.IO.File]::WriteAllText((Join-Path $stage "release-$Version.json"), ($release | ConvertTo-Json -Depth 8), $utf8NoBom)
    [System.IO.File]::WriteAllText((Join-Path $stage "SHA256SUMS.txt"), (($artifacts | ForEach-Object { "$($_.sha256)  $($_.name)" }) -join "`n") + "`n", $utf8NoBom)
    $readmeLines = @(
        "Sura Language public downloads",
        "Version: $Version",
        "Published: $PublishedDate",
        "",
        "Authenticode: $authenticode",
        "The direct-download warning is expected when Authenticode is NotSigned.",
        "HTTPS protects transport but does not create a Windows publisher signature.",
        "",
        "Files"
    )
    foreach ($artifact in $artifacts) { $readmeLines += "$($artifact.name) | $($artifact.bytes) bytes | $($artifact.sha256)" }
    $readmeLines += @(
        "",
        "Verify in PowerShell:",
        "Get-FileHash .\SuraLanguageSetup-$Version.exe -Algorithm SHA256"
    )
    [System.IO.File]::WriteAllText((Join-Path $stage "README-KO.txt"), ($readmeLines -join "`n") + "`n", $utf8NoBom)

    foreach ($file in Get-ChildItem -LiteralPath $stage -File) {
        Move-Item -LiteralPath $file.FullName -Destination (Join-Path $public $file.Name) -Force
    }
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}

[System.IO.File]::WriteAllText((Join-Path $root "sura_presentation/src/release.json"), ($siteRelease | ConvertTo-Json -Depth 6), $utf8NoBom)
foreach ($artifact in $artifacts) {
    $copied = Join-Path $public $artifact.name
    if ([int64](Get-Item -LiteralPath $copied).Length -ne [int64]$artifact.bytes -or
        (Get-FileHash -LiteralPath $copied -Algorithm SHA256).Hash.ToLowerInvariant() -ne [string]$artifact.sha256) {
        throw "published artifact verification failed: $copied"
    }
}

$report = [ordered]@{
    schema = "sura.public.release.pack.v1"
    generated_utc = [DateTime]::UtcNow.ToString("o")
    version = $Version
    public_dir = $public
    authenticode = $authenticode
    warning_expected = ($authenticode -ne "Valid")
    artifacts = $artifacts
}
if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $jsonPath = [System.IO.Path]::GetFullPath($JsonOut)
    $parent = Split-Path -Parent $jsonPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 7), $utf8NoBom)
}

"sura_public_release_pack: PASS ($Version, Authenticode=$authenticode)"
