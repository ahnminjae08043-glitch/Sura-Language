param(
    [Parameter(Mandatory = $true)]
    [string[]]$Files,
    [string]$PfxPath = "",
    [string]$CertificateThumbprint = "",
    [string]$PasswordEnv = "SURA_CODESIGN_PFX_PASSWORD",
    [string]$TimestampServer = "http://timestamp.digicert.com",
    [switch]$AllowTestCertificate,
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
if ([string]::IsNullOrWhiteSpace($PfxPath) -eq [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    throw "provide exactly one of -PfxPath or -CertificateThumbprint"
}

$certificate = $null
if (-not [string]::IsNullOrWhiteSpace($PfxPath)) {
    $pfx = (Resolve-Path -LiteralPath $PfxPath).Path
    $plainPassword = [Environment]::GetEnvironmentVariable($PasswordEnv)
    if ($null -eq $plainPassword) {
        throw "PFX password environment variable is not set: $PasswordEnv"
    }
    $certificate = New-Object -TypeName System.Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList @(
        $pfx,
        $plainPassword,
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable
    )
} else {
    $normalized = $CertificateThumbprint.Replace(" ", "").ToUpperInvariant()
    $certificate = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $normalized } |
        Select-Object -First 1
    if ($null -eq $certificate) { throw "code-signing certificate was not found: $normalized" }
}

if (-not $certificate.HasPrivateKey) { throw "the selected certificate has no private key" }
if ([DateTime]::Now -lt $certificate.NotBefore -or [DateTime]::Now -gt $certificate.NotAfter) {
    throw "the selected certificate is outside its validity period"
}
$codeSigningEku = @($certificate.Extensions | Where-Object { $_.Oid.Value -eq "2.5.29.37" } | ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value })
if ($codeSigningEku -notcontains "1.3.6.1.5.5.7.3.3") {
    throw "the selected certificate is not valid for code signing"
}
if (-not $AllowTestCertificate -and $certificate.Subject -eq $certificate.Issuer) {
    throw "self-signed certificates are test-only and do not remove public download warnings; pass -AllowTestCertificate only for a local test"
}

$results = @()
foreach ($file in $Files) {
    $path = (Resolve-Path -LiteralPath $file).Path
    $signature = Set-AuthenticodeSignature -LiteralPath $path -Certificate $certificate -HashAlgorithm SHA256 -TimestampServer $TimestampServer
    $verified = Get-AuthenticodeSignature -LiteralPath $path
    if (-not $AllowTestCertificate -and [string]$verified.Status -ne "Valid") {
        throw "signed file did not verify as Valid: $path ($($verified.Status))"
    }
    if ([string]$verified.Status -eq "NotSigned") {
        throw "file remained unsigned after signing: $path"
    }
    $results += [pscustomobject]@{
        file = $path
        status = [string]$verified.Status
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        signer_subject = [string]$certificate.Subject
        signer_thumbprint = [string]$certificate.Thumbprint
        timestamp_server = $TimestampServer
        timestamp_present = ($null -ne $verified.TimeStamperCertificate)
    }
}

$report = [ordered]@{
    schema = "sura.windows.signing.report.v1"
    signed_utc = [DateTime]::UtcNow.ToString("o")
    test_certificate_allowed = [bool]$AllowTestCertificate
    files = $results
}
if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $jsonPath = [System.IO.Path]::GetFullPath($JsonOut)
    $parent = Split-Path -Parent $jsonPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 6), $utf8NoBom)
}

"sura_sign_windows: PASS ($($results.Count) file(s))"
