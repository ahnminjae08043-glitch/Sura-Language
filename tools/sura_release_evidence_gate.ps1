param(
    [string]$ArtifactsDir = "artifacts",
    [string]$JsonOut = "",
    [string]$SummaryOut = "",
    [string[]]$RequiredBenchmarks = @(
        "bench_fib.sura",
        "bench_ai_schema.sura",
        "bench_rag_vector.sura",
        "bench_tool_routing.sura",
        "bench_policy_gate.sura",
        "bench_guardrail.sura",
        "bench_dependency_resolver.sura",
        "bench_workflow_scheduler.sura",
        "bench_telemetry_window.sura",
        "bench_fraud_scoring.sura",
        "bench_feature_flags.sura",
        "bench_physics.sura",
        "bench_physics_inplace.sura",
        "bench_physics3d.sura",
        "bench_market.sura"
    ),
    [string[]]$RequiredPythonComparisons = @(
        "fib(30)",
        "AI JSON/schema validation",
        "RAG vector ranking",
        "AI tool policy gate",
        "AI guardrail event scoring",
        "dependency resolver hot loop",
        "automation workflow scheduler",
        "telemetry rolling window",
        "payment fraud scoring",
        "feature flag rollout",
        "game physics Vec2 loop",
        "game physics in-place Vec2 loop",
        "game physics Vec3 loop",
        "market simulation objects"
    ),
    [string[]]$RequiredCoverageCategories = @(
        "cross_platform",
        "stable_engine",
        "package_ecosystem",
        "performance",
        "protected_release",
        "interop",
        "registry_security",
        "ai_stdlib_automation",
        "developer_tools",
        "goal_audit",
        "ci_coverage_gate"
    )
)

$ErrorActionPreference = "Stop"

function Resolve-OutputPath {
    param([string]$Path, [string]$Base)
    if ([string]::IsNullOrWhiteSpace($Path)) { return "" }
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $Base $Path
}

function Read-JsonFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "JSON file not found: $Path"
    }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
}

function Read-TextFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "text file not found: $Path"
    }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function As-Array {
    param($Items)
    if ($null -eq $Items) { return @() }
    if ($Items -is [string]) { return @($Items) }
    if ($Items -is [System.Collections.IEnumerable]) {
        $out = @()
        foreach ($item in $Items) { $out += $item }
        return $out
    }
    return @($Items)
}

function Expand-RequiredList {
    param([string[]]$Items)
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($item in (As-Array $Items)) {
        if ($null -eq $item) { continue }
        foreach ($part in ([string]$item -split ",")) {
            $trimmed = $part.Trim()
            if (-not [string]::IsNullOrWhiteSpace($trimmed)) {
                $out.Add($trimmed)
            }
        }
    }
    return @($out)
}

function Get-PropertyValue {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function Add-Check {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Name,
        [bool]$Passed,
        [string]$Message,
        [string]$Action
    )
    $Checks.Add([pscustomobject]@{
        name = $Name
        passed = $Passed
        message = $Message
        action = $Action
    })
}

function Add-Text-Check {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Name,
        [string]$Text,
        [string[]]$Patterns,
        [string]$Action
    )
    foreach ($pattern in $Patterns) {
        Add-Check $Checks "$Name contains $pattern" ($Text -match [regex]::Escape($pattern)) "required text: $pattern" $Action
    }
}

$root = (Resolve-Path -LiteralPath ".").Path
$artifactRoot = Resolve-OutputPath $ArtifactsDir $root
if (-not (Test-Path -LiteralPath $artifactRoot)) {
    throw "artifacts directory not found: $artifactRoot"
}

if ([string]::IsNullOrWhiteSpace($JsonOut)) {
    $JsonOut = Join-Path $artifactRoot "release_evidence.json"
} else {
    $JsonOut = Resolve-OutputPath $JsonOut $root
}
if ([string]::IsNullOrWhiteSpace($SummaryOut)) {
    $SummaryOut = Join-Path $artifactRoot "release_evidence.md"
} else {
    $SummaryOut = Resolve-OutputPath $SummaryOut $root
}

