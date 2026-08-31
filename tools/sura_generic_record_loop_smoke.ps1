param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$genericTest = Join-Path $root "tests\68_jit_generic_record_loop.sura"
$fallbackTest = Join-Path $root "tests\69_jit_generic_record_loop_fallback.sura"

function Invoke-SuraJit([string]$Source) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $enginePath --jit $Source 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    return @($code, ($output -join "`n"))
}

$previousDisable = [Environment]::GetEnvironmentVariable(
    "SURA_JIT_DISABLE_GENERIC_RECORD_LOOP", "Process")
try {
    Remove-Item Env:\SURA_JIT_DISABLE_GENERIC_RECORD_LOOP -ErrorAction SilentlyContinue
    $generic = Invoke-SuraJit $genericTest
    if ($generic[0] -ne 0 -or
        $generic[1] -notmatch "68_jit_generic_record_loop: PASS" -or
        $generic[1] -notmatch "1 generic numeric record loop execution\(s\)") {
        Write-Output $generic[1]
        throw "generic numeric record loop was not selected (exit=$($generic[0]))"
    }

    $env:SURA_JIT_DISABLE_GENERIC_RECORD_LOOP = "1"
    $disabled = Invoke-SuraJit $genericTest
    if ($disabled[0] -ne 0 -or
        $disabled[1] -notmatch "68_jit_generic_record_loop: PASS" -or
        $disabled[1] -notmatch "0 generic numeric record loop execution\(s\)") {
        Write-Output $disabled[1]
        throw "generic-loop disabled fallback failed (exit=$($disabled[0]))"
    }

    Remove-Item Env:\SURA_JIT_DISABLE_GENERIC_RECORD_LOOP -ErrorAction SilentlyContinue
    $fallback = Invoke-SuraJit $fallbackTest
    if ($fallback[0] -ne 0 -or
        $fallback[1] -notmatch "69_jit_generic_record_loop_fallback: PASS" -or
        $fallback[1] -notmatch "0 generic numeric record loop execution\(s\)") {
        Write-Output $fallback[1]
        throw "side-effecting record loop did not fall back safely (exit=$($fallback[0]))"
    }

    "sura_generic_record_loop_smoke: PASS"
} finally {
    if ($null -eq $previousDisable) {
        Remove-Item Env:\SURA_JIT_DISABLE_GENERIC_RECORD_LOOP -ErrorAction SilentlyContinue
    } else {
        [Environment]::SetEnvironmentVariable(
            "SURA_JIT_DISABLE_GENERIC_RECORD_LOOP", $previousDisable, "Process")
    }
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
