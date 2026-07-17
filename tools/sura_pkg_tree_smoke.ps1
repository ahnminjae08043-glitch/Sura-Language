param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_tree_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

function Write-Package {
    param(
        [string]$Path,
        [string]$Name,
        [string]$Version,
        [string]$Dependencies = "{}"
    )
    Write-Text (Join-Path $Path "sura.pkg.json") @"
{
  "name": "$Name",
  "version": "$Version",
  "main": "src/$Name.sura",
  "dependencies": $Dependencies
}
"@
    Write-Text (Join-Path $Path "src/$Name.sura") "func ${Name}_marker do`n  return `"$Version`"`nend`n"
}

$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $registry = Join-Path $temp "registry"
    $env:SURA_REGISTRY = $registry
    $env:SURA_REGISTRY_URL = $null

    $base = Join-Path $temp "base_lib"
    Write-Package -Path $base -Name "base_lib" -Version "1.0.0"
    $publishBase = Run-Pkg -PkgArgs @("publish", $base)
    if ($publishBase.Code -ne 0) {
        Write-Output $publishBase.Output
        throw "expected base_lib publish to pass"
    }

    $mid = Join-Path $temp "mid_lib"
    Write-Package -Path $mid -Name "mid_lib" -Version "1.0.0" -Dependencies "{`n    `"base_lib`": `">=1.0.0 <2.0.0`"`n  }"
    $publishMid = Run-Pkg -PkgArgs @("publish", $mid)
    if ($publishMid.Code -ne 0) {
        Write-Output $publishMid.Output
        throw "expected mid_lib publish to pass"
    }

    $app = Join-Path $temp "tree_app"
    New-Item -ItemType Directory -Force -Path $app | Out-Null
    Write-Text (Join-Path $app "sura.pkg.json") @"
{
  "name": "tree_app",
  "version": "0.1.0",
  "main": "src/app.sura",
  "dependencies": {
    "mid_lib": "^1.0.0"
  }
}
"@
    Write-Text (Join-Path $app "src/app.sura") "print `"tree`"`n"

    Push-Location $app
    try {
        $tree = Run-Pkg -PkgArgs @("tree")
        if ($tree.Code -ne 0 -or
            $tree.Output -notmatch "Dependency tree" -or
            $tree.Output -notmatch "tree_app@0\.1\.0" -or
            $tree.Output -notmatch "mid_lib@1\.0\.0" -or
            $tree.Output -notmatch "base_lib@1\.0\.0" -or
            $tree.Output -notmatch "spec=\^1\.0\.0" -or
            $tree.Output -notmatch "spec=>=1\.0\.0 <2\.0\.0") {
            Write-Output $tree.Output
            throw "expected tree output to show direct and transitive dependencies"
        }

        $jsonTree = Run-Pkg -PkgArgs @("tree", "--json")
        if ($jsonTree.Code -ne 0) {
            Write-Output $jsonTree.Output
            throw "expected tree --json to pass"
        }
        $parsed = $jsonTree.Output | ConvertFrom-Json
        if ($parsed.schema -ne "sura.package.tree.v1" -or
            $parsed.passed -ne $true -or
            $parsed.root.name -ne "tree_app" -or
            $parsed.dependencies[0].name -ne "mid_lib" -or
            $parsed.dependencies[0].version -ne "1.0.0" -or
            $parsed.dependencies[0].dependencies[0].name -ne "base_lib" -or
            $parsed.dependencies[0].dependencies[0].version -ne "1.0.0") {
            $parsed | ConvertTo-Json -Depth 10
            throw "expected tree JSON to include nested dependency graph"
        }

        $why = Run-Pkg -PkgArgs @("why", "base_lib")
        if ($why.Code -ne 0 -or
            $why.Output -notmatch "Dependency reason" -or
            $why.Output -notmatch "package: base_lib@1\.0\.0" -or
            $why.Output -notmatch "tree_app@0\.1\.0" -or
            $why.Output -notmatch "mid_lib@1\.0\.0" -or
            $why.Output -notmatch "base_lib@1\.0\.0" -or
            $why.Output -notmatch "spec=>=1\.0\.0 <2\.0\.0") {
            Write-Output $why.Output
            throw "expected why output to explain transitive base_lib dependency"
        }

        $whyJson = Run-Pkg -PkgArgs @("why", "base_lib", "--json")
        if ($whyJson.Code -ne 0) {
            Write-Output $whyJson.Output
            throw "expected why --json to pass"
        }
        $whyParsed = $whyJson.Output | ConvertFrom-Json
        if ($whyParsed.schema -ne "sura.package.why.v1" -or
            $whyParsed.passed -ne $true -or
            $whyParsed.package.name -ne "base_lib" -or
            $whyParsed.package.found -ne $true -or
            $whyParsed.paths[0][0].name -ne "tree_app" -or
            $whyParsed.paths[0][1].name -ne "mid_lib" -or
            $whyParsed.paths[0][2].name -ne "base_lib" -or
            $whyParsed.paths[0][2].spec -ne ">=1.0.0 <2.0.0") {
            $whyParsed | ConvertTo-Json -Depth 10
            throw "expected why JSON to include root-to-target dependency path"
        }

        $missingWhyJson = Run-Pkg -PkgArgs @("why", "missing_lib", "--json")
        if ($missingWhyJson.Code -eq 0) {
            Write-Output $missingWhyJson.Output
            throw "expected why --json for missing dependency to fail"
        }
        $missingWhyParsed = $missingWhyJson.Output | ConvertFrom-Json
        if ($missingWhyParsed.schema -ne "sura.package.why.v1" -or
            $missingWhyParsed.passed -ne $false -or
            $missingWhyParsed.package.name -ne "missing_lib" -or
            $missingWhyParsed.package.found -ne $false -or
            @($missingWhyParsed.paths).Count -ne 0) {
            $missingWhyParsed | ConvertTo-Json -Depth 10
            throw "expected missing why JSON to report not found"
        }
    }
    finally {
        Pop-Location
    }

    "pkg_tree_smoke: PASS"
}
finally {
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
