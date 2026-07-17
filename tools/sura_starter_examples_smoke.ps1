param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$exampleRoot = Join-Path $root "examples\starter"
$examples = @(Get-ChildItem -LiteralPath $exampleRoot -Filter "*.sura" -File | Sort-Object Name)

if ($examples.Count -ne 12) {
    throw "expected 12 starter examples, found $($examples.Count)"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_starter_examples_" + [System.Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    foreach ($example in $examples) {
        $check = & $enginePath --check $example.FullName 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) {
            $check | Write-Output
            throw "starter example check failed: $($example.Name)"
        }

        Push-Location $temp
        try {
            $run = & $enginePath $example.FullName 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
        }
        finally {
            Pop-Location
        }
        if ($code -ne 0) {
            $run | Write-Output
            throw "starter example run failed: $($example.Name)"
        }
        Write-Host ("[PASS] {0}" -f $example.Name)
    }

    Write-Host "starter_examples_smoke: PASS (12/12)"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
