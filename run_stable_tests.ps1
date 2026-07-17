param(
    [switch]$NoJit,
    [string]$Engine = "",
    [string[]]$TestPath = @(),
    [ValidateRange(1, 86400)][int]$TimeoutSeconds = 120
)

$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$processHelper = Join-Path $root "tools\sura_test_process.ps1"
if (-not (Test-Path -LiteralPath $processHelper)) {
    Write-Error "Sura test process helper not found: $processHelper"
    exit 1
}
. $processHelper

$engine = if ($Engine) { $Engine } else { Join-Path $root "SuraLanguage.exe" }

if (-not (Test-Path $engine)) {
    Write-Error "SuraLanguage.exe not found. Run build.bat first."
    exit 1
}

$tests = @()
if ($TestPath.Count -gt 0) {
    $tests += $TestPath | ForEach-Object { Get-Item -LiteralPath $_ }
} else {
    $tests += Get-ChildItem -Path (Join-Path $root "tests") -Filter "*.sura" -File | Sort-Object Name
    $tests += @(
        "test_stdlib.sura",
        "test_stdlib_practical.sura",
        "test_world_features.sura",
        "test_js_target.sura",
        "test_wasm_target.sura",
        "test_methods_chain.sura",
        "test_null_optional.sura",
        "test_when_match.sura",
        "test_for_in_improved.sura",
        "test_phase9\test_phase9_ops.sura"
    ) | ForEach-Object { Get-Item (Join-Path $root $_) }
}

$pass = 0
$fail = 0
$engineSnapshot = $null
try {
    $engineSnapshot = New-SuraTestEngineSnapshot -EnginePath $engine
} catch {
    Write-Error "Could not create an immutable Sura test engine snapshot: $($_.Exception.Message)"
    exit 1
}
$engine = $engineSnapshot.Path
Write-Host ("Engine snapshot: {0} ({1} bytes)" -f $engineSnapshot.Sha256, $engineSnapshot.Bytes)

try {
    foreach ($test in $tests) {
        $relative = Resolve-Path -Path $test.FullName -Relative
        $sourceText = [System.IO.File]::ReadAllText($test.FullName)
        $expectMatch = [regex]::Match(
            $sourceText,
            '(?m)^\s*#\s*sura-test:\s*expect-error\s+(\S+)\s*$'
        )
        $expectedError = if ($expectMatch.Success) { $expectMatch.Groups[1].Value } else { "" }
        $engineArgs = @()
        if (-not $NoJit) { $engineArgs += "--jit" }
        $engineArgs += $test.FullName
        $run = Invoke-SuraTestProcess -EnginePath $engine -Arguments $engineArgs -TimeoutSeconds $TimeoutSeconds
        $code = $run.ExitCode
        $outputText = $run.Output
        $passedTest = if ($run.TimedOut) {
            $false
        } elseif ($expectedError) {
            $code -ne 0 -and $outputText -match [regex]::Escape($expectedError)
        } else {
            $code -eq 0
        }
        if ($passedTest) {
            Write-Host "[PASS] $relative ($($run.DurationMs) ms)"
            $pass++
        } else {
            if ($run.TimedOut) {
                Write-Host "[TIMEOUT] $relative exceeded ${TimeoutSeconds}s"
            } else {
                Write-Host "[FAIL] $relative ($($run.DurationMs) ms)"
            }
            if ($expectedError -and -not $run.TimedOut) {
                Write-Host "Expected non-zero exit containing '$expectedError', got exit $code."
            }
            if ($outputText) { $outputText | Write-Host }
            $fail++
        }
    }
}
finally {
    if (-not (Test-SuraTestEngineSnapshot -Snapshot $engineSnapshot)) {
        Write-Host "[FAIL] immutable test engine snapshot changed during the suite"
        $fail++
    }
    if (-not (Test-SuraTestEngineSourceUnchanged -Snapshot $engineSnapshot)) {
        Write-Warning "The source Sura engine changed during the suite; results still use snapshot $($engineSnapshot.Sha256)."
    }
    try {
        Remove-SuraTestEngineSnapshot -Snapshot $engineSnapshot
    } catch {
        Write-Host "[FAIL] could not remove immutable test engine snapshot: $($_.Exception.Message)"
        $fail++
    }
}

Write-Host ("Stable tests ({0}): {1} passed, {2} failed" -f $(if ($NoJit) { "VM" } else { "JIT" }), $pass, $fail)
if ($fail -gt 0) { exit 1 }
$global:LASTEXITCODE = 0
