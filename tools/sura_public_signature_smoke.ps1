param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$openssl = Get-Command openssl -ErrorAction SilentlyContinue
if (-not $openssl) {
    "public_signature_smoke: SKIP (openssl not found)"
    exit 0
}

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_public_signature_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

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
    $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

function Run-OpenSSL {
    param([string[]]$OpenSSLArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $openssl.Source @OpenSSLArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($code -ne 0) {
        Write-Output ($out -join "`n")
        throw "openssl failed: $($OpenSSLArgs -join ' ')"
    }
}

$oldPrivate = $env:SURA_SIGNING_PRIVATE_KEY
$oldPublic = $env:SURA_SIGNING_PUBLIC_KEY
$oldPublicDir = $env:SURA_SIGNING_PUBLIC_KEY_DIR
$oldKeyId = $env:SURA_SIGNING_KEY_ID
$oldRequire = $env:SURA_REQUIRE_PUBLIC_SIGNATURE
$oldRegistry = $env:SURA_REGISTRY
$oldRegistryUrl = $env:SURA_REGISTRY_URL
$serverProcess = $null

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $privateKey = Join-Path $temp "private.pem"
    $publicKey = Join-Path $temp "public.pem"
    Run-OpenSSL -OpenSSLArgs @("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", $privateKey)
    Run-OpenSSL -OpenSSLArgs @("rsa", "-pubout", "-in", $privateKey, "-out", $publicKey)

    $env:SURA_SIGNING_PRIVATE_KEY = $privateKey
    $env:SURA_SIGNING_PUBLIC_KEY = $publicKey
    $env:SURA_SIGNING_PUBLIC_KEY_DIR = $null
    $env:SURA_SIGNING_KEY_ID = "smoke-public-key"
    $env:SURA_REQUIRE_PUBLIC_SIGNATURE = $null
    $env:SURA_REGISTRY_URL = $null

    $pkg = Join-Path $temp "public_signed_pkg"
    Write-Text (Join-Path $pkg "sura.pkg.json") @"
{
  "name": "public_signed_pkg",
  "version": "1.0.0",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $pkg "src/main.sura") "print `"signed`"`n"

    $signReportPath = Join-Path $temp "package-sign-report.json"
    $sign = Run-Pkg -PkgArgs @("sign", $pkg, "--json", $signReportPath)
    if ($sign.Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $pkg "sura.pkg.sig"))) {
        Write-Output $sign.Output
        throw "expected public-key package signing to pass"
    }
    $sig = Get-Content -Raw -Path (Join-Path $pkg "sura.pkg.sig")
    if ($sig -notmatch '"algorithm"\s*:\s*"rsa-sha256-v2"' -or
        $sig -notmatch '"keyId"\s*:\s*"smoke-public-key"' -or
        $sig -match '"signature"\s*:\s*"[a-f0-9]{64}"') {
        Write-Output $sig
        throw "expected package signature to use public-key metadata and base64 signature"
    }
    $signReport = Get-Content -Raw -Path $signReportPath | ConvertFrom-Json
    if ($signReport.schema -ne "sura.package.sign.v1" -or
        $signReport.package -ne "public_signed_pkg" -or
        $signReport.version -ne "1.0.0" -or
        $signReport.passed -ne $true -or
        $signReport.algorithm -ne "rsa-sha256-v2" -or
        $signReport.key_id -ne "smoke-public-key" -or
        -not $signReport.hash) {
        throw "expected sign --json to report public-key package signature"
    }

    $verify = Run-Pkg -PkgArgs @("verify", $pkg)
    if ($verify.Code -ne 0 -or $verify.Output -notmatch "public-key signature verified") {
        Write-Output $verify.Output
        throw "expected public-key package verification to pass"
    }

    $env:SURA_REQUIRE_PUBLIC_SIGNATURE = "1"
    $strictVerify = Run-Pkg -PkgArgs @("verify", $pkg)
    if ($strictVerify.Code -ne 0 -or $strictVerify.Output -notmatch "public-key signature verified") {
        Write-Output $strictVerify.Output
        throw "expected strict public-key package verification to pass"
    }
    $env:SURA_SIGNING_PUBLIC_KEY = $null
    $env:SURA_SIGNING_PUBLIC_KEY_DIR = $null
    $missingPublic = Run-Pkg -PkgArgs @("verify", $pkg)
    if ($missingPublic.Code -eq 0 -or $missingPublic.Output -notmatch "public key required") {
        Write-Output $missingPublic.Output
        throw "expected strict verification to require a trusted public key"
    }

    $trustedKeys = Join-Path $temp "trusted_keys"
    $env:SURA_SIGNING_PUBLIC_KEY_DIR = $trustedKeys
    $trustReportPath = Join-Path $temp "trust-key-report.json"
    $trust = Run-Pkg -PkgArgs @("trust-key", "smoke-public-key", $publicKey, "--json", $trustReportPath)
    if ($trust.Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $trustedKeys "smoke-public-key.pem"))) {
        Write-Output $trust.Output
        throw "expected trust-key to write public key into SURA_SIGNING_PUBLIC_KEY_DIR"
    }
    $trustReport = Get-Content -Raw -Path $trustReportPath | ConvertFrom-Json
    if ($trustReport.schema -ne "sura.registry.trust_key.v1" -or
        $trustReport.key_id -ne "smoke-public-key" -or
        $trustReport.passed -ne $true -or
        $trustReport.store -ne "key_dir" -or
        $trustReport.destination -notmatch "trusted_keys" -or
        -not $trustReport.hash) {
        throw "expected trust-key --json to report SURA_SIGNING_PUBLIC_KEY_DIR trust store"
    }
    $trustedVerify = Run-Pkg -PkgArgs @("verify", $pkg)
    if ($trustedVerify.Code -ne 0 -or $trustedVerify.Output -notmatch "public-key signature verified") {
        Write-Output $trustedVerify.Output
        throw "expected strict verification to use SURA_SIGNING_PUBLIC_KEY_DIR"
    }

    $registry = Join-Path $temp "registry"
    $env:SURA_REGISTRY = $registry
    $env:SURA_SIGNING_PUBLIC_KEY_DIR = $null
    $trustRegistryReportPath = Join-Path $temp "trust-registry-key-report.json"
    $trustRegistry = Run-Pkg -PkgArgs @("trust-key", "smoke-public-key", $publicKey, "--json", $trustRegistryReportPath)
    if ($trustRegistry.Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $registry "keys/smoke-public-key.pem"))) {
        Write-Output $trustRegistry.Output
        throw "expected trust-key to write public key into registry keys"
    }
    $trustRegistryReport = Get-Content -Raw -Path $trustRegistryReportPath | ConvertFrom-Json
    if ($trustRegistryReport.schema -ne "sura.registry.trust_key.v1" -or
        $trustRegistryReport.key_id -ne "smoke-public-key" -or
        $trustRegistryReport.passed -ne $true -or
        $trustRegistryReport.store -ne "registry" -or
        $trustRegistryReport.destination -notmatch "registry/keys|registry\\\\keys" -or
        -not $trustRegistryReport.hash) {
        throw "expected trust-key --json to report registry key store"
    }
    $publish = Run-Pkg -PkgArgs @("publish", $pkg)
    if ($publish.Code -ne 0) {
        Write-Output $publish.Output
        throw "expected public-key signed publish to pass"
    }
    $registryVerify = Run-Pkg -PkgArgs @("verify-registry", $registry)
    if ($registryVerify.Code -ne 0 -or $registryVerify.Output -notmatch "public-key signature verified") {
        Write-Output $registryVerify.Output
        throw "expected local registry verification to discover registry keys"
    }

    $remoteRegistry = Join-Path $temp "remote_registry"
    Copy-Item -LiteralPath $registry -Destination $remoteRegistry -Recurse
    $port = Get-Random -Minimum 39001 -Maximum 49000
    $url = "http://127.0.0.1:$port"
    $server = Join-Path (Get-Location) "tools/sura_registry_api.js"
    $stdout = Join-Path $temp "public_signature_registry.out.log"
    $stderr = Join-Path $temp "public_signature_registry.err.log"
    $serverProcess = Start-Process -FilePath "node" `
        -ArgumentList @($server, "--root", $remoteRegistry, "--port", "$port", "--token", "dev-token", "--admin-token", "admin-token") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr

    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            $health = Invoke-RestMethod -Uri "${url}/health" -Method Get
            if ($health.ok) { $ready = $true; break }
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $ready) {
        if (Test-Path $stderr) { Get-Content -Raw -Path $stderr | Write-Output }
        throw "registry API did not become ready"
    }

    $env:SURA_REGISTRY_URL = $url
    $remoteVerify = Run-Pkg -PkgArgs @("verify-registry")
    if ($remoteVerify.Code -ne 0 -or
        $remoteVerify.Output -notmatch "public-key signature verified" -or
        $remoteVerify.Output -notmatch "/keys/smoke-public-key\.pem") {
        Write-Output $remoteVerify.Output
        throw "expected remote registry verification to discover hosted keys"
    }
    $env:SURA_REGISTRY_URL = $null
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force
    }
    $serverProcess = $null

    $env:SURA_REQUIRE_PUBLIC_SIGNATURE = $null
    $env:SURA_SIGNING_PUBLIC_KEY_DIR = $trustedKeys

    $policyPkg = Join-Path $temp "public_policy_pkg"
    Write-Text (Join-Path $policyPkg "sura.pkg.json") @"
{
  "name": "public_policy_pkg",
  "version": "1.0.0",
  "main": "src/main.sura",
  "dependencies": {}
}
"@
    Write-Text (Join-Path $policyPkg "src/main.sura") "print `"policy`"`n"
    Write-Text (Join-Path $policyPkg "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_get"],
  "url_prefixes": ["file://"],
  "allow_shell": false,
  "command_prefixes": []
}
"@

    $signPolicyReportPath = Join-Path $temp "public-sign-policy-report.json"
    $signPolicy = Run-Pkg -PkgArgs @("sign-policy", $policyPkg, "--json", $signPolicyReportPath)
    if ($signPolicy.Code -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $policyPkg "sura.tools.sig"))) {
        Write-Output $signPolicy.Output
        throw "expected public-key tool policy signing to pass"
    }
    $policySig = Get-Content -Raw -Path (Join-Path $policyPkg "sura.tools.sig")
    if ($policySig -notmatch '"algorithm"\s*:\s*"rsa-sha256-tool-policy-v2"') {
        Write-Output $policySig
        throw "expected tool policy signature to use public-key algorithm"
    }
    $signPolicyReport = Get-Content -Raw -Path $signPolicyReportPath | ConvertFrom-Json
    if ($signPolicyReport.schema -ne "sura.package.sign_policy.v1" -or
        $signPolicyReport.passed -ne $true -or
        $signPolicyReport.algorithm -ne "rsa-sha256-tool-policy-v2" -or
        $signPolicyReport.key_id -ne "smoke-public-key" -or
        -not $signPolicyReport.hash) {
        throw "expected sign-policy --json to report public-key tool policy signature"
    }
    $verifyPolicy = Run-Pkg -PkgArgs @("verify-policy", $policyPkg)
    if ($verifyPolicy.Code -ne 0 -or $verifyPolicy.Output -notmatch "public-key signature verified") {
        Write-Output $verifyPolicy.Output
        throw "expected public-key tool policy verification to pass"
    }

    Write-Text (Join-Path $pkg "src/main.sura") "print `"tampered`"`n"
    $tampered = Run-Pkg -PkgArgs @("verify", $pkg)
    if ($tampered.Code -eq 0 -or $tampered.Output -notmatch "package hash mismatch") {
        Write-Output $tampered.Output
        throw "expected tampered package verification to fail"
    }

    "public_signature_smoke: PASS"
}
finally {
    $env:SURA_SIGNING_PRIVATE_KEY = $oldPrivate
    $env:SURA_SIGNING_PUBLIC_KEY = $oldPublic
    $env:SURA_SIGNING_PUBLIC_KEY_DIR = $oldPublicDir
    $env:SURA_SIGNING_KEY_ID = $oldKeyId
    $env:SURA_REQUIRE_PUBLIC_SIGNATURE = $oldRequire
    $env:SURA_REGISTRY = $oldRegistry
    $env:SURA_REGISTRY_URL = $oldRegistryUrl
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force
    }
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
# This gate printed PASS while exiting nonzero: its last native command
# was a negative check that correctly failed, and the script inherited
# that code. CI reads the exit code, so a passing gate reported failure.
exit 0
