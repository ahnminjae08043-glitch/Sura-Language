param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([IO.Path]::GetTempPath()) ("sura_strict_default_" + [Guid]::NewGuid().ToString("N"))
$utf8 = New-Object Text.UTF8Encoding($false)

function Run-Engine([string[]]$Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $enginePath @Arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Text = ($output -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $invalid = Join-Path $temp "invalid.sura"
    [IO.File]::WriteAllText($invalid, @"
count: number is "not a number"
print("UNSAFE_BODY_EXECUTED")
"@, $utf8)

    $defaultRun = Run-Engine @($invalid)
    if ($defaultRun.Code -eq 0 -or
        $defaultRun.Text -notmatch '\[E200\]' -or
        $defaultRun.Text -notmatch 'Execution stopped' -or
        $defaultRun.Text -match 'UNSAFE_BODY_EXECUTED') {
        Write-Output $defaultRun.Text
        throw "default execution must reject type errors before running the body"
    }

    $defaultCheck = Run-Engine @("--check", $invalid)
    if ($defaultCheck.Code -eq 0 -or $defaultCheck.Text -notmatch '\[FAIL\]') {
        Write-Output $defaultCheck.Text
        throw "default check mode must fail on type errors"
    }

    $legacyRun = Run-Engine @("--legacy-types", $invalid)
    if ($legacyRun.Code -ne 0 -or
        $legacyRun.Text -notmatch '\[Sura Type Warning\]' -or
        $legacyRun.Text -notmatch 'UNSAFE_BODY_EXECUTED') {
        Write-Output $legacyRun.Text
        throw "explicit legacy type mode must retain warning-and-run compatibility"
    }

    $safe = Join-Path $temp "safe.sura"
    [IO.File]::WriteAllText($safe, "value: number is 1`n", $utf8)
    $release = Run-Engine @("--release", "--legacy-types", $safe,
        "--out", (Join-Path $temp "unsafe.srp"))
    if ($release.Code -eq 0 -or $release.Text -notmatch '--release requires strict type safety') {
        Write-Output $release.Text
        throw "protected releases must reject the legacy type bypass"
    }

    $bytecode = Run-Engine @("--compile", "--legacy-types", $safe,
        "--out", (Join-Path $temp "unsafe.sura.bc"))
    if ($bytecode.Code -eq 0 -or $bytecode.Text -notmatch '--compile requires strict type safety') {
        Write-Output $bytecode.Text
        throw "precompiled bytecode must reject the legacy type bypass"
    }

    $help = Run-Engine @("--help")
    if ($help.Code -ne 0 -or $help.Text -notmatch '--legacy-types') {
        throw "CLI help must document the explicit legacy compatibility flag"
    }

    "sura_strict_default_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