$artifactFiles = @(
    @{ key = "dashboard_html"; path = "bench_dashboard.html"; kind = "html" },
    @{ key = "dashboard_json"; path = "bench_dashboard.json"; kind = "json" },
    @{ key = "benchmark_summary"; path = "bench_summary.md"; kind = "markdown" },
    @{ key = "benchmark_release_notes"; path = "bench_release_notes.md"; kind = "markdown" },
    @{ key = "benchmark_history"; path = "bench_history.json"; kind = "json" },
    @{ key = "ci_coverage"; path = "ci_coverage_gate.json"; kind = "json" },
    @{ key = "version_contract"; path = "version_contract.json"; kind = "json" },
    @{ key = "compatibility_contract"; path = "compatibility_report.json"; kind = "json" },
    @{ key = "windows_signature"; path = "windows_signature.json"; kind = "json" },
    @{ key = "native_performance"; path = "native_perf.json"; kind = "json" },
    @{ key = "native_performance_summary"; path = "native_perf.md"; kind = "markdown" },
    @{ key = "target_lowering_audit"; path = "target_lowering_audit.json"; kind = "json" },
    @{ key = "target_lowering_audit_summary"; path = "target_lowering_audit.md"; kind = "markdown" },
    @{ key = "goal_audit"; path = "goal_audit.json"; kind = "json" },
    @{ key = "goal_audit_summary"; path = "goal_audit.md"; kind = "markdown" },
    @{ key = "security_audit_handoff"; path = "security_audit_bundle.json"; kind = "json" },
    @{ key = "security_audit_handoff_summary"; path = "security_audit_bundle.md"; kind = "markdown" },
    @{ key = "security_audit_handoff_archive"; path = "sura-security-audit-1.11.1.zip"; kind = "zip" }
)

$checks = New-Object System.Collections.Generic.List[object]
$artifacts = New-Object System.Collections.Generic.List[object]

foreach ($spec in $artifactFiles) {
    $full = Join-Path $artifactRoot $spec.path
    $exists = Test-Path -LiteralPath $full
    Add-Check $checks "artifact exists: $($spec.path)" $exists "required release evidence artifact" "generate and keep $($spec.path) before uploading release evidence"
    $size = 0
    $hash = ""
    if ($exists) {
        $item = Get-Item -LiteralPath $full
        $size = $item.Length
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash
        Add-Check $checks "artifact non-empty: $($spec.path)" ($size -gt 0) "artifact size: $size" "regenerate $($spec.path)"
    }
    $artifacts.Add([pscustomobject]@{
        key = $spec.key
        path = $spec.path
        kind = $spec.kind
        exists = $exists
        bytes = $size
        sha256 = $hash
    })
}

$dashboardJsonPath = Join-Path $artifactRoot "bench_dashboard.json"
$summaryPath = Join-Path $artifactRoot "bench_summary.md"
$releaseNotesPath = Join-Path $artifactRoot "bench_release_notes.md"
$htmlPath = Join-Path $artifactRoot "bench_dashboard.html"
$historyPath = Join-Path $artifactRoot "bench_history.json"
$coveragePath = Join-Path $artifactRoot "ci_coverage_gate.json"
$versionContractPath = Join-Path $artifactRoot "version_contract.json"
$compatibilityContractPath = Join-Path $artifactRoot "compatibility_report.json"
$windowsSignaturePath = Join-Path $artifactRoot "windows_signature.json"
$nativePerfPath = Join-Path $artifactRoot "native_perf.json"
$nativePerfSummaryPath = Join-Path $artifactRoot "native_perf.md"
$targetLoweringPath = Join-Path $artifactRoot "target_lowering_audit.json"
$targetLoweringSummaryPath = Join-Path $artifactRoot "target_lowering_audit.md"
$goalAuditPath = Join-Path $artifactRoot "goal_audit.json"
$goalAuditSummaryPath = Join-Path $artifactRoot "goal_audit.md"
$securityAuditPath = Join-Path $artifactRoot "security_audit_bundle.json"
$securityAuditSummaryPath = Join-Path $artifactRoot "security_audit_bundle.md"
$securityAuditZipPath = Join-Path $artifactRoot "sura-security-audit-1.11.1.zip"

