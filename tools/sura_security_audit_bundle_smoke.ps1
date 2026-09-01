param(
    [string]$RepoRoot = ".",
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$generator = Join-Path $root "tools/sura_security_audit_bundle.ps1"
if (-not (Test-Path -LiteralPath $generator -PathType Leaf)) {
    throw "security audit bundle generator not found: $generator"
}
if ([string]::IsNullOrWhiteSpace($Engine)) {
    $engineName = if ($env:OS -eq "Windows_NT") { "SuraLanguage.exe" } else { "SuraLanguage" }
    $candidate = Join-Path $root $engineName
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $Engine = $candidate }
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_security_audit_bundle_" + [Guid]::NewGuid().ToString("N"))
$bundle = Join-Path $temp "bundle"
$zip = Join-Path $temp "bundle.zip"
$reportPath = Join-Path $temp "report.json"
$summaryPath = Join-Path $temp "summary.md"

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $arguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $generator,
        "-RepoRoot", $root,
        "-OutDir", $bundle,
        "-ZipOut", $zip,
        "-ReportOut", $reportPath,
        "-SummaryOut", $summaryPath
    )
    if (-not [string]::IsNullOrWhiteSpace($Engine)) {
        $arguments += @("-Engine", $Engine)
    }
    $hostExecutable = (Get-Process -Id $PID).Path
    $output = & $hostExecutable @arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch "sura_security_audit_bundle: PASS") {
        $output | Write-Host
        throw "security audit bundle generation failed"
    }

    foreach ($required in @(
        $zip,
        $reportPath,
        $summaryPath,
        (Join-Path $bundle "manifest.json"),
        (Join-Path $bundle "README.md"),
        (Join-Path $bundle "review/SECURITY.md"),
        (Join-Path $bundle "review/SECURITY_AUDIT.md"),
        (Join-Path $bundle "review/onnx_weights.hpp"),
        (Join-Path $bundle "review/tests/71_onnx_execution.sura")
    )) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "audit bundle required file is missing: $required"
        }
    }

    $report = [System.IO.File]::ReadAllText($reportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $manifestPath = Join-Path $bundle "manifest.json"
    $manifest = [System.IO.File]::ReadAllText($manifestPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($report.schema -ne "sura.security.audit.bundle.report.v1" -or
        $report.status -ne "HANDOFF_CREATED" -or
        $report.independent_external_audit_performed -ne $false -or
        $manifest.schema -ne "sura.security.audit.handoff.v1" -or
        $manifest.independent_external_audit.performed -ne $false -or
        $manifest.independent_external_audit.report_in_repository -ne $false -or
        $manifest.independent_external_audit.certification_claimed -ne $false -or
        $manifest.source_file_count -lt 100) {
        throw "audit bundle report or manifest contract is invalid"
    }
    if ($report.archive.sha256 -ne (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() -or
        $report.manifest.sha256 -ne (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw "audit bundle report hash does not match its artifact"
    }

    $requiredPaths = @(
        "SECURITY.md", "SECURITY_AUDIT.md", "compatibility.json", "main.cpp",
        "jit_vm.hpp", "onnx_weights.hpp", "sura_ffi.cpp", "surapkg.cpp",
        "tests/parser_untrusted_input_test.cpp", "tests/71_onnx_execution.sura",
        "tools/sura_untrusted_input_smoke.ps1"
    )
    $entriesByPath = @{}
    foreach ($entry in @($manifest.source_files)) { $entriesByPath[[string]$entry.path] = $entry }
    foreach ($relative in $requiredPaths) {
        if (-not $entriesByPath.ContainsKey($relative)) {
            throw "audit bundle manifest is missing source: $relative"
        }
    }
    foreach ($entry in @($manifest.source_files)) {
        $copy = Join-Path (Join-Path $bundle "review") ([string]$entry.path).Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        if (-not (Test-Path -LiteralPath $copy -PathType Leaf)) {
            throw "manifest source copy is missing: $($entry.path)"
        }
        $item = Get-Item -LiteralPath $copy
        $hash = (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([int64]$entry.bytes -ne [int64]$item.Length -or
            [string]$entry.sha256 -ne $hash) {
            throw "manifest integrity mismatch: $($entry.path)"
        }
        if ([string]$entry.path -match '(^|/)(\.env|registry)(/|$)' -or [string]$entry.path -match '\.(exe|dll|pfx)$') {
            throw "audit bundle contains forbidden secret/data/binary path: $($entry.path)"
        }
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $zipNames = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        foreach ($required in @("manifest.json", "README.md", "review/SECURITY.md", "review/main.cpp")) {
            if ($zipNames -notcontains $required) { throw "archive is missing entry: $required" }
        }
        if (@($zipNames | Where-Object { $_ -match '\.(exe|dll|pfx)$' }).Count -ne 0) {
            throw "archive contains a forbidden binary or signing-key file"
        }
    } finally {
        $archive.Dispose()
    }

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $second = & $hostExecutable @arguments 2>&1 | ForEach-Object { "$_" }
        $secondCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($secondCode -eq 0 -or ($second -join "`n") -notmatch "already exists") {
        throw "audit bundle generator must reject accidental output replacement without -Force"
    }

    $summary = [System.IO.File]::ReadAllText($summaryPath, [System.Text.Encoding]::UTF8)
    foreach ($fact in @(
        "Independent external audit performed: no",
        "not a security certification",
        "does not prove the absence"
    )) {
        if (-not $summary.Contains($fact)) { throw "audit summary is missing factual boundary: $fact" }
    }

    Write-Host ("sura_security_audit_bundle_smoke: PASS ({0} source files)" -f $manifest.source_file_count)
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

# The last check above is a negative test, so this script printed PASS while
# inheriting its nonzero exit code. State the verdict explicitly.
exit 0
