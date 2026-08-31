param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/persistent_desktop_apps_qemu_gate.sura"
$gate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"

if (-not (Test-Path -LiteralPath $gate -PathType Leaf)) {
    throw "Generic QEMU boot gate was not found: $gate"
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Persistent desktop apps QEMU source was not found: $source"
}

$arguments = @{
    Engine = $Engine
    Source = $source
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
    ExpectedEfiText = "Sura executed persistent desktop apps QEMU gate"
    ExpectedMarker = "SURA_PERSISTENT_DESKTOP_APPS_READY"
    ExpectedExitCode = 33
    AdditionalExpectedSerialMarkers = @(
        "SURA_PERSISTENT_DESKTOP_CALCULATOR_OK",
        "SURA_PERSISTENT_DESKTOP_EDITOR_OK",
        "SURA_PERSISTENT_DESKTOP_FILES_OK",
        "SURA_PERSISTENT_DESKTOP_TERMINAL_OK",
        "SURA_PERSISTENT_DESKTOP_SYSTEM_OK",
        "SURA_PERSISTENT_DESKTOP_WINDOW_SERVER_OK",
        "SURA_PERSISTENT_DESKTOP_SAME_PROCESS_OK",
        "SURA_PERSISTENT_DESKTOP_CR3_OK"
    )
}
if ($CompileOnly) {
    $arguments.CompileOnly = $true
}

& $gate @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