$dashboard = $null
$nativePerf = $null
if (Test-Path -LiteralPath $dashboardJsonPath) {
    $dashboard = Read-JsonFile $dashboardJsonPath
    Add-Check $checks "dashboard generated timestamp" (-not [string]::IsNullOrWhiteSpace([string](Get-PropertyValue $dashboard "generated_utc"))) "bench_dashboard.json has generated_utc" "regenerate benchmark dashboard JSON"
    Add-Check $checks "dashboard summary present" ($null -ne (Get-PropertyValue $dashboard "summary")) "bench_dashboard.json has summary" "regenerate benchmark dashboard JSON"
    Add-Check $checks "dashboard release notes present" ($null -ne (Get-PropertyValue $dashboard "release_notes")) "bench_dashboard.json has release_notes" "regenerate benchmark dashboard JSON with -ReleaseNotesOut"

    $benchNames = @((As-Array (Get-PropertyValue $dashboard "benchmarks")) | ForEach-Object { [string](Get-PropertyValue $_ "benchmark") })
    foreach ($required in (Expand-RequiredList $RequiredBenchmarks)) {
        Add-Check $checks "required benchmark: $required" ($benchNames -contains $required) "benchmark evidence must include $required" "restore $required to the dashboard benchmark set"
    }

    $pythonLabels = @((As-Array (Get-PropertyValue $dashboard "python_comparisons")) | ForEach-Object { [string](Get-PropertyValue $_ "label") })
    foreach ($required in (Expand-RequiredList $RequiredPythonComparisons)) {
        Add-Check $checks "required Python comparison: $required" ($pythonLabels -contains $required) "Python comparison evidence must include $required" "restore $required to the dashboard Python comparison set"
    }

    $summary = Get-PropertyValue $dashboard "summary"
    $benchCount = [int](Get-PropertyValue $summary "benchmark_count")
    $pyCount = [int](Get-PropertyValue $summary "python_comparison_count")
    Add-Check $checks "dashboard benchmark count" ($benchCount -ge (Expand-RequiredList $RequiredBenchmarks).Count) "benchmark_count=$benchCount" "regenerate benchmark dashboard with required benchmarks"
    Add-Check $checks "dashboard Python comparison count" ($pyCount -ge (Expand-RequiredList $RequiredPythonComparisons).Count) "python_comparison_count=$pyCount" "regenerate benchmark dashboard with required Python comparisons"
}

if (Test-Path -LiteralPath $summaryPath) {
    $summaryText = Read-TextFile $summaryPath
    Add-Text-Check $checks "benchmark summary" $summaryText @("# Sura Benchmark Summary", "Average JIT speedup", "Python Comparison") "regenerate bench_summary.md"
}

if (Test-Path -LiteralPath $releaseNotesPath) {
    $releaseText = Read-TextFile $releaseNotesPath
    Add-Text-Check $checks "benchmark release notes" $releaseText @("# Sura Benchmark Release Notes", "Faster-than-Python cases", "Python Comparison Highlights", "Dashboard JSON includes") "regenerate bench_release_notes.md"
}

if (Test-Path -LiteralPath $htmlPath) {
    $htmlText = Read-TextFile $htmlPath
    Add-Text-Check $checks "benchmark dashboard HTML" $htmlText @("<svg", "JIT Speedup Chart", "Benchmark Summary") "regenerate bench_dashboard.html"
}

if (Test-Path -LiteralPath $historyPath) {
    $history = Read-JsonFile $historyPath
    $entries = @(As-Array (Get-PropertyValue $history "entries"))
    Add-Check $checks "benchmark history entries" ($entries.Count -gt 0) "history entries=$($entries.Count)" "regenerate bench_history.json"
    if ($dashboard -and $entries.Count -gt 0) {
        $generated = [string](Get-PropertyValue $dashboard "generated_utc")
        $found = $false
        foreach ($entry in $entries) {
            if ([string](Get-PropertyValue $entry "generated_utc") -eq $generated) {
                $found = $true
                break
            }
        }
        Add-Check $checks "benchmark history includes current dashboard" $found "history should include $generated" "rerun dashboard with -HistoryIn/-HistoryOut"
    }
}

