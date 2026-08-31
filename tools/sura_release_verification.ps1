param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Version = "",
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$contract = [System.IO.File]::ReadAllText((Join-Path $root "version.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
if ($contract.schema -ne "sura.version.v1" -or [string]$contract.version -notmatch '^\d+\.\d+\.\d+$') {
    throw "version.json does not satisfy sura.version.v1"
}
if ([string]::IsNullOrWhiteSpace($Version)) { $Version = [string]$contract.version }
if ($Version -ne [string]$contract.version) { throw "verification version $Version does not match version.json $($contract.version)" }
$series = [string]$contract.series
$public = Join-Path $root "sura_presentation/public/downloads"
if ([string]::IsNullOrWhiteSpace($JsonOut)) { $JsonOut = Join-Path $public "verification-$Version.json" }
$engine = Join-Path $root "SuraLanguage.exe"
$surapkg = Join-Path $root "surapkg.exe"
$powershell = (Get-Command powershell -ErrorAction Stop).Source
$npm = (Get-Command npm.cmd -ErrorAction Stop).Source

function Invoke-Captured {
    param([string]$FilePath, [string[]]$Arguments, [string]$WorkingDirectory)
    Push-Location $WorkingDirectory
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
        Pop-Location
    }
    return [pscustomobject]@{ code = $code; text = ($output -join "`n") }
}

function Require-Step {
    param([string]$Name, $Result, [string]$RequiredPattern = "")
    if ($Result.code -ne 0 -or (-not [string]::IsNullOrWhiteSpace($RequiredPattern) -and $Result.text -notmatch $RequiredPattern)) {
        Write-Output $Result.text
        throw "$Name failed with exit code $($Result.code)"
    }
}

$engineVersion = Invoke-Captured $engine @("--version") $root
Require-Step "engine version" $engineVersion ('^Sura Language ' + [regex]::Escape($Version) + '$')

$vm = Invoke-Captured $powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "run_stable_tests.ps1"), "-NoJit", "-FailOnSkip") $root
Require-Step "stable VM suite" $vm 'Stable tests \(VM\): \d+ passed, 0 skipped, 0 failed'
$vmMatch = [regex]::Match($vm.text, 'Stable tests \(VM\): (\d+) passed, 0 skipped, 0 failed')
$vmCount = [int]$vmMatch.Groups[1].Value

$jit = Invoke-Captured $powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "run_stable_tests.ps1"), "-FailOnSkip") $root
Require-Step "stable JIT suite" $jit 'Stable tests \(JIT\): \d+ passed, 0 skipped, 0 failed'
$jitMatch = [regex]::Match($jit.text, 'Stable tests \(JIT\): (\d+) passed, 0 skipped, 0 failed')
$jitCount = [int]$jitMatch.Groups[1].Value

$installer = Invoke-Captured $powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "tools/sura_installer_smoke.ps1")) $root
Require-Step "installer smoke" $installer 'sura_installer_smoke: PASS'
$starter = Invoke-Captured $powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "tools/sura_starter_examples_smoke.ps1"), "-Engine", $engine) $root
Require-Step "starter examples" $starter 'starter_examples_smoke: PASS \(12/12\)'
$scaffold = Invoke-Captured $powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "tools/sura_pkg_scaffold_smoke.ps1"), "-Surapkg", $surapkg, "-Engine", $engine) $root
Require-Step "starter scaffold" $scaffold 'pkg_scaffold_smoke: PASS'
$vscode = Invoke-Captured $npm @("run", "check") (Join-Path $root "sura-vscode")
Require-Step "VS Code extension" $vscode 'sura-vscode smoke: PASS'
$website = Invoke-Captured $npm @("run", "build") (Join-Path $root "sura_presentation")
Require-Step "website production build" $website 'Build complete'

