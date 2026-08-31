param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$SurapkgPath = (Resolve-Path $Surapkg).Path
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_list_" + [System.Guid]::NewGuid().ToString("N"))

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
    Push-Location $temp
    try {
        $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $ErrorActionPreference = $old
    }
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    Write-Text (Join-Path $temp "stdlib/core.sura") @"
func core_help do
  return 1
end
"@
    Write-Text (Join-Path $temp "packages/demo/sura.pkg.json") @"
{"name":"demo","version":"1.2.3","main":"src/demo.sura"}
"@
    Write-Text (Join-Path $temp "packages/demo/src/demo.sura") @"
func demo_help do
  return 2
end
"@
    Write-Text (Join-Path $temp "packages/loose.sura") @"
func loose_help do
  return 3
end
"@
    Write-Text (Join-Path $temp "sura.pkg.json") @"
{
  "name": "root_pkg",
  "version": "0.1.0",
  "dependencies": {
    "demo": "^1.2",
    "loose": "file:packages/loose.sura"
  }
}
"@

    $text = Run-Pkg -PkgArgs @("list")
    if ($text.Code -ne 0 -or
        $text.Output -notmatch "Installed packages" -or
        $text.Output -notmatch "core\s+stdlib" -or
        $text.Output -notmatch "cli\s+stdlib" -or
        $text.Output -notmatch "json\s+stdlib" -or
        $text.Output -notmatch "set\s+stdlib" -or
        $text.Output -notmatch "llm\s+stdlib" -or
        $text.Output -notmatch "nn\s+stdlib" -or
        $text.Output -notmatch "autograd\s+stdlib" -or
        $text.Output -notmatch "plugin\s+stdlib" -or
        $text.Output -notmatch "vector\s+stdlib" -or
        $text.Output -notmatch "demo\s+package@1\.2\.3" -or
        $text.Output -notmatch "loose\s+package-file" -or
        $text.Output -notmatch "Manifest dependencies" -or
        $text.Output -notmatch "demo\s+\^1\.2") {
        Write-Output $text.Output
        throw "expected list text output to include packages and deps"
    }

    $json = Run-Pkg -PkgArgs @("list", "--json")
    if ($json.Code -ne 0) {
        Write-Output $json.Output
        throw "expected list --json to pass"
    }
    $report = $json.Output | ConvertFrom-Json
    $entries = @($report.entries)
    $deps = @($report.dependencies)
    $demo = $entries | Where-Object { $_.name -eq "demo" -and $_.kind -eq "package" } | Select-Object -First 1
    $core = $entries | Where-Object { $_.name -eq "core" -and $_.kind -eq "stdlib" } | Select-Object -First 1
    $arrayModule = $entries | Where-Object { $_.name -eq "array" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:array" } | Select-Object -First 1
    $cli = $entries | Where-Object { $_.name -eq "cli" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:cli" } | Select-Object -First 1
    $jsonModule = $entries | Where-Object { $_.name -eq "json" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:json" } | Select-Object -First 1
    $llmModule = $entries | Where-Object { $_.name -eq "llm" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:llm" } | Select-Object -First 1
    $nnModule = $entries | Where-Object { $_.name -eq "nn" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:nn" } | Select-Object -First 1
    $autogradModule = $entries | Where-Object { $_.name -eq "autograd" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:autograd" } | Select-Object -First 1
    $mathModule = $entries | Where-Object { $_.name -eq "math" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:math" } | Select-Object -First 1
    $pathModule = $entries | Where-Object { $_.name -eq "path" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:path" } | Select-Object -First 1
    $pythonModule = $entries | Where-Object { $_.name -eq "python" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:python" } | Select-Object -First 1
    $ffiModule = $entries | Where-Object { $_.name -eq "ffi" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:ffi" } | Select-Object -First 1
    $pluginModule = $entries | Where-Object { $_.name -eq "plugin" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:plugin" } | Select-Object -First 1
    $stringModule = $entries | Where-Object { $_.name -eq "string" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:string" } | Select-Object -First 1
    $setModule = $entries | Where-Object { $_.name -eq "set" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:set" } | Select-Object -First 1
    $osModule = $entries | Where-Object { $_.name -eq "os" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:os" } | Select-Object -First 1
    $randomModule = $entries | Where-Object { $_.name -eq "random" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:random" } | Select-Object -First 1
    $vectorModule = $entries | Where-Object { $_.name -eq "vector" -and $_.kind -eq "stdlib" -and $_.path -eq "builtin:vector" } | Select-Object -First 1
    $loose = $entries | Where-Object { $_.name -eq "loose" -and $_.kind -eq "package-file" } | Select-Object -First 1
    $demoDep = $deps | Where-Object { $_.name -eq "demo" -and $_.spec -eq "^1.2" } | Select-Object -First 1
    $looseDep = $deps | Where-Object { $_.name -eq "loose" -and $_.spec -eq "file:packages/loose.sura" } | Select-Object -First 1
    if ($report.schema -ne "sura.package.list.v1" -or
        -not $report.passed -or
        [int]$report.stdlib_count -lt 27 -or
        [int]$report.package_count -ne 1 -or
        [int]$report.package_file_count -ne 1 -or
        [int]$report.dependency_count -ne 2 -or
        -not $demo -or $demo.version -ne "1.2.3" -or
        -not $core -or -not $arrayModule -or -not $cli -or -not $jsonModule -or
        -not $mathModule -or -not $pathModule -or -not $pythonModule -or -not $ffiModule -or
        -not $pluginModule -or
        -not $stringModule -or -not $setModule -or -not $osModule -or
        -not $randomModule -or
        -not $llmModule -or -not $nnModule -or -not $autogradModule -or -not $vectorModule -or -not $loose -or
        -not $demoDep -or -not $looseDep) {
        Write-Output $json.Output
        throw "expected list --json to expose package inventory"
    }

    "pkg_list_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