if (Test-Path -LiteralPath $coveragePath) {
    $coverage = Read-JsonFile $coveragePath
    Add-Check $checks "CI coverage schema" (([string](Get-PropertyValue $coverage "schema")) -eq "sura.ci.coverage_gate.v1") "ci_coverage_gate.json schema" "rerun sura_ci_coverage_gate.ps1"
    Add-Check $checks "CI coverage passed" ([bool](Get-PropertyValue $coverage "passed")) "ci coverage must pass" "fix CI coverage gate failures before release upload"
    $coverageCategories = As-Array (Get-PropertyValue $coverage "categories")
    foreach ($required in (Expand-RequiredList $RequiredCoverageCategories)) {
        $match = $coverageCategories | Where-Object { [string](Get-PropertyValue $_ "name") -eq $required -and [string](Get-PropertyValue $_ "status") -eq "PASS" } | Select-Object -First 1
        Add-Check $checks "CI coverage category: $required" ($null -ne $match) "required CI coverage category must pass: $required" "restore CI coverage category $required"
    }
}

if (Test-Path -LiteralPath $versionContractPath) {
    $versionContract = Read-JsonFile $versionContractPath
    $sourceVersion = Read-JsonFile (Join-Path $root "version.json")
    Add-Check $checks "version contract schema" (([string](Get-PropertyValue $versionContract "schema")) -eq "sura.version.contract.report.v1") "version_contract.json schema" "rerun sura_version_sync.ps1"
    Add-Check $checks "version contract passed" (([string](Get-PropertyValue $versionContract "status")) -eq "pass") "version contract must pass" "fix release version drift before upload"
    Add-Check $checks "version contract release" (([string](Get-PropertyValue $versionContract "version")) -eq ([string](Get-PropertyValue $sourceVersion "version"))) "version contract must match version.json" "rerun sura_version_sync.ps1 after rebuilding release artifacts"
}

if (Test-Path -LiteralPath $compatibilityContractPath) {
    $compatibilityContract = Read-JsonFile $compatibilityContractPath
    $sourceVersion = Read-JsonFile (Join-Path $root "version.json")
    Add-Check $checks "compatibility contract schema" (([string](Get-PropertyValue $compatibilityContract "schema")) -eq "sura.compatibility.report.v1") "compatibility_report.json schema" "rerun sura_compatibility_gate.ps1"
    Add-Check $checks "compatibility contract passed" ([bool](Get-PropertyValue $compatibilityContract "passed")) "compatibility source, bytecode, and ABI checks must pass" "fix compatibility regressions before release upload"
    Add-Check $checks "compatibility contract release" (([string](Get-PropertyValue $compatibilityContract "language_version")) -eq ([string](Get-PropertyValue $sourceVersion "version"))) "compatibility report must match version.json" "rerun compatibility gate with the release engine"
    $compatFixtures = @(As-Array (Get-PropertyValue $compatibilityContract "fixtures"))
    Add-Check $checks "compatibility fixture coverage" ($compatFixtures.Count -ge 3) "core, object, and stdlib compatibility fixtures must run" "restore the 1.11 compatibility fixture set"
    $historicalProbes = @(As-Array (Get-PropertyValue $compatibilityContract "historical_probes"))
    Add-Check $checks "historical compatibility coverage" ($historicalProbes.Count -ge 1) "at least one pre-guarantee runtime series must have explicit verification-only evidence" "rerun the compatibility gate with the archived runtime probe"
    $historicalCurrentFailures = @($historicalProbes | Where-Object {
        [string](Get-PropertyValue (Get-PropertyValue $_ "current_runtime") "status") -ne "passed" -or
        @(As-Array (Get-PropertyValue (Get-PropertyValue $_ "current_runtime") "fixtures")).Count -lt 3
    })
    Add-Check $checks "historical fixtures on current runtime" ($historicalProbes.Count -ge 1 -and $historicalCurrentFailures.Count -eq 0) "historical core, object, and stdlib probes must pass on the release runtime" "fix the historical source regression before upload"
    $historicalArchiveFailures = @($historicalProbes | Where-Object {
        [string](Get-PropertyValue (Get-PropertyValue $_ "archived_runtime") "status") -ne "passed" -or
        [string](Get-PropertyValue (Get-PropertyValue $_ "archived_runtime") "bytecode_forward_load") -ne "passed"
    })
    Add-Check $checks "archived runtime and bytecode evidence" ($historicalProbes.Count -ge 1 -and $historicalArchiveFailures.Count -eq 0) "the signed/hash-checked archived Windows runtime must execute the probes and its bytecode must load in the release runtime" "run compatibility evidence on Windows x64 before upload"
}