$store = Invoke-Captured $powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "tools/sura_store_msix.ps1")) $root
Require-Step "Store MSIX" $store 'sura_store_msix: PASS'
$storeReportPath = Join-Path $root "dist/store-msix/SuraLanguage-$Version.0-x64.msix.json"
$storeReport = [System.IO.File]::ReadAllText($storeReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json

$signatureReportPath = Join-Path $root "artifacts/windows_signature.json"
$signature = Invoke-Captured $powershell @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $root "tools/sura_windows_signature_gate.ps1"), "-JsonOut", $signatureReportPath) $root
Require-Step "Windows signature audit" $signature 'sura_windows_signature_gate: PASS'
$signatureReport = [System.IO.File]::ReadAllText($signatureReportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json

$targetAuditPath = Join-Path $root "build/target-lowering-$series.json"
if (-not (Test-Path -LiteralPath $targetAuditPath)) { throw "target lowering audit not found: $targetAuditPath" }
$targetAudit = [System.IO.File]::ReadAllText($targetAuditPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json

$priorVersions = @(Get-ChildItem -LiteralPath $public -Filter "verification-*.json" -File |
    Where-Object { $_.Name -ne "verification-$Version.json" } |
    ForEach-Object {
        $match = [regex]::Match($_.Name, '^verification-(\d+\.\d+\.\d+)\.json$')
        if ($match.Success) { [pscustomobject]@{ file = $_; version = [Version]$match.Groups[1].Value } }
    } |
    Sort-Object version -Descending)
if ($priorVersions.Count -eq 0) { throw "a prior public verification record is required for archived performance provenance" }
$prior = [System.IO.File]::ReadAllText($priorVersions[0].file.FullName, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$performanceRecord = $prior.performance_record
$performanceRecord.status = "archived performance record; not rerun for $Version"
$performanceRecord.reason = "This patch release changes JIT coverage, diagnostics, GC observability, editor integration, installer diagnostics, and release tooling; no new C++ comparison is claimed."

$verification = [ordered]@{
    schema = "sura.public.verification.v1"
    product = "Sura Language"
    version = $Version
    verified = [DateTime]::UtcNow.ToString("yyyy-MM-dd")
    engine_file = "SuraLanguage.exe"
    engine_sha256 = (Get-FileHash -LiteralPath $engine -Algorithm SHA256).Hash.ToLowerInvariant()
    engine_bytes = [int64](Get-Item -LiteralPath $engine).Length
    engine_locations_checked = @("repository build", "generated installer kit", "single-file installer smoke installation")
    package_helper = [ordered]@{
        file = "surapkg.exe"
        bytes = [int64](Get-Item -LiteralPath $surapkg).Length
        sha256 = (Get-FileHash -LiteralPath $surapkg -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    results = [ordered]@{
        stable_vm = "$vmCount/$vmCount PASS"
        stable_jit = "$jitCount/$jitCount PASS"
        core_vm = "$vmCount/$vmCount PASS"
        core_jit = "$jitCount/$jitCount PASS"
        bytecode_validation = "not rerun as a standalone suite; stable VM/JIT suites PASS"
        ffi_safety = "not rerun as a standalone suite; stable VM/JIT suites PASS"
        async_runtime_concurrency = "stable VM/JIT async cases PASS; dedicated async race regression retained"
        release_payload_identity = "installer smoke verified generated kit, locked-upgrade handling, single-file installation, execution, and uninstall"
        starter_examples = "12/12 parse, typecheck, and run PASS without optional dependencies"
        starter_scaffold = "surapkg new, generated package run, and generated package test PASS"
        target_lowering_audit = "$($targetAudit.status); JS full $($targetAudit.js.full) partial $($targetAudit.js.partial) ignored $($targetAudit.js.ignored); WASM full $($targetAudit.wasm.full) partial $($targetAudit.wasm.partial) ignored $($targetAudit.wasm.ignored)"
        installer_smoke = "locked executable rejection, single-file extract, quiet install, UTF-8 execution, and uninstall PASS"
        vscode_extension = "TypeScript compile, LSP client/fallback smoke, esbuild, and extension smoke PASS"
        website = "vinext production build PASS"
        windows_authenticode = "valid $($signatureReport.valid_count); unsigned $($signatureReport.unsigned_count); direct warning expected $($signatureReport.direct_download_warning_expected)"
        store_msix = "package content PASS; local loose-layout smoke $($storeReport.local_loose_layout_smoke_status)"
    }
    performance_record = $performanceRecord
}

$jsonPath = [System.IO.Path]::GetFullPath($JsonOut)
$parent = Split-Path -Parent $jsonPath
if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
[System.IO.File]::WriteAllText($jsonPath, ($verification | ConvertTo-Json -Depth 10), $utf8NoBom)

"sura_release_verification: PASS ($Version, VM=$vmCount, JIT=$jitCount)"
