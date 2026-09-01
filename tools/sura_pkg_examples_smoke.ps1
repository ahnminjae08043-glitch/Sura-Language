param(
    [string]$Surapkg = ".\surapkg.exe",
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$surapkgPath = (Resolve-Path -LiteralPath $Surapkg).Path
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$gallery = Join-Path $root "examples"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_examples_" + [System.Guid]::NewGuid().ToString("N"))

function Invoke-Captured {
    param([string]$FilePath, [string[]]$Arguments, [string]$WorkingDirectory)

    Push-Location $WorkingDirectory
    try {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $output = & $FilePath @Arguments 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $old
        return [pscustomobject]@{ Code = $code; Text = ($output -join "`n") }
    }
    finally {
        Pop-Location
    }
}

try {
    if (-not (Test-Path -LiteralPath $gallery -PathType Container)) {
        throw "example gallery was not found: $gallery"
    }
    $expectedFiles = @(Get-ChildItem -LiteralPath $gallery -Recurse -File -Filter *.sura | Sort-Object FullName)
    if ($expectedFiles.Count -lt 40) {
        throw "example gallery unexpectedly contains only $($expectedFiles.Count) Sura files"
    }

    $layoutBin = Join-Path $temp "install/bin"
    $layoutExamples = Join-Path $temp "install/examples"
    $work = Join-Path $temp "work"
    New-Item -ItemType Directory -Force -Path $layoutBin, $work | Out-Null
    Copy-Item -LiteralPath $surapkgPath -Destination (Join-Path $layoutBin ([System.IO.Path]::GetFileName($surapkgPath))) -Force
    Copy-Item -LiteralPath $gallery -Destination $layoutExamples -Recurse -Force
    $installedSurapkg = Join-Path $layoutBin ([System.IO.Path]::GetFileName($surapkgPath))

    # Run away from the source checkout so discovery must use the executable's
    # installed parent directory rather than ./examples in the current folder.
    $listRun = Invoke-Captured $installedSurapkg @("examples", "--json") $work
    if ($listRun.Code -ne 0) {
        Write-Output $listRun.Text
        throw "installed-layout example listing failed"
    }
    $list = $listRun.Text | ConvertFrom-Json
    if ($list.schema -ne "sura.package.examples.v1" -or
        $list.passed -ne $true -or
        [int]$list.total_count -ne $expectedFiles.Count -or
        [int]$list.match_count -ne $expectedFiles.Count -or
        @($list.examples).Count -ne $expectedFiles.Count) {
        $list | ConvertTo-Json -Depth 7
        throw "example gallery JSON contract did not match the installed files"
    }

    $ids = @($list.examples | ForEach-Object { [string]$_.id })
    # surapkg sorts ids byte-wise (std::string operator<); Sort-Object is
    # culture-collated and on Linux/macOS ICU orders "games_2d" before
    # "games/x", so the contract check must compare ordinally.
    $sortedIds = [string[]]$ids.Clone()
    [System.Array]::Sort($sortedIds, [System.StringComparer]::Ordinal)
    if (($ids -join "`n") -ne ($sortedIds -join "`n") -or @($ids | Select-Object -Unique).Count -ne $ids.Count) {
        throw "example ids must be unique and sorted"
    }
    foreach ($requiredId in @("starter/01_hello", "algorithms/word_frequency", "ai_ml/linear_regression", "games_2d/pong", "bad_apple_ascii")) {
        if ($ids -notcontains $requiredId) { throw "example gallery is missing $requiredId" }
    }
    $pong = $list.examples | Where-Object { $_.id -eq "games_2d/pong" } | Select-Object -First 1
    $cuda = $list.examples | Where-Object { $_.id -eq "cuda_language_model_training" } | Select-Object -First 1
    if (-not ($pong.requirements -contains "windows-graphics") -or
        -not ($cuda.requirements -contains "cuda")) {
        throw "example optional-requirement detection was incomplete"
    }

    $filterRun = Invoke-Captured $installedSurapkg @("examples", "algorithms", "--json") $work
    $filtered = $filterRun.Text | ConvertFrom-Json
    if ($filterRun.Code -ne 0 -or $filtered.schema -ne "sura.package.examples.v1" -or
        [int]$filtered.match_count -ne 5 -or @($filtered.examples | Where-Object { $_.category -ne "algorithms" }).Count -ne 0) {
        throw "example category filter failed"
    }

    $emptyRun = Invoke-Captured $installedSurapkg @("examples", "definitely-no-such-example", "--json") $work
    $empty = $emptyRun.Text | ConvertFrom-Json
    if ($emptyRun.Code -ne 0 -or [int]$empty.match_count -ne 0 -or @($empty.examples).Count -ne 0) {
        throw "an empty example search should return a successful empty JSON list"
    }

    $project = Join-Path $work "word_demo"
    $reportPath = Join-Path $work "example-report.json"
    $createRun = Invoke-Captured $installedSurapkg @("example", "algorithms/word_frequency.sura", $project, "--json", $reportPath) $work
    if ($createRun.Code -ne 0 -or $createRun.Text -notmatch "created example project") {
        Write-Output $createRun.Text
        throw "example project creation failed"
    }
    foreach ($required in @(
        (Join-Path $project "sura.pkg.json"),
        (Join-Path $project "src/main.sura"),
        (Join-Path $project "sura.example.json"),
        (Join-Path $project "README.md"),
        (Join-Path $project ".vscode/settings.json"),
        (Join-Path $project ".vscode/launch.json"),
        $reportPath
    )) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "example project is missing generated file: $required"
        }
    }

    $sourceFile = Join-Path $gallery "algorithms/word_frequency.sura"
    $sourceHash = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant()
    $mainHash = (Get-FileHash -LiteralPath (Join-Path $project "src/main.sura") -Algorithm SHA256).Hash.ToLowerInvariant()
    $report = [System.IO.File]::ReadAllText($reportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $provenance = [System.IO.File]::ReadAllText((Join-Path $project "sura.example.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $manifest = [System.IO.File]::ReadAllText((Join-Path $project "sura.pkg.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($sourceHash -ne $mainHash -or
        $report.schema -ne "sura.package.example.v1" -or $report.passed -ne $true -or
        $report.example -ne "algorithms/word_frequency" -or $report.source_sha256 -ne $sourceHash -or
        $provenance.schema -ne "sura.example.provenance.v1" -or $provenance.source_sha256 -ne $sourceHash -or
        $manifest.main -ne "src/main.sura" -or $manifest.name -ne "word_demo") {
        throw "example source, provenance, report, or manifest contract failed"
    }

    $run = Invoke-Captured $enginePath @((Join-Path $project "src/main.sura")) $project
    if ($run.Code -ne 0 -or $run.Text -notmatch "Word frequency") {
        Write-Output $run.Text
        throw "generated example project did not run"
    }

    $duplicate = Invoke-Captured $installedSurapkg @("example", "algorithms/word_frequency", $project) $work
    if ($duplicate.Code -eq 0 -or $duplicate.Text -notmatch "already exists") {
        throw "example creation should reject an existing destination"
    }
    $unknown = Invoke-Captured $installedSurapkg @("example", "../outside", (Join-Path $work "bad")) $work
    if ($unknown.Code -eq 0 -or $unknown.Text -notmatch "unknown example id") {
        throw "example creation should reject unknown or traversal ids"
    }

    Write-Host ("sura_pkg_examples_smoke: PASS ({0} installed examples, generated {1})" -f $list.total_count, $report.example)
}
finally {
    if (Test-Path -LiteralPath $temp) {
        $resolvedTemp = [System.IO.Path]::GetFullPath($temp)
        $systemTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
        if ($resolvedTemp.StartsWith($systemTemp, [System.StringComparison]::OrdinalIgnoreCase) -and
            [System.IO.Path]::GetFileName($resolvedTemp) -match '^sura_pkg_examples_[0-9a-f]{32}$') {
            Remove-Item -LiteralPath $resolvedTemp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# The last check above is a negative test, so this script printed PASS while
# inheriting its nonzero exit code. State the verdict explicitly.
exit 0