if (Test-Path -LiteralPath $windowsSignaturePath) {
    $signatureReport = Read-JsonFile $windowsSignaturePath
    $signatureFiles = @(As-Array (Get-PropertyValue $signatureReport "files"))
    $unsignedCount = [int](Get-PropertyValue $signatureReport "unsigned_count")
    Add-Check $checks "Windows signature schema" (([string](Get-PropertyValue $signatureReport "schema")) -eq "sura.windows.signature.report.v1") "windows_signature.json schema" "rerun sura_windows_signature_gate.ps1"
    Add-Check $checks "Windows signature audit passed" (([string](Get-PropertyValue $signatureReport "status")) -eq "pass") "signature audit must contain no invalid signatures or manifest mismatch" "fix invalid signatures or public manifest drift"
    Add-Check $checks "Windows signature file inventory" ($signatureFiles.Count -ge 3) "engine, package manager, and direct installer signatures must be audited" "rerun signature gate with all Windows executables"
    Add-Check $checks "unsigned direct-download action recorded" ($unsignedCount -eq 0 -or -not [string]::IsNullOrWhiteSpace([string](Get-PropertyValue $signatureReport "next_action"))) "unsigned releases must record the Store or trusted-certificate next action" "record the signing or Store publication action"
}

if (Test-Path -LiteralPath $nativePerfPath) {
    $nativePerf = Read-JsonFile $nativePerfPath
    Add-Check $checks "native performance schema" (([string](Get-PropertyValue $nativePerf "schema")) -eq "sura.native.performance.v1") "native_perf.json schema" "rerun sura_native_perf_baseline.ps1"
    Add-Check $checks "native performance passed" ([bool](Get-PropertyValue $nativePerf "passed")) "native performance baseline must run successfully" "fix native baseline generation before release upload"
    Add-Check $checks "native performance Sura timing" ([double](Get-PropertyValue $nativePerf "sura_jit_ms") -gt 0) "sura_jit_ms must be positive" "rerun native baseline with SuraLanguage"
    Add-Check $checks "native performance C++ timing" ([double](Get-PropertyValue $nativePerf "native_ms") -gt 0) "native_ms must be positive" "rerun native baseline with a C++ compiler"
    Add-Check $checks "native performance ratio" ([double](Get-PropertyValue $nativePerf "sura_native_ratio") -gt 0) "sura_native_ratio must be positive" "rerun native baseline and keep ratio evidence"
    $scope = Get-PropertyValue $nativePerf "measurement_scope"
    $scopeSteps = if ($scope) { Get-PropertyValue $scope "steps" } else { $null }
    $scopeRegion = if ($scope) { [string](Get-PropertyValue $scope "timed_region") } else { "" }
    Add-Check $checks "native performance fair scope" ([bool](Get-PropertyValue $nativePerf "fair_scope_passed") -and [int]$scopeSteps -eq 100000 -and $scopeRegion -eq "inner physics loop only") "native baseline must prove Sura and C++ timed the same inner physics loop scope" "rerun sura_native_perf_baseline.ps1 after keeping the shared 100k physics loop workload"
    $nativeBaselines = @(As-Array (Get-PropertyValue $nativePerf "baselines"))
    $native3d = $nativeBaselines | Where-Object {
        [string](Get-PropertyValue $_ "id") -eq "vec3" -or
        [string](Get-PropertyValue $_ "dimension") -eq "vec3" -or
        [string](Get-PropertyValue $_ "benchmark") -match "Vec3|3D"
    } | Select-Object -First 1
    $native3dScope = if ($native3d) { Get-PropertyValue $native3d "measurement_scope" } else { $null }
    $native3dSteps = if ($native3dScope) { Get-PropertyValue $native3dScope "steps" } else { $null }
    $native3dRegion = if ($native3dScope) { [string](Get-PropertyValue $native3dScope "timed_region") } else { "" }
    $native3dOk = ($null -ne $native3d -and
        [bool](Get-PropertyValue $native3d "fair_scope_passed") -and
        [int]$native3dSteps -eq 100000 -and
        $native3dRegion -eq "inner physics loop only" -and
        [double](Get-PropertyValue $native3d "sura_jit_ms") -gt 0 -and
        [double](Get-PropertyValue $native3d "native_ms") -gt 0 -and
        [double](Get-PropertyValue $native3d "sura_native_ratio") -gt 0)
    Add-Check $checks "native performance 3D fair scope" $native3dOk "native_perf.json must include a fair-scope 3D Vec3 C++ baseline" "rerun sura_native_perf_baseline.ps1 after keeping bench_physics3d.sura aligned with the C++ Vec3 loop"
    $dashboardNative = if ($dashboard) { Get-PropertyValue $dashboard "native_performance" } else { $null }
    Add-Check $checks "dashboard native performance present" ($null -ne $dashboardNative) "bench_dashboard.json must surface native_perf.json evidence" "regenerate benchmark dashboard with -NativePerfIn native_perf.json"
    if ($null -ne $dashboardNative) {
        $artifactRatio = [double](Get-PropertyValue $nativePerf "sura_native_ratio")
        $dashboardRatio = [double](Get-PropertyValue $dashboardNative "sura_native_ratio")
        $ratioTolerance = [Math]::Max(0.001, [Math]::Abs($artifactRatio) * 0.02)
        Add-Check $checks "dashboard native performance ratio" ($dashboardRatio -gt 0) "dashboard native ratio must be positive" "regenerate benchmark dashboard with native performance evidence"
        Add-Check $checks "dashboard native performance matches artifact" ([Math]::Abs($artifactRatio - $dashboardRatio) -le $ratioTolerance) "dashboard native ratio should match native_perf.json" "regenerate dashboard after native performance baseline"
        Add-Check $checks "dashboard native performance 3D present" ([bool](Get-PropertyValue $dashboardNative "native_3d_available")) "bench_dashboard.json must surface native_perf.json 3D baseline evidence" "regenerate benchmark dashboard with the updated native_perf.json"
    }
    if (Test-Path -LiteralPath $htmlPath) {
        Add-Text-Check $checks "benchmark dashboard native HTML" (Read-TextFile $htmlPath) @("Native C++ Baseline", "Sura/native ratio", "game physics Vec3 loop") "regenerate bench_dashboard.html with -NativePerfIn"
    }
    if (Test-Path -LiteralPath $summaryPath) {
        Add-Text-Check $checks "benchmark summary native evidence" (Read-TextFile $summaryPath) @("Native C++ Baseline", "Sura/native ratio", "Native C++ 3D comparison") "regenerate bench_summary.md with -NativePerfIn"
    }
    if (Test-Path -LiteralPath $releaseNotesPath) {
        Add-Text-Check $checks "benchmark release native evidence" (Read-TextFile $releaseNotesPath) @("Native C++ Baseline", "Sura/native ratio", "Native C++ 3D baseline") "regenerate bench_release_notes.md with -NativePerfIn"
    }
}

