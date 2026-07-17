param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$EnginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_nn_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Run-SuraCase {
    param([string]$Name, [string]$Source, [string]$Expected)
    $script = Join-Path $temp ($Name + ".sura")
    [System.IO.File]::WriteAllText($script, ($Source.Trim() + "`n"), $utf8NoBom)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $EnginePath --jit $script 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $old
    }
    $text = $output -join "`n"
    if ($code -eq 0 -or $text -notmatch $Expected) {
        Write-Output $text
        throw "$Name should fail with /$Expected/"
    }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    Run-SuraCase "bad_width" @'
use nn
model is nn.mlp([2, 4, 1], {task: "binary"})
nn.predict(model, [1])
'@ "expected 2"

    Run-SuraCase "bad_target" @'
use nn
model is nn.mlp([2, 4, 1], {task: "binary"})
nn.train(model, [[0, 0]], [2], {epochs: 1})
'@ "binary targets must be between 0 and 1"

    Run-SuraCase "bad_model" @'
use nn
nn.summary({kind: "mlp", layers: []})
'@ "model must contain 1"

    Run-SuraCase "bad_loss" @'
use nn
model is nn.mlp([1, 1], {task: "regression"})
nn.train(model, [[0]], [[0]], {epochs: 1, loss: "binary_cross_entropy"})
'@ "requires a sigmoid output"

    Run-SuraCase "bad_standardizer" @'
use nn
nn.standardize([1], {format: "sura.nn.standardizer.v1", mean: [0], scale: [0]})
'@ "scales must be positive"

    Run-SuraCase "bad_split" @'
use nn
nn.split([[0], [1]], [0], {test_count: 1})
'@ "counts must match"

    "nn_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
