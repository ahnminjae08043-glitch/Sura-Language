param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/gzip_qemu_gate.sura"
$gate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"

if (-not (Test-Path -LiteralPath $gate -PathType Leaf)) {
    throw "Generic QEMU boot gate was not found: $gate"
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "gzip QEMU source was not found: $source"
}

$arguments = @{
    Engine = $Engine
    Source = $source
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
    ExpectedEfiText = "Sura gzip and DEFLATE test"
    ExpectedMarker = "SURA_GZIP_INFLATE_OK"
    ExpectedExitCode = 33
}
if ($CompileOnly) {
    $arguments.CompileOnly = $true
}

& $gate @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