if (Test-Path -LiteralPath $nativePerfSummaryPath) {
    $nativePerfText = Read-TextFile $nativePerfSummaryPath
    Add-Text-Check $checks "native performance summary" $nativePerfText @("# Sura Native Performance Baseline", "Sura/native ratio", "3D Sura/native ratio", "Timed region", "C++ flags", "Fair scope check") "regenerate native_perf.md"
}

if (Test-Path -LiteralPath $targetLoweringPath) {
    $targetLowering = Read-JsonFile $targetLoweringPath
    Add-Check $checks "target lowering audit schema" (([string](Get-PropertyValue $targetLowering "schema")) -eq "sura.target.lowering_audit.v1") "target_lowering_audit.json schema" "rerun sura_target_lowering_audit.ps1"
    Add-Check $checks "target lowering audit AST count" ([int](Get-PropertyValue $targetLowering "ast_node_count") -ge 40) "target lowering audit maps AST node coverage" "rerun sura_target_lowering_audit.ps1 after keeping ast.hpp coverage mapped"
    $pipeline = Get-PropertyValue $targetLowering "pipeline"
    Add-Check $checks "target lowering audit frontier visibility" ($null -ne $pipeline -and $null -ne (Get-PropertyValue $pipeline "ast_or_bytecode_frontend")) "target lowering audit records whether JS/WASM use AST/bytecode frontends" "rerun sura_target_lowering_audit.ps1"
}
if (Test-Path -LiteralPath $targetLoweringSummaryPath) {
    Add-Text-Check $checks "target lowering audit summary" (Read-TextFile $targetLoweringSummaryPath) @("# Sura Target Lowering Audit", "Full AST/bytecode frontend", "Partial Or Missing Nodes") "regenerate target_lowering_audit.md"
}

