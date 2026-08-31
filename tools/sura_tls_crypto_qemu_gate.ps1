param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe"),
    [string]$Qemu = "",
    [string]$Firmware = "",
    [int]$TimeoutSeconds = 30,
    [ValidateRange(16, 1024)]
    [int]$TcgTranslationCacheMiB = 64,
    [switch]$CompileOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "examples/os/tls_crypto_qemu_gate.sura"
$gate = Join-Path $PSScriptRoot "sura_qemu_boot_gate.ps1"

if (-not (Test-Path -LiteralPath $gate -PathType Leaf)) {
    throw "Generic QEMU boot gate was not found: $gate"
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "TLS cryptographic QEMU source was not found: $source"
}

$arguments = @{
    Engine = $Engine
    Source = $source
    Qemu = $Qemu
    Firmware = $Firmware
    TimeoutSeconds = $TimeoutSeconds
    ExpectedEfiText = "Sura TLS cryptographic foundation test"
    ExpectedMarker = "SURA_TLS_CRYPTO_OK"
    AdditionalExpectedSerialMarkers = @(
        "SURA_SHA256_OK",
        "SURA_HMAC_SHA256_OK",
        "SURA_HKDF_SHA256_OK",
        "SURA_TLS13_LABEL_OK",
        "SURA_AES128_OK",
        "SURA_AES128_GCM_OK",
        "SURA_X25519_OK",
        "SURA_TLS13_RECORD_OK",
        "SURA_TLS13_KEY_SCHEDULE_OK",
        "SURA_TLS13_HELLO_OK",
        "SURA_ENTROPY_X86_OK",
        "SURA_RSA_PUBLIC_OK",
        "SURA_RSA_SHA256_VERIFY_OK",
        "SURA_DER_READER_OK",
        "SURA_X509_CHAIN_OK",
        "SURA_TLS13_APPLICATION_OK",
        "SURA_TLS13_MESSAGES_OK"
    )
    AdditionalQemuArguments = @(
        "-cpu", "max"
    )
    TcgTranslationCacheMiB = $TcgTranslationCacheMiB
    ExpectedExitCode = 33
}
if ($CompileOnly) {
    $arguments.CompileOnly = $true
}

& $gate @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
