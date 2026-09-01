param(
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe")
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$utf8WithBom = New-Object System.Text.UTF8Encoding($true)

if (-not (Test-Path -LiteralPath $Engine)) {
    throw "Sura engine not found: $Engine"
}

function Invoke-ExpectedFailure {
    param([string[]]$EngineArgs)
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $Engine @EngineArgs 1>$null 2>$null
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Copy-TamperedPackage {
    param([string]$Source, [string]$Destination)
    $bytes = [System.IO.File]::ReadAllBytes($Source)
    if ($bytes.Length -lt 16) {
        throw "release package too small to tamper: $Source"
    }
    $bytes[$bytes.Length - 1] = $bytes[$bytes.Length - 1] -bxor 0x5A
    [System.IO.File]::WriteAllBytes($Destination, $bytes)
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_release_pack_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $src = Join-Path $temp "private_app.sura"
    $pack = Join-Path $temp "private_app.sura.srp"
    $packAgain = Join-Path $temp "private_app_again.sura.srp"
    $tamperedPack = Join-Path $temp "private_app_tampered.sura.srp"
    $metadataPack = Join-Path $temp "private_app_metadata.sura.srp"
    $expiredPack = Join-Path $temp "private_app_expired.sura.srp"
    $keyedPack = Join-Path $temp "private_app_keyed.sura.srp"
    $keyedTamperedPack = Join-Path $temp "private_app_keyed_tampered.sura.srp"
    $keyedFilePack = Join-Path $temp "private_app_keyed_file.sura.srp"
    $licensedPack = Join-Path $temp "private_app_licensed.sura.srp"
    $licensedTamperedPack = Join-Path $temp "private_app_licensed_tampered.sura.srp"
    $licensedFilePack = Join-Path $temp "private_app_licensed_file.sura.srp"
    $keyFile = Join-Path $temp "release.key"
    $licenseFile = Join-Path $temp "release.license"
    $result = Join-Path $temp "result.txt"
    $suraResultPath = $result -replace "\\", "/"
    $korean = -join ([char[]](0xBE44, 0xACF5, 0xAC1C, 0x0020, 0xBC30, 0xD3EC))
    $escapedKorean = [regex]::Escape($korean)
    $releaseKey = "customer-key-123"
    $releaseLicense = "seat-license-42"
    [System.IO.File]::WriteAllText($keyFile, $releaseKey + "`r`n", $utf8WithBom)
    [System.IO.File]::WriteAllText($licenseFile, $releaseLicense + "`r`n", $utf8WithBom)

    [System.IO.File]::WriteAllText(
        $src,
        "file_write(`"$suraResultPath`", `"$korean`")`r`nassert_eq(1 + 2, 3)`r`n",
        $utf8WithBom
    )

    & $Engine --release $src --out $pack
    if ($LASTEXITCODE -ne 0) {
        throw "--release failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $pack)) {
        throw "expected release package to be created"
    }

    $packageText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($pack))
    if ($packageText -match $escapedKorean -or $packageText -match "file_write") {
        throw "release package leaked source text"
    }
    $packageBytes = [System.IO.File]::ReadAllBytes($pack)
    if ($packageBytes.Length -lt 5 -or $packageBytes[4] -ne 5) {
        throw "release package should use hardened v5 container"
    }
    if ([System.Text.Encoding]::ASCII.GetString($packageBytes) -match "SURB") {
        throw "release package leaked raw bytecode header"
    }
    Copy-TamperedPackage $pack $tamperedPack
    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    $tamperedExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $tamperedPack)
    if ($tamperedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "tampered release package executed instead of failing integrity checks"
    }

    & $Engine --release $src --out $packAgain
    if ($LASTEXITCODE -ne 0) {
        throw "second randomized --release failed with exit code $LASTEXITCODE"
    }
    $firstBytes = [System.Convert]::ToBase64String([System.IO.File]::ReadAllBytes($pack))
    $secondBytes = [System.Convert]::ToBase64String([System.IO.File]::ReadAllBytes($packAgain))
    if ($firstBytes -eq $secondBytes) {
        throw "release packages should use a randomized nonce"
    }

    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    & $Engine --load-release $pack
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "--load-release did not run the package"
    }
    $actual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($actual -ne $korean) {
        throw "--load-release did not preserve UTF-8 output"
    }

    Remove-Item -LiteralPath $result -Force
    & $Engine $pack
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "direct .sura.srp execution did not run the package"
    }
    $directActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($directActual -ne $korean) {
        throw "direct .sura.srp execution did not preserve UTF-8 output"
    }

    Remove-Item -LiteralPath $result -Force
    $dumpBlockedExit = Invoke-ExpectedFailure -EngineArgs @("--dump", "--load-release", $pack)
    if ($dumpBlockedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "protected release package allowed bytecode dump without owner opt-in"
    }

    $traceBlockedExit = Invoke-ExpectedFailure -EngineArgs @("--trace", $pack)
    if ($traceBlockedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "protected release package allowed trace inspection without owner opt-in"
    }

    $debugBlockedExit = Invoke-ExpectedFailure -EngineArgs @("--debug-protocol", $pack)
    if ($debugBlockedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "protected release package allowed debug protocol inspection without owner opt-in"
    }

    try {
        $env:SURA_ALLOW_RELEASE_INSPECT = "1"
        & $Engine --dump --load-release $pack 1>$null 2>$null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
            throw "owner opt-in did not allow protected release inspection"
        }
    } finally {
        Remove-Item Env:SURA_ALLOW_RELEASE_INSPECT -ErrorAction SilentlyContinue
    }
    $inspectActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($inspectActual -ne $korean) {
        throw "owner opt-in release inspection did not preserve UTF-8 output"
    }

    & $Engine --release $src --out $metadataPack --release-id "customer-42" --release-expires "2999-12-31"
    if ($LASTEXITCODE -ne 0) {
        throw "metadata --release failed with exit code $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $result -Force
    & $Engine --load-release $metadataPack
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "metadata package did not run before expiration"
    }
    $metadataActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($metadataActual -ne $korean) {
        throw "metadata package did not preserve UTF-8 output"
    }

    & $Engine --release $src --out $expiredPack --release-id "expired-customer" --release-expires "2000-01-01"
    if ($LASTEXITCODE -ne 0) {
        throw "expired --release failed with exit code $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $result -Force
    $expiredExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $expiredPack)
    if ($expiredExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "expired package ran after expiration"
    }

    & $Engine --release $src --out $keyedPack --release-key $releaseKey
    if ($LASTEXITCODE -ne 0) {
        throw "keyed --release failed with exit code $LASTEXITCODE"
    }
    $keyedText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($keyedPack))
    if ($keyedText -match $escapedKorean -or $keyedText -match "file_write") {
        throw "keyed release package leaked source text"
    }
    Copy-TamperedPackage $keyedPack $keyedTamperedPack
    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    $keyedTamperedExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $keyedTamperedPack, "--load-release-key", $releaseKey)
    if ($keyedTamperedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "tampered keyed release package executed instead of failing integrity checks"
    }

    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    $missingKeyExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $keyedPack)
    if ($missingKeyExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "keyed package ran without key"
    }

    $wrongKeyExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $keyedPack, "--load-release-key", "wrong-key")
    if ($wrongKeyExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "keyed package ran with wrong key"
    }

    & $Engine --load-release $keyedPack --load-release-key $releaseKey
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "keyed package did not run with matching key"
    }
    $keyedActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($keyedActual -ne $korean) {
        throw "keyed package did not preserve UTF-8 output"
    }

    Remove-Item -LiteralPath $result -Force
    try {
        $env:SURA_RELEASE_KEY = $releaseKey
        & $Engine $keyedPack
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
            throw "direct keyed .sura.srp execution did not use SURA_RELEASE_KEY"
        }
        $envActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
        if ($envActual -ne $korean) {
            throw "direct keyed .sura.srp execution did not preserve UTF-8 output"
        }
    } finally {
        Remove-Item Env:SURA_RELEASE_KEY -ErrorAction SilentlyContinue
    }

    & $Engine --release $src --out $keyedFilePack --release-key-file $keyFile
    if ($LASTEXITCODE -ne 0) {
        throw "keyed file --release failed with exit code $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $result -Force
    & $Engine --load-release $keyedFilePack --load-release-key-file $keyFile
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "keyed package did not run with matching key file"
    }
    $keyedFileActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($keyedFileActual -ne $korean) {
        throw "keyed package did not preserve UTF-8 output with key file"
    }

    & $Engine --release $src --out $licensedPack --release-license $releaseLicense
    if ($LASTEXITCODE -ne 0) {
        throw "licensed --release failed with exit code $LASTEXITCODE"
    }
    $licensedText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($licensedPack))
    if ($licensedText -match $escapedKorean -or $licensedText -match "file_write" -or $licensedText -match [regex]::Escape($releaseLicense)) {
        throw "licensed release package leaked source or license text"
    }
    Copy-TamperedPackage $licensedPack $licensedTamperedPack
    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    $licensedTamperedExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $licensedTamperedPack, "--load-release-license", $releaseLicense)
    if ($licensedTamperedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "tampered licensed release package executed instead of failing integrity checks"
    }

    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    $missingLicenseExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $licensedPack)
    if ($missingLicenseExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "licensed package ran without license"
    }

    $wrongLicenseExit = Invoke-ExpectedFailure -EngineArgs @("--load-release", $licensedPack, "--load-release-license", "wrong-license")
    if ($wrongLicenseExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "licensed package ran with wrong license"
    }

    & $Engine --load-release $licensedPack --load-release-license $releaseLicense
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "licensed package did not run with matching license"
    }
    $licensedActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($licensedActual -ne $korean) {
        throw "licensed package did not preserve UTF-8 output"
    }

    Remove-Item -LiteralPath $result -Force
    try {
        $env:SURA_RELEASE_LICENSE = $releaseLicense
        & $Engine $licensedPack
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
            throw "direct licensed .sura.srp execution did not use SURA_RELEASE_LICENSE"
        }
        $envLicenseActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
        if ($envLicenseActual -ne $korean) {
            throw "direct licensed .sura.srp execution did not preserve UTF-8 output"
        }
    } finally {
        Remove-Item Env:SURA_RELEASE_LICENSE -ErrorAction SilentlyContinue
    }

    & $Engine --release $src --out $licensedFilePack --release-license-file $licenseFile
    if ($LASTEXITCODE -ne 0) {
        throw "licensed file --release failed with exit code $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $result -Force
    & $Engine --load-release $licensedFilePack --load-release-license-file $licenseFile
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "licensed package did not run with matching license file"
    }
    $licensedFileActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($licensedFileActual -ne $korean) {
        throw "licensed package did not preserve UTF-8 output with license file"
    }

    Write-Output "release_pack_smoke: PASS"
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