if (Test-Path -LiteralPath $goalAuditPath) {
    $goalAudit = Read-JsonFile $goalAuditPath
    Add-Check $checks "goal audit schema" (([string](Get-PropertyValue $goalAudit "schema")) -eq "sura.goal.audit.v1") "goal_audit.json schema" "rerun sura_goal_audit.ps1"
    $progress = [double](Get-PropertyValue $goalAudit "progress_percent")
    Add-Check $checks "goal audit progress recorded" ($progress -gt 0 -and $progress -le 100) "progress_percent=$progress" "rerun sura_goal_audit.ps1"
    Add-Check $checks "goal audit check count" ([int](Get-PropertyValue $goalAudit "required_count") -ge 45) "required_count=$([int](Get-PropertyValue $goalAudit "required_count"))" "keep the goal audit mapped to the full active goal including non-negotiable frontier work"
    Add-Check $checks "goal audit categories recorded" (@(As-Array (Get-PropertyValue $goalAudit "categories")).Count -ge 7) "goal categories must be present" "restore goal audit category coverage"
    $goalPassed = [bool](Get-PropertyValue $goalAudit "passed")
    $remaining = @(As-Array (Get-PropertyValue $goalAudit "remaining_work"))
    Add-Check $checks "goal audit remaining work recorded" ($goalPassed -or $remaining.Count -gt 0) "remaining_work count=$($remaining.Count)" "record remaining work when the world-class goal is not complete"
}

if (Test-Path -LiteralPath $goalAuditSummaryPath) {
    $goalAuditText = Read-TextFile $goalAuditSummaryPath
    Add-Text-Check $checks "goal audit summary" $goalAuditText @("# Sura Goal Audit", "Progress:", "Remaining Work") "regenerate goal_audit.md"
}

