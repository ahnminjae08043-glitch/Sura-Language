param(
    [string]$Surapkg = (Join-Path (Split-Path -Parent $PSScriptRoot) "surapkg.exe"),
    [string]$Engine = (Join-Path (Split-Path -Parent $PSScriptRoot) "SuraLanguage.exe")
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$utf8WithBom = New-Object System.Text.UTF8Encoding($true)

if (-not (Test-Path -LiteralPath $Surapkg)) {
    throw "surapkg not found: $Surapkg"
}
if (-not (Test-Path -LiteralPath $Engine)) {
    throw "Sura engine not found: $Engine"
}

function Invoke-ExpectedFailure {
    param([string[]]$CommandArgs)
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $Engine @CommandArgs 1>$null 2>$null
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Copy-TamperedPackage {
    param([string]$Source, [string]$Destination)
    $bytes = [System.IO.File]::ReadAllBytes($Source)
    if ($bytes.Length -lt 16) {
        throw "protected package too small to tamper: $Source"
    }
    $bytes[$bytes.Length - 1] = $bytes[$bytes.Length - 1] -bxor 0x5A
    [System.IO.File]::WriteAllBytes($Destination, $bytes)
}

$oldEngine = $env:SURA_ENGINE
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_protect_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

try {
    $env:SURA_ENGINE = $Engine
    $pkg = Join-Path $temp "private_pkg"
    $srcDir = Join-Path $pkg "src"
    New-Item -ItemType Directory -Force -Path $srcDir | Out-Null

    $result = Join-Path $temp "result.txt"
    $suraResultPath = $result -replace "\\", "/"
    $korean = -join ([char[]](0xBCF4, 0xD638, 0x0020, 0xBC30, 0xD3EC, 0x0020, 0xC644, 0xB8CC))
    $escapedKorean = [regex]::Escape($korean)
    $releaseKey = "customer-key-123"
    $releaseLicense = "seat-license-42"
    $keyFile = Join-Path $temp "release.key"
    $licenseFile = Join-Path $temp "release.license"
    $defaultTamperedPack = Join-Path $temp "private_pkg_default_tampered.sura.srp"
    $closedTamperedPack = Join-Path $temp "private_pkg_closed_tampered.sura.srp"
    $launcherPack = Join-Path $temp "private_pkg_launcher.sura.srp"
    $launcherExe = Join-Path $temp "private_pkg_launcher.exe"
    [System.IO.File]::WriteAllText($keyFile, $releaseKey + "`r`n", $utf8WithBom)
    [System.IO.File]::WriteAllText($licenseFile, $releaseLicense + "`r`n", $utf8WithBom)

    [System.IO.File]::WriteAllText(
        (Join-Path $pkg "sura.pkg.json"),
        "{`n  `"name`": `"private_pkg`",`n  `"version`": `"0.1.0`",`n  `"main`": `"src/private_pkg.sura`",`n  `"dependencies`": {}`n}`n",
        $utf8NoBom
    )
    [System.IO.File]::WriteAllText(
        (Join-Path $srcDir "private_pkg.sura"),
        "file_write(`"$suraResultPath`", `"$korean`")`r`nassert_eq(1 + 2, 3)`r`n",
        $utf8WithBom
    )

    & $Surapkg protect $pkg
    if ($LASTEXITCODE -ne 0) {
        throw "surapkg protect default output failed with exit code $LASTEXITCODE"
    }
    $defaultPack = Join-Path $pkg "dist/private_pkg-0.1.0.sura.srp"
    if (-not (Test-Path -LiteralPath $defaultPack)) {
        throw "expected default protected package at $defaultPack"
    }
    $defaultReport = "$defaultPack.protect.json"
    if (-not (Test-Path -LiteralPath $defaultReport)) {
        throw "expected protect leak report at $defaultReport"
    }
    $defaultReportText = [System.IO.File]::ReadAllText($defaultReport, [System.Text.Encoding]::UTF8)
    if ($defaultReportText -notmatch '"schema"\s*:\s*"sura\.package\.protect\.v1"' -or
        $defaultReportText -notmatch '"status"\s*:\s*"PASS"' -or
        $defaultReportText -notmatch '"passed"\s*:\s*true' -or
        $defaultReportText -notmatch '"next_actions"\s*:' -or
        $defaultReportText -notmatch 'ship only the protected .sura.srp or launcher' -or
        $defaultReportText -notmatch '--closed-source and --key-file' -or
        $defaultReportText -notmatch '"sourceFilesScanned"\s*:\s*1' -or
        $defaultReportText -notmatch '"targets"\s*:') {
        throw "default protect leak report did not pass"
    }
    $defaultVerifyReport = Join-Path $temp "default.protect-verify.json"
    & $Surapkg protect-verify $defaultReport --require-target package --json $defaultVerifyReport
    if ($LASTEXITCODE -ne 0) {
        throw "surapkg protect-verify default report failed with exit code $LASTEXITCODE"
    }
    $defaultVerify = Get-Content -Raw -LiteralPath $defaultVerifyReport | ConvertFrom-Json
    if ($defaultVerify.schema -ne "sura.package.protect_verify.v1" -or
        $defaultVerify.passed -ne $true -or
        $defaultVerify.protect_schema -ne "sura.package.protect.v1" -or
        $defaultVerify.failure_count -ne 0 -or
        $defaultVerify.target_count -lt 1 -or
        -not ($defaultVerify.targets | Where-Object { $_.kind -eq "package" -and $_.exists -eq $true })) {
        $defaultVerify | ConvertTo-Json -Depth 6
        throw "default protect-verify JSON report did not pass"
    }
    $defaultStrictVerifyReport = Join-Path $temp "default.strict.protect-verify.json"
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $defaultStrictOutput = (& $Surapkg protect-verify $defaultReport --require-closed-source --require-key --require-license --require-expires --json $defaultStrictVerifyReport 2>&1) | Out-String
        $defaultStrictExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    if ($defaultStrictExit -eq 0 -or -not (Test-Path -LiteralPath $defaultStrictVerifyReport)) {
        Write-Output $defaultStrictOutput
        throw "default protect-verify strict gate should fail"
    }
    $defaultStrictVerify = Get-Content -Raw -LiteralPath $defaultStrictVerifyReport | ConvertFrom-Json
    if ($defaultStrictVerify.passed -ne $false -or
        $defaultStrictVerify.failure_count -lt 4 -or
        -not ($defaultStrictVerify.next_actions | Where-Object { $_.check -eq "closed_source" }) -or
        -not ($defaultStrictVerify.next_actions | Where-Object { $_.check -eq "keyed" }) -or
        -not ($defaultStrictVerify.next_actions | Where-Object { $_.check -eq "licensed" }) -or
        -not ($defaultStrictVerify.next_actions | Where-Object { $_.check -eq "expires" })) {
        $defaultStrictVerify | ConvertTo-Json -Depth 6
        throw "default strict protect-verify gate did not explain missing controls"
    }
    $defaultText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($defaultPack))
    if ($defaultText -match $escapedKorean -or $defaultText -match "file_write") {
        throw "default protected package leaked source text"
    }
    Copy-TamperedPackage $defaultPack $defaultTamperedPack
    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    $defaultTamperedExit = Invoke-ExpectedFailure -CommandArgs @("--load-release", $defaultTamperedPack)
    if ($defaultTamperedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "tampered default protected package executed instead of failing integrity checks"
    }

    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    & $Engine --load-release $defaultPack
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "default protected package did not run"
    }
    $defaultActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($defaultActual -ne $korean) {
        throw "default protected package did not preserve UTF-8 output"
    }

    $lockedPack = Join-Path $temp "private_pkg_locked.sura.srp"
    $lockedReport = "$lockedPack.protect.json"
    & $Surapkg protect $pkg --out $lockedPack --key-file $keyFile --license-file $licenseFile --id "customer-42" --expires "2999-12-31" --json $lockedReport
    if ($LASTEXITCODE -ne 0) {
        throw "surapkg protect locked output failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $lockedPack)) {
        throw "expected locked protected package at $lockedPack"
    }
    if (-not (Test-Path -LiteralPath $lockedReport)) {
        throw "expected locked protect leak report at $lockedReport"
    }
    $lockedReportText = [System.IO.File]::ReadAllText($lockedReport, [System.Text.Encoding]::UTF8)
    if ($lockedReportText -notmatch '"status"\s*:\s*"PASS"' -or
        $lockedReportText -notmatch 'provide the release key at runtime' -or
        $lockedReportText -notmatch 'provide the release license at runtime' -or
        $lockedReportText -match [regex]::Escape($releaseLicense)) {
        throw "locked protect leak report did not pass or leaked license"
    }
    $lockedText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($lockedPack))
    if ($lockedText -match $escapedKorean -or $lockedText -match "file_write" -or $lockedText -match [regex]::Escape($releaseLicense)) {
        throw "locked protected package leaked source or license text"
    }

    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    & $Engine --load-release $lockedPack --load-release-key-file $keyFile --load-release-license-file $licenseFile
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "locked protected package did not run with matching key/license"
    }
    $lockedActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($lockedActual -ne $korean) {
        throw "locked protected package did not preserve UTF-8 output"
    }

    $missingStrictPack = Join-Path $temp "private_pkg_missing_closed.sura.srp"
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $closedMissingOutput = (& $Surapkg protect $pkg --out $missingStrictPack --closed-source 2>&1) | Out-String
        $closedMissingExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    if ($closedMissingExit -eq 0 -or $closedMissingOutput -notmatch "--closed-source/--require-key") {
        throw "closed-source protect should require a non-empty release key"
    }
    if (Test-Path -LiteralPath $missingStrictPack) {
        throw "closed-source protect created an artifact without required key/license"
    }

    $closedPack = Join-Path $temp "private_pkg_closed.sura.srp"
    $closedReport = Join-Path $temp "private_pkg_closed.protect.json"
    & $Surapkg protect $pkg --out $closedPack --closed-source --key-file $keyFile --license-file $licenseFile --expires "2999-12-31" "--json=$closedReport"
    if ($LASTEXITCODE -ne 0) {
        throw "closed-source protect failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $closedPack)) {
        throw "expected closed-source protected package at $closedPack"
    }
    Copy-TamperedPackage $closedPack $closedTamperedPack
    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    $closedTamperedExit = Invoke-ExpectedFailure -CommandArgs @("--load-release", $closedTamperedPack, "--load-release-key-file", $keyFile, "--load-release-license-file", $licenseFile)
    if ($closedTamperedExit -eq 0 -or (Test-Path -LiteralPath $result)) {
        throw "tampered closed-source protected package executed instead of failing integrity checks"
    }
    $closedReportText = [System.IO.File]::ReadAllText($closedReport, [System.Text.Encoding]::UTF8)
    if ($closedReportText -notmatch '"schema"\s*:\s*"sura\.package\.protect\.v1"' -or
        $closedReportText -notmatch '"mode"\s*:\s*"closed-source"' -or
        $closedReportText -notmatch '"passed"\s*:\s*true' -or
        $closedReportText -notmatch '"keyed"\s*:\s*true' -or
        $closedReportText -notmatch '"licensed"\s*:\s*true' -or
        $closedReportText -notmatch '"next_actions"\s*:' -or
        $closedReportText -notmatch '"expires"\s*:\s*"2999-12-31"') {
        throw "closed-source protect report did not include enforced protection metadata"
    }
    $closedVerifyReport = Join-Path $temp "closed.protect-verify.json"
    & $Surapkg protect-verify $closedReport --require-closed-source --require-key --require-license --require-expires --require-target package --json $closedVerifyReport
    if ($LASTEXITCODE -ne 0) {
        throw "surapkg protect-verify closed-source report failed with exit code $LASTEXITCODE"
    }
    $closedVerify = Get-Content -Raw -LiteralPath $closedVerifyReport | ConvertFrom-Json
    if ($closedVerify.passed -ne $true -or
        $closedVerify.mode -ne "closed-source" -or
        $closedVerify.keyed -ne $true -or
        $closedVerify.licensed -ne $true -or
        $closedVerify.expires -ne "2999-12-31" -or
        $closedVerify.failure_count -ne 0) {
        $closedVerify | ConvertTo-Json -Depth 6
        throw "closed-source protect-verify JSON report did not pass"
    }

    Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue
    & $Engine --load-release $closedPack --load-release-key-file $keyFile --load-release-license-file $licenseFile
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "closed-source protected package did not run with matching key/license"
    }
    $closedActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($closedActual -ne $korean) {
        throw "closed-source protected package did not preserve UTF-8 output"
    }

    & $Surapkg protect $pkg --out $launcherPack --exe $launcherExe
    if ($LASTEXITCODE -ne 0) {
        throw "surapkg protect launcher exe failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path -LiteralPath $launcherExe)) {
        throw "expected protected launcher exe at $launcherExe"
    }
    $launcherReport = "$launcherPack.protect.json"
    if (-not (Test-Path -LiteralPath $launcherReport)) {
        throw "expected launcher protect leak report at $launcherReport"
    }
    $launcherReportText = [System.IO.File]::ReadAllText($launcherReport, [System.Text.Encoding]::UTF8)
    if ($launcherReportText -notmatch '"status"\s*:\s*"PASS"' -or
        $launcherReportText -notmatch '"schema"\s*:\s*"sura\.package\.protect\.v1"' -or
        $launcherReportText -notmatch '"kind"\s*:\s*"launcher"') {
        throw "launcher protect leak report did not include launcher target"
    }
    $launcherVerifyReport = Join-Path $temp "launcher.protect-verify.json"
    & $Surapkg protect-verify $launcherReport --require-target launcher --json $launcherVerifyReport
    if ($LASTEXITCODE -ne 0) {
        throw "surapkg protect-verify launcher report failed with exit code $LASTEXITCODE"
    }
    $launcherVerify = Get-Content -Raw -LiteralPath $launcherVerifyReport | ConvertFrom-Json
    if ($launcherVerify.passed -ne $true -or
        $launcherVerify.failure_count -ne 0 -or
        -not ($launcherVerify.targets | Where-Object { $_.kind -eq "launcher" -and $_.exists -eq $true })) {
        $launcherVerify | ConvertTo-Json -Depth 6
        throw "launcher protect-verify JSON report did not pass"
    }
    $launcherText = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($launcherExe))
    if ($launcherText -match $escapedKorean -or $launcherText -match "file_write") {
        throw "protected launcher exe leaked source text"
    }

    Remove-Item -LiteralPath $result -Force
    & $launcherExe
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $result)) {
        throw "protected launcher exe did not run"
    }
    $launcherActual = [System.IO.File]::ReadAllText($result, [System.Text.Encoding]::UTF8)
    if ($launcherActual -ne $korean) {
        throw "protected launcher exe did not preserve UTF-8 output"
    }

    Write-Output "pkg_protect_smoke: PASS"
} finally {
    if ($null -eq $oldEngine) {
        Remove-Item Env:SURA_ENGINE -ErrorAction SilentlyContinue
    } else {
        $env:SURA_ENGINE = $oldEngine
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

# Verified passing; state the exit code rather than inheriting it.
exit 0
