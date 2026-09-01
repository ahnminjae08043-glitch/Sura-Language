param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe")
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "tests/test_jit_collections.sura"

function Invoke-SuraCollectionTest {
    param([string[]]$Arguments)

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $enginePath @Arguments $source 2>&1 | ForEach-Object { "$_" }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    return @($exitCode, ($output -join "`n"))
}

$vm = Invoke-SuraCollectionTest @()
if ($vm[0] -ne 0 -or $vm[1] -notmatch "jit collections checksum: 37497500") {
    Write-Output $vm[1]
    throw "collection VM semantics failed (exit=$($vm[0]))"
}

$jit = Invoke-SuraCollectionTest @("--jit")
if ($jit[0] -ne 0 -or $jit[1] -notmatch "jit collections checksum: 37497500") {
    Write-Output $jit[1]
    throw "collection JIT semantics failed (exit=$($jit[0]))"
}
if ($jit[1] -notmatch "1 function\(s\).*compiled") {
    Write-Output $jit[1]
    throw "make_packet was not native-compiled"
}

"sura_jit_collections_smoke: PASS"
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