if (Test-Path -LiteralPath $securityAuditPath) {
    $securityAudit = Read-JsonFile $securityAuditPath
    Add-Check $checks "security audit handoff schema" (([string](Get-PropertyValue $securityAudit "schema")) -eq "sura.security.audit.bundle.report.v1") "security_audit_bundle.json schema" "rerun sura_security_audit_bundle.ps1"
    Add-Check $checks "security audit handoff status" (([string](Get-PropertyValue $securityAudit "status")) -eq "HANDOFF_CREATED") "handoff generator must complete" "rerun sura_security_audit_bundle.ps1"
    Add-Check $checks "security audit honest status" (-not [bool](Get-PropertyValue $securityAudit "independent_external_audit_performed")) "no independent external audit is currently recorded" "do not claim an independent audit without its report"
    Add-Check $checks "security audit source inventory" ([int](Get-PropertyValue $securityAudit "source_file_count") -ge 100) "hashed source inventory must be present" "restore the complete security review handoff inventory"
    if (Test-Path -LiteralPath $securityAuditZipPath) {
        $archive = Get-PropertyValue $securityAudit "archive"
        $expectedHash = [string](Get-PropertyValue $archive "sha256")
        $actualHash = (Get-FileHash -LiteralPath $securityAuditZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        Add-Check $checks "security audit handoff archive hash" ($expectedHash -eq $actualHash) "report archive SHA-256 must match uploaded ZIP" "regenerate the handoff report and ZIP together"
    }
}
if (Test-Path -LiteralPath $securityAuditSummaryPath) {
    Add-Text-Check $checks "security audit handoff summary" (Read-TextFile $securityAuditSummaryPath) @("Independent external audit performed: no", "not a security certification", "does not prove the absence") "regenerate security_audit_bundle.md without unsupported assurance claims"
}

$failed = @($checks | Where-Object { -not $_.passed })
$nextActions = @($failed | ForEach-Object { $_.action } | Select-Object -Unique)
$checkArray = @()
foreach ($check in $checks) { $checkArray += $check }
$artifactArray = @()
foreach ($artifact in $artifacts) { $artifactArray += $artifact }

$report = [ordered]@{
    schema = "sura.release.evidence_gate.v1"
    generated_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    artifacts_dir = $artifactRoot
    passed = ($failed.Count -eq 0)
    required_count = $checks.Count
    passed_count = ($checks.Count - $failed.Count)
    failed_count = $failed.Count
    artifacts = $artifactArray
    checks = $checkArray
    next_actions = $nextActions
}

$jsonParent = Split-Path -Parent $JsonOut
if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
[System.IO.File]::WriteAllText($JsonOut, ($report | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))

$summaryLines = New-Object System.Collections.Generic.List[string]
$summaryGeneratedUtc = $report["generated_utc"]
$summaryStatus = if ($report["passed"]) { "PASS" } else { "FAIL" }
$summaryPassedCount = $report["passed_count"]
$summaryRequiredCount = $report["required_count"]
$summaryLines.Add("# Sura Release Evidence")
$summaryLines.Add("")
$summaryLines.Add("Generated UTC: $summaryGeneratedUtc")
$summaryLines.Add("Status: $summaryStatus")
$summaryLines.Add("Checks: $summaryPassedCount/$summaryRequiredCount passed")
$summaryLines.Add("")
$summaryLines.Add("## Artifacts")
$tick = [char]96
foreach ($artifact in $artifactArray) {
    $summaryLines.Add(("- {0}{1}{0}: {2} bytes, sha256 {0}{3}{0}" -f $tick, $artifact.path, $artifact.bytes, $artifact.sha256))
}
$summaryLines.Add("")
$summaryLines.Add("## Next Actions")
if ($nextActions.Count -eq 0) {
    $summaryLines.Add("- none")
} else {
    foreach ($action in $nextActions) {
        $summaryLines.Add("- $action")
    }
}
$summaryParent = Split-Path -Parent $SummaryOut
if ($summaryParent) { New-Item -ItemType Directory -Force -Path $summaryParent | Out-Null }
[System.IO.File]::WriteAllText($SummaryOut, ($summaryLines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))

if ($failed.Count -gt 0) {
    foreach ($item in $failed) {
        [Console]::Error.WriteLine("[release evidence gate] $($item.name): $($item.message)")
    }
    throw "Release evidence gate failed with $($failed.Count) finding(s)"
}

Write-Host ("release_evidence_gate: PASS ({0}/{1} checks)" -f $report["passed_count"], $report["required_count"])
