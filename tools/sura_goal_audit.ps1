param(
    [string]$RepoRoot = ".",
    [string]$NativePerfJson = "artifacts\native_perf.json",
    [string]$JsonOut = "artifacts\goal_audit.json",
    [string]$SummaryOut = "artifacts\goal_audit.md",
    [double]$MaxNativeRatio = 10.0,
    [int]$MaxNativeEvidenceAgeHours = 168,
    [switch]$FailOnIncomplete
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$Path)
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-OutputPath {
    param([string]$Path, [string]$Base)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $Base $Path
}

function Read-Text {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "file not found: $Path"
    }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Read-JsonFile {
    param([string]$Path)
    return (Read-Text $Path) | ConvertFrom-Json
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

function Get-PropertyValue {
    param($Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function Convert-ToFiniteDouble {
    param($Value)
    if ($null -eq $Value) { return $null }
    try {
        $number = [Convert]::ToDouble($Value, [System.Globalization.CultureInfo]::InvariantCulture)
    } catch {
        return $null
    }
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { return $null }
    return $number
}

function Test-NumericNear {
    param(
        $Left,
        $Right,
        [double]$RelativeTolerance = 0.000000001,
        [double]$AbsoluteTolerance = 0.000000001
    )
    if ($null -eq $Left -or $null -eq $Right) { return $false }
    $leftNumber = Convert-ToFiniteDouble $Left
    $rightNumber = Convert-ToFiniteDouble $Right
    if ($null -eq $leftNumber -or $null -eq $rightNumber) { return $false }
    $difference = [Math]::Abs($leftNumber - $rightNumber)
    $scale = [Math]::Max([Math]::Abs($leftNumber), [Math]::Abs($rightNumber))
    return $difference -le [Math]::Max($AbsoluteTolerance, $RelativeTolerance * $scale)
}

function Test-NativeBaselineIntegrity {
    param(
        $Baseline,
        [string]$ExpectedId,
        [string]$ExpectedDimension
    )

    $problems = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Baseline) {
        $problems.Add("missing $ExpectedId baseline")
        return [pscustomobject]@{
            passed = $false
            problems = @($problems)
            sura_ms = $null
            native_ms = $null
            ratio = $null
            run_count = 0
        }
    }

    if ([string](Get-PropertyValue $Baseline "id") -ne $ExpectedId) {
        $problems.Add("baseline id is not $ExpectedId")
    }
    if ([string](Get-PropertyValue $Baseline "dimension") -ne $ExpectedDimension) {
        $problems.Add("$ExpectedId dimension is not $ExpectedDimension")
    }
    if (-not [bool](Get-PropertyValue $Baseline "passed")) {
        $problems.Add("$ExpectedId baseline did not pass")
    }
    if (-not [bool](Get-PropertyValue $Baseline "fair_scope_passed")) {
        $problems.Add("$ExpectedId fair-scope check did not pass")
    }

    $scope = Get-PropertyValue $Baseline "measurement_scope"
    $scopeSteps = Convert-ToFiniteDouble (Get-PropertyValue $scope "steps")
    $scopeRegion = [string](Get-PropertyValue $scope "timed_region")
    $scopeSuraRuns = Convert-ToFiniteDouble (Get-PropertyValue $scope "sura_runs")
    $scopeNativeRuns = Convert-ToFiniteDouble (Get-PropertyValue $scope "native_runs")
    if ($null -eq $scopeSteps -or $scopeSteps -ne 100000 -or $scopeRegion -ne "inner physics loop only") {
        $problems.Add("$ExpectedId measurement scope is not the 100k inner physics loop")
    }

    $suraMs = Convert-ToFiniteDouble (Get-PropertyValue $Baseline "sura_jit_ms")
    $nativeMs = Convert-ToFiniteDouble (Get-PropertyValue $Baseline "native_ms")
    $ratio = Convert-ToFiniteDouble (Get-PropertyValue $Baseline "sura_native_ratio")
    if ($null -eq $suraMs -or $suraMs -le 0 -or $null -eq $nativeMs -or $nativeMs -le 0 -or $null -eq $ratio -or $ratio -le 0) {
        $problems.Add("$ExpectedId timing fields must be finite and positive")
    } elseif (-not (Test-NumericNear $ratio ($suraMs / $nativeMs))) {
        $problems.Add("$ExpectedId ratio does not equal sura_jit_ms/native_ms")
    }

    $runs = @(As-Array (Get-PropertyValue $Baseline "sura_jit_runs"))
    $runValues = @()
    foreach ($run in $runs) {
        $runValue = Convert-ToFiniteDouble $run
        if ($null -eq $runValue -or $runValue -le 0) {
            $problems.Add("$ExpectedId contains a non-positive or non-finite Sura run")
            $runValues = @()
            break
        }
        $runValues += $runValue
    }
    $declaredRunCount = Convert-ToFiniteDouble (Get-PropertyValue $Baseline "sura_jit_run_count")
    $countOk = ($runs.Count -gt 0 -and
        $null -ne $declaredRunCount -and $declaredRunCount -eq [Math]::Floor($declaredRunCount) -and
        [int]$declaredRunCount -eq $runs.Count -and
        $null -ne $scopeSuraRuns -and $scopeSuraRuns -eq [Math]::Floor($scopeSuraRuns) -and
        [int]$scopeSuraRuns -eq $runs.Count -and
        $null -ne $scopeNativeRuns -and $scopeNativeRuns -eq [Math]::Floor($scopeNativeRuns) -and
        $scopeNativeRuns -ge 1)
    if (-not $countOk) {
        $problems.Add("$ExpectedId run counts are missing or inconsistent")
    }
    if ($runValues.Count -gt 0) {
        $runAverage = ($runValues | Measure-Object -Average).Average
        if (-not (Test-NumericNear $suraMs $runAverage)) {
            $problems.Add("$ExpectedId sura_jit_ms does not equal the average of sura_jit_runs")
        }
    }

    if ([string](Get-PropertyValue $Baseline "sura_jit_time_source") -ne "script_loop" -or
        [string](Get-PropertyValue $Baseline "native_time_source") -ne "native_loop" -or
        [string](Get-PropertyValue $Baseline "sura_loop_scope") -ne "100k" -or
        [string](Get-PropertyValue $Baseline "native_loop_scope") -ne "100k") {
        $problems.Add("$ExpectedId timing sources or loop labels are inconsistent")
    }

    return [pscustomobject]@{
        passed = ($problems.Count -eq 0)
        problems = @($problems)
        sura_ms = $suraMs
        native_ms = $nativeMs
        ratio = $ratio
        run_count = $runs.Count
    }
}

function Add-Check {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Category,
        [string]$Id,
        [string]$Name,
        [bool]$Passed,
        [string]$Message,
        [string[]]$Evidence,
        [string]$NextAction,
        [bool]$Blocking = $true
    )
    $Checks.Add([pscustomobject]@{
        category = $Category
        id = $Id
        name = $Name
        passed = $Passed
        blocking = $Blocking
        message = $Message
        evidence = @(As-Array $Evidence)
        next_action = $NextAction
    })
}

function Add-FileEvidence {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Category,
        [string]$Id,
        [string]$Name,
        [string[]]$Paths,
        [string]$NextAction
    )
    $missing = @()
    foreach ($path in (As-Array $Paths)) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $path))) {
            $missing += $path
        }
    }
    Add-Check $Checks $Category $Id $Name ($missing.Count -eq 0) `
        $(if ($missing.Count -eq 0) { "required files exist" } else { "missing files: $($missing -join ', ')" }) `
        $Paths $NextAction
}

function Add-TextEvidence {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Category,
        [string]$Id,
        [string]$Name,
        [string]$Path,
        [string[]]$Needles,
        [string]$NextAction
    )
    $full = Join-Path $root $Path
    if (-not (Test-Path -LiteralPath $full)) {
        Add-Check $Checks $Category $Id $Name $false "missing file: $Path" @($Path) $NextAction
        return
    }
    $text = Read-Text $full
    $missing = @()
    foreach ($needle in (As-Array $Needles)) {
        if ([string]::IsNullOrWhiteSpace($needle)) { continue }
        if (-not $text.Contains($needle)) {
            $missing += $needle
        }
    }
    Add-Check $Checks $Category $Id $Name ($missing.Count -eq 0) `
        $(if ($missing.Count -eq 0) { "required text evidence exists" } else { "missing text: $($missing -join ', ')" }) `
        @($Path) $NextAction
}

function Add-MultiTextEvidence {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Category,
        [string]$Id,
        [string]$Name,
        [object[]]$Files,
        [string]$NextAction
    )
    $missing = @()
    $evidence = @()
    foreach ($fileSpec in (As-Array $Files)) {
        if ($fileSpec -is [System.Collections.IDictionary]) {
            $path = [string]$fileSpec["path"]
            $needles = As-Array $fileSpec["needles"]
        } else {
            $path = [string](Get-PropertyValue $fileSpec "path")
            $needles = As-Array (Get-PropertyValue $fileSpec "needles")
        }
        if ([string]::IsNullOrWhiteSpace($path)) {
            $missing += "<missing path>"
            continue
        }
        $evidence += $path
        $full = Join-Path $root $path
        if (-not (Test-Path -LiteralPath $full)) {
            $missing += "missing file: $path"
            continue
        }
        $text = Read-Text $full
        foreach ($needle in $needles) {
            if ([string]::IsNullOrWhiteSpace($needle)) { continue }
            if (-not $text.Contains($needle)) {
                $missing += "$path missing text: $needle"
            }
        }
    }
    Add-Check $Checks $Category $Id $Name ($missing.Count -eq 0) `
        $(if ($missing.Count -eq 0) { "required multi-file text evidence exists" } else { "missing evidence: $($missing -join '; ')" }) `
        @($evidence | Sort-Object -Unique) $NextAction
}

function Add-NativePerformanceEvidence {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Path
    )
    $full = Resolve-OutputPath $Path $root
    if (-not (Test-Path -LiteralPath $full)) {
        Add-Check $Checks "performance" "native_performance_evidence" "C++ native performance baseline evidence" $false `
            "native performance report missing: $Path" @($Path) `
            "run tools/sura_native_perf_baseline.ps1 and keep artifacts/native_perf.json"
        Add-Check $Checks "performance" "native_cpp_speed_goal" "Rust/C++-class native speed proof" $false `
            "cannot evaluate Sura/native ratio without native_perf.json" @($Path) `
            "generate native_perf.json, then reduce Sura/native ratio to <= $MaxNativeRatio`x"
        return
    }

    $report = Read-JsonFile $full
    $problems = New-Object System.Collections.Generic.List[string]
    $schemaOk = ([string](Get-PropertyValue $report "schema")) -eq "sura.native.performance.v1"
    if (-not $schemaOk) { $problems.Add("schema is not sura.native.performance.v1") }
    $runOk = [bool](Get-PropertyValue $report "passed")
    if (-not $runOk) { $problems.Add("report passed is false") }

    $generatedText = [string](Get-PropertyValue $report "generated_utc")
    $generatedAt = [DateTimeOffset]::MinValue
    $generatedParsed = (-not [string]::IsNullOrWhiteSpace($generatedText) -and
        $generatedText -match 'Z$' -and
        [DateTimeOffset]::TryParse(
            $generatedText,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::AdjustToUniversal,
            [ref]$generatedAt
        ))
    $now = [DateTimeOffset]::UtcNow
    $freshOk = ($generatedParsed -and
        $generatedAt -le $now.AddMinutes(5) -and
        ($MaxNativeEvidenceAgeHours -le 0 -or $generatedAt -ge $now.AddHours(-$MaxNativeEvidenceAgeHours)))
    if (-not $freshOk) {
        $problems.Add("generated_utc is invalid, in the future, or older than $MaxNativeEvidenceAgeHours hours")
    }

    $engine = Get-PropertyValue $report "engine"
    $engineFile = [string](Get-PropertyValue $engine "file")
    $reportedEngineSha = ([string](Get-PropertyValue $engine "sha256")).ToLowerInvariant()
    $reportedEngineBytes = Convert-ToFiniteDouble (Get-PropertyValue $engine "bytes")
    $engineNameOk = (-not [string]::IsNullOrWhiteSpace($engineFile) -and
        $engineFile -eq [System.IO.Path]::GetFileName($engineFile))
    $enginePath = if ($engineNameOk) { Join-Path $root $engineFile } else { "" }
    $engineBindingOk = $false
    if ($engineNameOk -and (Test-Path -LiteralPath $enginePath -PathType Leaf)) {
        try {
            $currentEngineItem = Get-Item -LiteralPath $enginePath
            $currentEngineSha = (Get-FileHash -LiteralPath $enginePath -Algorithm SHA256).Hash.ToLowerInvariant()
            $engineBindingOk = ($reportedEngineSha -match '^[0-9a-f]{64}$' -and
                $reportedEngineSha -eq $currentEngineSha -and
                $null -ne $reportedEngineBytes -and $reportedEngineBytes -eq $currentEngineItem.Length)
        } catch {
            $engineBindingOk = $false
        }
    }
    if (-not $engineBindingOk) {
        $problems.Add("engine SHA-256 or byte count does not match the current $engineFile binary")
    }

    $baselines = @(As-Array (Get-PropertyValue $report "baselines"))
    $baselineCount = Convert-ToFiniteDouble (Get-PropertyValue $report "baseline_count")
    $baselineCountOk = ($baselines.Count -ge 2 -and
        $null -ne $baselineCount -and $baselineCount -eq [Math]::Floor($baselineCount) -and
        [int]$baselineCount -eq $baselines.Count)
    if (-not $baselineCountOk) { $problems.Add("baseline_count does not match the baseline array") }

    $vec2Matches = @($baselines | Where-Object { [string](Get-PropertyValue $_ "id") -eq "vec2" })
    $vec3Matches = @($baselines | Where-Object { [string](Get-PropertyValue $_ "id") -eq "vec3" })
    $selectorOk = ($vec2Matches.Count -eq 1 -and $vec3Matches.Count -eq 1 -and
        [string](Get-PropertyValue $report "primary_baseline_id") -eq "vec2" -and
        [string](Get-PropertyValue $report "three_d_baseline_id") -eq "vec3")
    if (-not $selectorOk) { $problems.Add("Vec2/Vec3 baseline ids or selectors are missing or ambiguous") }
    $vec2Baseline = if ($vec2Matches.Count -eq 1) { $vec2Matches[0] } else { $null }
    $vec3Baseline = if ($vec3Matches.Count -eq 1) { $vec3Matches[0] } else { $null }
    $vec2Integrity = Test-NativeBaselineIntegrity $vec2Baseline "vec2" "vec2"
    $vec3Integrity = Test-NativeBaselineIntegrity $vec3Baseline "vec3" "vec3"
    foreach ($problem in $vec2Integrity.problems) { $problems.Add([string]$problem) }
    foreach ($problem in $vec3Integrity.problems) { $problems.Add([string]$problem) }

    $topLevelOk = ($null -ne $vec2Baseline -and
        [bool](Get-PropertyValue $report "fair_scope_passed") -and
        (Test-NumericNear (Get-PropertyValue $report "sura_jit_ms") $vec2Integrity.sura_ms) -and
        (Test-NumericNear (Get-PropertyValue $report "native_ms") $vec2Integrity.native_ms) -and
        (Test-NumericNear (Get-PropertyValue $report "sura_native_ratio") $vec2Integrity.ratio) -and
        (Convert-ToFiniteDouble (Get-PropertyValue $report "sura_jit_run_count")) -eq $vec2Integrity.run_count)
    if (-not $topLevelOk) { $problems.Add("top-level Vec2 timing aliases do not match the Vec2 baseline") }

    $vec3AliasesOk = ($null -ne $vec3Baseline -and
        [bool](Get-PropertyValue $report "native_3d_available") -and
        [bool](Get-PropertyValue $report "native_3d_fair_scope_passed") -and
        (Test-NumericNear (Get-PropertyValue $report "native_3d_sura_jit_ms") $vec3Integrity.sura_ms) -and
        (Test-NumericNear (Get-PropertyValue $report "native_3d_ms") $vec3Integrity.native_ms) -and
        (Test-NumericNear (Get-PropertyValue $report "native_3d_sura_native_ratio") $vec3Integrity.ratio))
    if (-not $vec3AliasesOk) { $problems.Add("top-level Vec3 timing aliases do not match the Vec3 baseline") }

    $artifactContextOk = ($schemaOk -and $runOk -and $freshOk -and $engineBindingOk -and $baselineCountOk -and $selectorOk)
    $vec3BaselineOk = ($artifactContextOk -and $vec3Integrity.passed -and $vec3AliasesOk)
    $baselineOk = ($artifactContextOk -and $vec2Integrity.passed -and $vec3Integrity.passed -and $topLevelOk -and $vec3AliasesOk)
    $problemText = if ($problems.Count -gt 0) { ($problems | Select-Object -Unique) -join "; " } else { "" }
    Add-Check $Checks "performance" "native_performance_evidence" "C++ native performance baseline evidence" $baselineOk `
        $(if ($baselineOk) { "fresh engine-bound Vec2/Vec3 evidence passed integrity checks" } else { "native baseline integrity failed: $problemText" }) `
        @($Path, $engineFile) "rerun tools/sura_native_perf_baseline.ps1 with the current engine and keep the unmodified output"
    Add-Check $Checks "performance" "native_3d_performance_evidence" "3D C++ native performance baseline evidence" $vec3BaselineOk `
        $(if ($vec3BaselineOk) { "3D native baseline recorded a verified fair-scope ratio {0:N2}x" -f $vec3Integrity.ratio } else { "native baseline lacks current, internally consistent 3D Vec3 evidence" }) `
        @($Path, $engineFile) "rerun tools/sura_native_perf_baseline.ps1 with the current engine and keep the Vec3 run evidence"

    $ratioOk = ($baselineOk -and
        $vec2Integrity.ratio -le $MaxNativeRatio -and
        $vec3Integrity.ratio -le $MaxNativeRatio)
    Add-Check $Checks "performance" "native_cpp_speed_goal" "Rust/C++-class native speed proof" $ratioOk `
        $(if ($ratioOk) {
            "Vec2 {0:N2}x and Vec3 {1:N2}x are both within limit {2:N2}x" -f $vec2Integrity.ratio, $vec3Integrity.ratio, $MaxNativeRatio
        } elseif (-not $baselineOk) {
            "cannot accept Vec2/Vec3 ratios because native performance evidence failed integrity checks"
        } else {
            "Vec2 {0:N2}x and Vec3 {1:N2}x must both be within target {2:N2}x" -f $vec2Integrity.ratio, $vec3Integrity.ratio, $MaxNativeRatio
        }) `
        @($Path) "optimize JIT/AOT hot loops, numeric operations, and object-heavy code until both Vec2 and Vec3 Sura/native ratios are <= $MaxNativeRatio`x"
}

function Add-OpenRoadmapWork {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Id,
        [string]$Name,
        [string]$Needle,
        [string]$NextAction
    )
    $path = "Guide/WORLD_CLASS_ROADMAP.md"
    $full = Join-Path $root $path
    if (-not (Test-Path -LiteralPath $full)) {
        Add-Check $Checks "world_class_frontier" $Id $Name $false "roadmap missing: $path" @($path) $NextAction
        return
    }
    $text = Read-Text $full
    $stillOpen = $text.Contains($Needle)
    Add-Check $Checks "world_class_frontier" $Id $Name (-not $stillOpen) `
        $(if ($stillOpen) { "roadmap still lists this as non-negotiable remaining work" } else { "roadmap no longer lists this as open work" }) `
        @($path) $NextAction
}

$root = Resolve-RepoPath $RepoRoot
$checks = New-Object System.Collections.Generic.List[object]

Add-TextEvidence $checks "positioning" "python_like_scripting" "Python-like easy scripting position" "Guide/WORLD_CLASS_ROADMAP.md" @("Python-like scripting language", "Easier than Python") "keep the positioning guide aligned with the language syntax and stdlib"
Add-TextEvidence $checks "positioning" "lua_like_embedding" "Lua-like native embedding position" "Guide/WORLD_CLASS_ROADMAP.md" @("embed into native apps like Lua", "surapkg embed") "keep native embedding docs and smoke coverage current"
Add-TextEvidence $checks "positioning" "ai_automation_native" "AI and automation native position" "Guide/WORLD_CLASS_ROADMAP.md" @("AI/automation native", "surapkg agent") "keep AI/automation docs and agent smoke coverage current"
Add-TextEvidence $checks "positioning" "independent_language" "Independent language, not Python translation" "Guide/WORLD_CLASS_ROADMAP.md" @("Independent by default", "not a Python-to-Sura source translator") "avoid adding Python-to-Sura translation as a core path"

Add-TextEvidence $checks "package_ecosystem" "publish_registry" "Publish and registry workflow" "Guide/WORLD_CLASS_ROADMAP.md" @("publish", "SURA_REGISTRY_TOKEN") "restore publish and registry workflow evidence"
Add-TextEvidence $checks "package_ecosystem" "central_registry" "Central registry metadata and browser flow" "Guide/WORLD_CLASS_ROADMAP.md" @("SURA_REGISTRY_URL", "HTTP registry") "restore central registry metadata and browser evidence"
Add-MultiTextEvidence $checks "package_ecosystem" "hosted_registry_service_packaging" "Hosted registry service packaging and health smoke" @(
    @{ path = "tools/sura_registry_api.js"; needles = @("sura.registry.health_endpoint.v1", "SURA_REGISTRY_HOST", "SIGTERM") },
    @{ path = "deploy/registry/Dockerfile"; needles = @("HEALTHCHECK", "SURA_REGISTRY_HOST=0.0.0.0", "USER sura") },
    @{ path = "deploy/registry/docker-compose.yml"; needles = @("restart: unless-stopped", "SURA_REGISTRY_ADMIN_TOKEN", "healthcheck") },
    @{ path = "deploy/registry/systemd/sura-registry.service"; needles = @("Restart=always", "EnvironmentFile=/etc/sura/registry.env") },
    @{ path = "tools/sura_registry_service_smoke.ps1"; needles = @("sura.registry.health_endpoint.v1", "api/reports/review", "registry-health", "--fail-on-warning") }
) "restore hosted registry service packaging, health endpoint, admin queue smoke, and deployment docs"
Add-MultiTextEvidence $checks "world_class_frontier" "hosted_registry_abuse_review_ops" "Hosted registry abuse queue operations" @(
    @{ path = "tools/sura_registry_api.js"; needles = @("moderation-log.jsonl", "sura.registry.reports.queue.v1", "reportStatusCounts", "body.yank === true", "sura.registry.moderation_event.v1") },
    @{ path = "tools/sura_registry_service_smoke.ps1"; needles = @("status=reviewing", "status=actioned&name=service_pkg", "moderation-log.jsonl", "actioned abuse report review to yank package") },
    @{ path = "deploy/registry/README.md"; needles = @("moderation-log.jsonl", "actioned-report yanks", "/api/reports?status=open&limit=100") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("filtered admin queues", "per-status queue counts", "actioned-report yanks") }
) "restore hosted registry abuse review queue filters, moderation audit log, actioned yanks, and smoke/docs evidence"
Add-FileEvidence $checks "package_ecosystem" "versioning" "Package version management" @("tools/sura_pkg_version_smoke.ps1") "restore surapkg version smoke coverage"
Add-MultiTextEvidence $checks "package_ecosystem" "cross_version_compatibility" "Guaranteed and historical cross-version compatibility evidence" @(
    @{ path = "compatibility.json"; needles = @("guarantee_starts_at", "historical_probes", "verification_only", "tests/compat/1.10/core.sura") },
    @{ path = "COMPATIBILITY.md"; needles = @("tests/compat/1.10", "1.10.0", "not_applicable") },
    @{ path = "tools/sura_compatibility_gate.ps1"; needles = @("historical compatibility probe", "bytecode_forward_load", "archive_sha256") },
    @{ path = "tools/sura_release_evidence_gate.ps1"; needles = @("historical compatibility coverage", "archived runtime and bytecode evidence") },
    @{ path = "tests/compat/1.10/core.sura"; needles = @("func classify(value) do", "compat-1.10-core: PASS") }
) "restore the 1.11 guarantee contract, archived 1.10 runtime probes, forward bytecode load, release gate, and public documentation"
Add-TextEvidence $checks "package_ecosystem" "lockfile_resolver" "Lockfile and dependency resolver" "Guide/WORLD_CLASS_ROADMAP.md" @("semver-style dependency constraints", "resolve") "restore dependency resolver and lockfile evidence"
Add-TextEvidence $checks "package_ecosystem" "security_advisories" "Registry security advisory gates" "Guide/WORLD_CLASS_ROADMAP.md" @("Registry advisory gates", "surapkg advisories") "restore advisory gates and install/update blocking coverage"
Add-MultiTextEvidence $checks "package_ecosystem" "windows_user_installer" "Python-style Windows user installer" @(
    @{ path = "tools/sura_make_installer.ps1"; needles = @("SuraLanguageSetup-", "Sura Language Setup", "Required space", "payload_size_bytes", "sura.cmd", "surapkg.cmd", "LOCALAPPDATA", "GetManifestResourceStream") },
    @{ path = "tools/sura_installer_smoke.ps1"; needles = @("sura.installer.pack.v1", "install-ok", "only SuraSetup.exe", "payload_size_bytes", "--quiet-install") },
    @{ path = "Guide/INSTALL.md"; needles = @("Sura should feel like Python", "only installer file", "users only need Sura", "sura app.sura") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("Sura Language", "user-facing single-file wizard", "required disk space", "SuraLanguageSetup-<version>.exe", "SuraLanguage-<version>-windows-x64.zip") }
) "restore installer generator, PATH command shims, smoke coverage, and install docs"
Add-FileEvidence $checks "package_ecosystem" "docs_generation" "Package docs generation" @("tools/sura_pkg_docs_smoke.ps1", "tools/sura_pkg_search_smoke.ps1") "restore generated package docs and search smoke coverage"
Add-FileEvidence $checks "package_ecosystem" "release_flow" "Guarded package release flow" @("tools/sura_release_smoke.ps1", "tools/sura_release_pack_smoke.ps1") "restore guarded release smoke coverage"

Add-FileEvidence $checks "interop" "python_bridge" "Python package bridge" @("tools/sura_python_bridge_smoke.ps1") "restore Python bridge smoke coverage"
Add-FileEvidence $checks "interop" "c_abi_ffi" "C ABI FFI and native embedding" @("sura_ffi.hpp", "sura_ffi.cpp", "tools/sura_embed_smoke.ps1") "restore C ABI embedding files and smoke coverage"
Add-MultiTextEvidence $checks "interop" "native_plugin_cancellation" "Native plugin cooperative cancellation evidence" @(
    @{ path = "sura_plugin.h"; needles = @("SURA_PLUGIN_CANCELLED", "should_cancel") },
    @{ path = "examples/sura_plugin_sample.c"; needles = @("sample_spin_ms_cancellable", "ctx->host->should_cancel", "SURA_PLUGIN_CANCELLED") },
    @{ path = "tools/sura_plugin_smoke.ps1"; needles = @("host_capabilities", "cancel", "native_spin_ms", "native export cancelled", "max_call_ms") }
) "restore cancellable plugin ABI, sample plugin polling, and policy smoke coverage"
Add-FileEvidence $checks "interop" "js_target" "JavaScript target" @("tools/sura_to_js.ps1", "tools/sura_js_target_smoke.ps1") "restore JavaScript target smoke coverage"
Add-MultiTextEvidence $checks "world_class_frontier" "js_class_exception_lowering" "JS class and exception lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("JS target class/extends", "try/catch/finally", "Push-JsBlock `"class`"", "constructor", "catch (`$errName)") },
    @{ path = "test_js_target.sura"; needles = @("class JsPoint do", "class JsNamedPoint extends JsPoint do", "try", "finally") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("class JsPoint", "class JsNamedPoint extends JsPoint", "catch \(err\)", "try/catch/finally") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("JS language parity", "extends", "finally") }
) "restore JS target class/extends plus try/catch/finally lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_super_init_lowering" "JS super.init constructor lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('super\.init\s*\(', 'super(') },
    @{ path = "test_js_target.sura"; needles = @("class JsScoredPoint extends JsPoint do", "super.init(x, y)", "assert_eq(scored_point_js.score_label(), `"score:10`")") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("class JsScoredPoint extends JsPoint", "super\(x, y\);", "super.init calls") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`super.init(...)`', "JavaScript target") }
) "restore JS target super.init constructor lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_when_lowering" "JS when/is/in/else lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("jsWhenCounter", "Start-JsWhenArm", "when\s+(.+)\s+do", "else\s+then") },
    @{ path = "test_js_target.sura"; needles = @("when score_js do", "in 2 to 4 then", "inline_when_js") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("__sura_when", "__eq\(__sura_when", "when/is/in/else") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("when", "is", "in", "else") }
) "restore JS target when/is/in/else lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_inline_elif_lowering" "JS inline elif statement lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('elif\s+(.+?)\s+then(?:\s+(.+))?', "Convert-SimpleStatement `$elifInline -Inline") },
    @{ path = "test_js_target.sura"; needles = @('elif score_js == 2 then inline_elif_js is "two"', 'elif score_js == 3 then inline_elif_js is "three"', 'assert_eq(inline_elif_js, "three")') },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("inline_elif_js", "inline elif branches") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('inline `elif ... then`', "JS target") }
) "restore JS target inline elif statement lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "if_inline_else_lowering" "Native/JS/WASM inline else statement lowering parity" @(
    @{ path = "parser.hpp"; needles = @("match(TT::THEN)", "inline_body = parse_stmt_body(ln)", "else_block->body.push_back") },
    @{ path = "tools/sura_to_js.ps1"; needles = @('else\s+then(?:\s+(.+))?', "Convert-SimpleStatement `$elseInline -Inline") },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("inline else-then statements", ";; inline else then", 'else(?:\s+then(?:\s+(.+))?)?') },
    @{ path = "tests/03_control.sura"; needles = @('else then inline_else_result is "not big"', 'assert_eq(inline_else_result, "not big")') },
    @{ path = "test_js_target.sura"; needles = @('else then inline_else_js is "other"', 'assert_eq(inline_else_js, "other")') },
    @{ path = "test_wasm_target.sura"; needles = @("else then inline_else is 2", "assert_eq(inline_else, 2)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("inline_else_js", "inline else branches") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("inline else-then statements", "inline else branches", 'local\.set \$inline_else') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('inline `else then`', "JS", "WASM") }
) "restore native parser plus JS/WASM inline else statement lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_match_lowering" "JS match/when wildcard lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("jsMatchCounter", "Start-JsMatchArm", 'match\s+(.+)', 'when\s+(.+?)\s+then(?:\s+(.+))?') },
    @{ path = "test_js_target.sura"; needles = @("match score_js", "when 3 then match_js is `"three`"", "when _ then match_wildcard_js is `"fallback`"") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("__sura_match", "__eq\(__sura_match", "match/when/wildcard") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("JS language parity", "match", "wildcard") }
) "restore JS target match/when wildcard lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_print_n_space_lowering" "JS print_n space-form lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("const print_n", "const print_no_nl = print_n", '^(print|print_n|print_no_nl)\s+(.+)$') },
    @{ path = "test_js_target.sura"; needles = @('print_n "js target print_n"', 'print " ok"') },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("const print_n", "print_no_nl", 'print_n\("js target print_n"\);') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`print_n value`', "JavaScript target") }
) "restore JS target print_n/print_no_nl space-form lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_enum_lowering" "JS enum declaration lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('Get-JsBlockTop) -eq "enum"', 'enum\s+([A-Za-z_][A-Za-z0-9_]*)\s+do', "unsupported enum member") },
    @{ path = "test_js_target.sura"; needles = @("enum JsMode do", "READY", "SCORE is 7", "assert_eq(JsMode.SCORE, 7)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("const JsMode", 'READY: "READY"', "SCORE: 7") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("enum declarations", "JavaScript target") }
) "restore JS target enum declaration lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_struct_lowering" "JS struct declaration lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("jsStructStack", 'struct\s+([A-Za-z_][A-Za-z0-9_]*)\s+do', "__SuraStruct_", "Add-JsStructAutoConstructor") },
    @{ path = "test_js_target.sura"; needles = @("struct JsVec2 do", "return JsVec2(self.x + other.x, self.y + other.y)", "assert_eq(vec_sum_js.x, 6)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("class __SuraStruct_JsVec2", "function JsVec2", "struct declarations and factory calls") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("struct declarations", "factory calls", "JavaScript target") }
) "restore JS target struct declaration lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_lambda_lowering" "JS lambda expression lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("Convert-SuraLambdaExpression", "=>", "lambda") },
    @{ path = "test_js_target.sura"; needles = @("lambda_scale_js is |value| value * lambda_factor_js", "lambda_ready_js is || `"ready`"", "assert_eq(lambda_scale_js(3), 12)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("arrow functions", "lambda_scale_js", "lambda_ready_js") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("lambda expressions", "arrow functions", "JavaScript target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("lambda expressions", "JS target parity") }
) "restore JS target lambda expression lowering, smoke, and docs evidence"
    Add-MultiTextEvidence $checks "world_class_frontier" "js_func_expr_lowering" "JS block function expression lowering parity" @(
        @{ path = "tools/sura_to_js.ps1"; needles = @("func-expr-assign", 'is\s+func\s*\(([^)]*)\)', 'function($params)') },
        @{ path = "test_js_target.sura"; needles = @("block_double_js is func(value) do", "return value * 2", "assert_eq(block_double_js(6), 12)") },
        @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("block function expressions", "block_double_js", 'function\(value\)') },
        @{ path = "Guide/ECOSYSTEM.md"; needles = @("block function expressions", "JavaScript function expressions") },
        @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("block function expressions", "JS target parity") }
    ) "restore JS target block function expression lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_optional_coalesce_lowering" "JS optional chaining and null coalescing parity" @(
    @{ path = "test_js_target.sura"; needles = @('optional_user_js?.profile?.name ?? "anon"', 'optional_profile_js?.profile?.name ?? "anon"', 'optional_profile_js?.missing?.name ?? "anon"') },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("optional chaining and null coalescing", 'optional_user_js\?\.profile\?\.name \?\? "anon"') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("optional chaining/null coalescing expressions", "JavaScript target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("optional chaining/null coalescing expressions", "JS target parity") }
) "restore JS target optional chaining/null coalescing smoke and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_stable_runtime_subset" "JS target stable-test runtime parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("Get-JsRecoveredCommentStatement", "optionalRoots", "__sura_define_method", "Convert-SuraInlineFunctionExpressions", "__sura_div", "function clamp") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("stable JS runtime parity", "test_stdlib.sura", "tests\08_exceptions.sura", "test_for_in_improved.sura") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("13 selected existing Sura scripts", '`test_stdlib.sura`', '`test_for_in_improved.sura`') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("13 selected existing Sura scripts", "JS target parity") }
) "restore JS target stable-test runtime parity smoke and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_nested_closure_lowering" "JS nested function closure lowering parity" @(
    @{ path = "test_js_target.sura"; needles = @("func make_js_adder(base) do", "func add_js_value(value) do", "return add_js_value", "assert_eq(add_five_js(7), 12)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("nested function closures", "function make_js_adder", "function add_js_value") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("nested function closures", "JavaScript target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("nested function closures", "JS target parity") }
) "restore JS target nested function closure smoke and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_indexed_for_lowering" "JS indexed for-loop lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("__sura_for_items", "__sura_for_entries", 'for\s+([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s+in') },
    @{ path = "test_js_target.sura"; needles = @("for idx_js, item_js in items do", "indexed_total_js += idx_js + item_js", "assert_eq indexed_total_js, 16") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("indexed array for-loops", "__sura_for_items", "item_js") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('indexed `for index, value in items` loops', "JavaScript target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('indexed `for index, value in items` loops', "JS target parity") }
) "restore JS target indexed for-loop lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_dict_indexed_for_lowering" "JS dict key/value for-loop lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("Object.entries", "__sura_entries", "__sura_for_entries", 'for (const [$indexName, $itemName]') },
    @{ path = "test_js_target.sura"; needles = @("for key_js, value_js in dict_for_js do", "dict_indexed_total_js += value_js", "assert_eq(dict_indexed_total_js, 15)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("indexed dict for-loops", "dict_indexed_total_js", "__sura_entries") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('indexed `for key, value in dict` loops', "JavaScript target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('indexed `for key, value in dict` loops', "JS target parity") }
) "restore JS target dict key/value for-loop lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_safe_for_iter_lowering" "JS nil and dict-safe for-in lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("function __sura_iter", "function __sura_entries", "__sura_iter(") },
    @{ path = "test_js_target.sura"; needles = @("nil_iter_count_js is 0", "assert_eq(nil_iter_count_js, 0)", "dict_single_iter_count_js is 0", "assert_eq(dict_single_iter_count_js, 0)", "assert_eq(nil_indexed_iter_count_js, 0)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("nil/dict single for loops", "function __sura_iter", "nil_indexed_iter_count_js") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('nil/dict-safe `for ... in`', "JavaScript target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('nil/dict-safe `for ... in`', "JS target parity") }
) "restore JS nil/dict-safe for-in lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_space_assert_lowering" "JS/WASM space-form assertion lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("assert_not_contains", 'assert_type|assert_len', 'assert_approx)\s+(.+)') },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("space-form assertions", 'assert_between|assert_approx', 'assert_approx)\s+(.+)') },
    @{ path = "test_js_target.sura"; needles = @("assert indexed_total_js == 16", "assert_eq indexed_total_js, 16") },
    @{ path = "test_wasm_target.sura"; needles = @("assert result == 32", "assert_eq result, 32", "assert_ne result, 0") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("space-form assertion statements", "assert_eq", "indexed_total_js") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("space-form assertion statements", "space-form assertions") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("space-form assertions", '`assert_eq actual, expected`') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("space-form assertions", "WASM target parity") }
) "restore JS/WASM space-form assertion lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_loop_control_lowering" "WASM break and continue lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('Push-WasmLoopContext', 'Get-WasmLoopContext', 'ContinueToLoop', 'br `$$label') },
    @{ path = "test_wasm_target.sura"; needles = @("if i == 2 then continue", "if i == 5 then break", "assert_eq(control_sum, 24)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('block \$__break', 'br \$__continue', 'break/continue') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("named-label", "break", "continue") }
) "restore WASM target break/continue named-label lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_loop_control_lowering" "JS break and continue loop-control parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('^(break|continue)$', 'return "$t;"') },
    @{ path = "test_js_target.sura"; needles = @("if i_js_loop == 2 then continue", "if i_js_loop == 5 then break", "assert_eq(control_sum_js, 24)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("break/continue loop control", "control_sum_js", 'continue;') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`break`/`continue` loop control', "JavaScript target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('`break`/`continue` loop control', "JS target parity") }
) "restore JS target break/continue loop-control smoke and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_step_for_lowering" "JS/WASM stepped range-for lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("__sura_for_step", 'step\s+(.+)\s+do', '$stepVar > 0 ?') },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('__for_step', 'step\s+(.+)\s+do', "range-for with step") },
    @{ path = "test_js_target.sura"; needles = @("for n in 5 to 1 step -2 do", "assert_eq(step_total_js, 9)") },
    @{ path = "test_wasm_target.sura"; needles = @("for n in 5 to 1 step -2 do", "assert_eq(step_total, 9)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('\(local \$__(?:ast_)?for_step', 'local\.set \$__(?:ast_)?for_step', "range-for with step") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('stepped range `for`', "JS target", "WASM target") }
) "restore JS/WASM stepped range-for lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_tilde_for_lowering" "JS/WASM tilde range-for lowering parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('(?:to|~)', 'for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+)\s+(?:to|~)\s+(.+)\s+do') },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('(?:to|~)', "tilde range-for", "range-for with step") },
    @{ path = "test_js_target.sura"; needles = @("for n in 1 ~ 3 do", "tilde_total_js += n", "assert_eq(tilde_total_js, 6)") },
    @{ path = "test_wasm_target.sura"; needles = @("for n in 1 ~ 3 do", "tilde_total += n", "assert_eq(tilde_total, 6)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("tilde range-for loops", "tilde_total_js") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("tilde range-for loops", 'local\.set \$tilde_total') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("tilde range", '`for n in 1 ~ 3`') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("tilde range", '`for n in 1 ~ 3`') }
) "restore JS/WASM tilde range-for lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_loop_limit_snapshot_lowering" "JS repeat and range limit snapshot parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @("__sura_repeat_limit", "__sura_for_end", '$loopVar < $limitVar') },
    @{ path = "test_js_target.sura"; needles = @("range_limit_js is 3", "range_limit_js is 0", "assert_eq(range_count_js, 3)", "repeat_limit_js is 3", "repeat_limit_js is 0", "assert_eq(repeat_fixed_count_js, 3)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("snapshot repeat and range loop limits", "__sura_repeat_limit", "__sura_for_end") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("JS target snapshots", "range loop limits") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("repeat/range limit snapshots", "JS target parity") }
) "restore JS repeat/range loop limit snapshot lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_tilde_when_range_lowering" "Native/JS/WASM tilde range when-arm parity" @(
    @{ path = "parser.hpp"; needles = @("'to' or '~' expected in when range arm", "TT::TILDE", "arm.range_end") },
    @{ path = "tools/sura_to_js.ps1"; needles = @('in\s+(.+)\s+(?:to|~)\s+(.+)\s+then', "Start-JsWhenArm") },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('in\s+.+\s+(?:to|~)\s+.+', 'in\s+(.+?)\s+(?:to|~)\s+(.+?)\s+then') },
    @{ path = "test_js_target.sura"; needles = @("in 1 ~ 3 then", "tilde_when_js is `"tilde`"", "assert_eq(tilde_when_js, `"tilde`")") },
    @{ path = "test_wasm_target.sura"; needles = @("in 30 ~ 35 then", "when_tilde is 5", "assert_eq(when_tilde, 5)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("tilde range when arms", "tilde_when_js") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("tilde range when arms", 'local\.set \$when_tilde') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('tilde range `when ... in 1 ~ 3 then` arms', 'tilde range `when ... in 30 ~ 35 then` arms') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('tilde range `when ... in 1 ~ 3 then` arms', 'tilde range `when ... in 30 ~ 35 then` arms') }
) "restore native, JS, and WASM tilde range when-arm parsing/lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_when_first_else_lowering" "Native/JS/WASM first-else when-arm parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('HasElse = $false', '$lines.Add("if (!$($when.Matched)) {")', '$lines.Add("$($when.Matched) = true;")') },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('local.get `$$matched', 'i32.eqz', 'WASM when has duplicate else arms') },
    @{ path = "test_js_target.sura"; needles = @("when_first_else_js is `"none`"", "else then when_first_else_js is `"fallback`"", "assert_eq(when_first_else_js, `"fallback`")") },
    @{ path = "test_wasm_target.sura"; needles = @("when_first_else is 0", "else then when_first_else is 9", "assert_eq(when_first_else, 9)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("first-else when arms", "when_first_else_js", 'if \(!__sura_when_matched\d+\) \{') },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("first-else when arms", 'local\.set \$when_first_else', 'i32\.const 1') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('first-else `when` arms', 'first-else numeric `when` arms') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('first-else `when` arms', 'first-else numeric `when` arms') }
) "restore native, JS, and WASM first-else when-arm lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "native_reverse_step_for" "Native reverse stepped range-for execution" @(
    @{ path = "jit_compiler.hpp"; needles = @("for step must not be zero", "Positive steps stop after", "CMP_GTE") },
    @{ path = "tests/03_control.sura"; needles = @("for n in 5 to 1 step -2 do", "assert_eq(reverse_total, 9)") },
    @{ path = "stdlib/data.sura"; needles = @("for _i in _len - 1 to 0 step -1 do") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("native runtime", 'reverse stepped range `for`') }
) "restore native JIT reverse stepped range-for execution and stable-test evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_when_lowering" "WASM when/is/in/else lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("New-WasmWhenCondition", "when\s+(.+)\s+do", "i32.ge_s", "i32.le_s", "__when") },
    @{ path = "test_wasm_target.sura"; needles = @("when result do", "in 32 to 40 then", "when_inline is 7") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('local\.set \$when_pick', 'when/is/in/else') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('numeric `when`', '`is`', '`in`') }
) "restore WASM target when/is/in/else lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_match_lowering" "WASM match/when wildcard lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("New-WasmMatchCondition", 'match\s+(.+)', 'when\s+(.+?)(?:\s+then', "match/when wildcard arms") },
    @{ path = "test_wasm_target.sura"; needles = @("match result", "when 32 then match_pick is 2", "when _ then match_wildcard is 7") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('\(local \$__ast_match_matched', 'local\.set \$match_pick', "match/when wildcard arms") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('numeric `match`', "wildcard", "WASM target") }
) "restore WASM target match/when wildcard lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_block_match_arm_lowering" "JS/WASM block-style match arm parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('Get-JsMatchTop', 'when\s+(.+?)$', 'Start-JsMatchArm') },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('when\s+.+$' , 'notmatch ''\s+do$''', 'when\s+(.+?)(?:\s+then') },
    @{ path = "test_js_target.sura"; needles = @("match_block_js is `"none`"", "when 3", "assert_eq(match_block_js, `"three`")") },
    @{ path = "test_wasm_target.sura"; needles = @("match_block_pick is 0", "when 32", "assert_eq(match_block_pick, 2)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("block-style match arms without then", "match_block_js") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("block-style match arms without then", 'local\.set \$match_block_pick') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('block-style `match` arms without `then`', 'block-style numeric `match` arms without `then`') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('block-style `match` arms without `then`', 'block-style numeric `match` arms without `then`') }
) "restore JS/WASM block-style match arm lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_match_first_wildcard_lowering" "JS/WASM first-wildcard match arm parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('HasWildcard = $false', '$lines.Add("if (!$($match.Matched)) {")', '$lines.Add("$($match.Matched) = true;")') },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('local.get `$$matched', 'i32.eqz', 'WASM match has duplicate wildcard arms') },
    @{ path = "test_js_target.sura"; needles = @("match_first_wildcard_js is `"none`"", "when _ then match_first_wildcard_js is `"any`"", "assert_eq(match_first_wildcard_js, `"any`")") },
    @{ path = "test_wasm_target.sura"; needles = @("match_first_wildcard is 0", "when _ then match_first_wildcard is 9", "assert_eq(match_first_wildcard, 9)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("first-wildcard match arms", "match_first_wildcard_js", 'if \(!__sura_match_matched\d+\) \{') },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("first-wildcard match arms", 'local\.set \$match_first_wildcard', 'i32\.const 1') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('first-wildcard `match` arms', 'first-wildcard numeric `match` arms') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('first-wildcard `match` arms', 'first-wildcard numeric `match` arms') }
) "restore JS/WASM first-wildcard match arm lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_nonterminal_default_arm_lowering" "JS/WASM nonterminal else/wildcard arm parity" @(
    @{ path = "tools/sura_to_js.ps1"; needles = @('$lines.Add("if (!$($when.Matched)) {")', '$lines.Add("if (!$($match.Matched)) {")', '$lines.Add("$($when.Matched) = true;")', '$lines.Add("$($match.Matched) = true;")') },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('local.get `$$matched', 'i32.eqz', 'i32.and', 'local.set `$$matched') },
    @{ path = "test_js_target.sura"; needles = @("when_middle_else_js is `"none`"", "assert_eq(when_middle_else_js, `"exact`")", "match_middle_wildcard_js is `"none`"", "assert_eq(match_middle_wildcard_js, `"exact`")") },
    @{ path = "test_wasm_target.sura"; needles = @("when_middle_else is 0", "assert_eq(when_middle_else, 8)", "match_middle_wildcard is 0", "assert_eq(match_middle_wildcard, 8)") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("nonterminal else when arms", "nonterminal wildcard match arms", "when_middle_else_js", "match_middle_wildcard_js") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("nonterminal else when arms", "nonterminal wildcard match arms", 'local\.set \$when_middle_else', 'local\.set \$match_middle_wildcard') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('nonterminal default `else`/`_` arms', 'later exact arms can still override') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('nonterminal default `else`/`_` arms', 'later exact arms can still override') }
) "restore JS/WASM nonterminal default-arm lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_recursive_function_lowering" "WASM recursive function and inline return lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("recursive numeric calls", "inline if-then statements", ";; inline then") },
    @{ path = "test_wasm_target.sura"; needles = @("func fib(n: int) -> int do", "if n <= 1 then return n", "assert_eq(fib(10), 55)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('\(func \$fib', 'call \$fib', "inline if-then returns") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("recursive numeric functions", 'inline `if ... then return`') }
) "restore WASM target recursive numeric function lowering, inline return smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_function_exports" "WASM top-level numeric function export parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("exported top-level numeric functions", 'exportClause', '(export') },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("export top-level numeric Sura functions", '\(func \$square \(export "square"\)', '\(func \$fib \(export "fib"\)') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("exports top-level numeric functions", "host runtimes can call Sura numeric functions directly") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("exported top-level numeric functions", "WASM target parity") }
) "restore WASM top-level numeric function exports, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_inline_elif_lowering" "WASM inline elif statement lowering parity" @(
    @{ path = "parser.hpp"; needles = @("while (check(TT::ELIF))", "auto inline_body = parse_stmt_body(eln)", "ebody->body.push_back") },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("inline elif-then statements", ";; inline elif then", 'elif\s+(.+?)\s+then(?:\s+(.+))?') },
    @{ path = "test_wasm_target.sura"; needles = @("elif result == 31 then inline_elif is 2", "elif result == 32 then inline_elif is 3", "assert_eq(inline_elif, 3)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("inline elif-then statements", "inline elif branches", 'local\.set \$inline_elif') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('inline `elif ... then`', "WASM target") }
) "restore WASM target inline elif statement lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_assert_ne_lowering" "WASM assert_ne/assert_neq lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("assert_ne", "assert_neq", "i32.eq") },
    @{ path = "test_wasm_target.sura"; needles = @("assert_ne(result, 0)", "assert_neq(result, 31)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('call \$assert_neq', "assert_ne/assert_neq") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`assert_ne`', '`assert_neq`', "WAT traps") }
) "restore WASM target assert_ne/assert_neq lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_assert_between_lowering" "WASM assert_between lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("assert_between", "i32.lt_s", "i32.gt_s", "i32.or") },
    @{ path = "test_wasm_target.sura"; needles = @("assert_between(result, 30, 40)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('call \$assert_between', "numeric bounds checks") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`assert_between`', "WAT traps") }
) "restore WASM target assert_between numeric bounds lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_assert_approx_lowering" "WASM assert_approx lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("assert_approx", "New-WasmAbsCode", "i32.gt_s") },
    @{ path = "test_wasm_target.sura"; needles = @("assert_approx(result + 1, 33, 0)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('call \$assert_approx', "absolute-difference bounds checks") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`assert_approx`', "WAT traps") }
) "restore WASM target assert_approx numeric absolute-difference lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_pow_lowering" "WASM pow/math.pow lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("__sura_pow_i32", '"math.pow"', '"pow"') },
    @{ path = "test_wasm_target.sura"; needles = @("assert_eq(math.pow(2, 5), 32)", "assert_eq(pow(3, 3), 27)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('\(func \$__sura_pow_i32', 'call \$__sura_pow_i32', "pow/math.pow") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`pow`', '`math.pow`', "integer-safe") }
) "restore WASM target pow/math.pow integer lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_sqrt_lowering" "WASM sqrt/math.sqrt lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("__sura_sqrt_i32", '"math.sqrt"', '"sqrt"') },
    @{ path = "test_wasm_target.sura"; needles = @("assert_eq(math.sqrt(144), 12)", "assert_eq(sqrt(81), 9)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('\(func \$__sura_sqrt_i32', 'call \$__sura_sqrt_i32', "sqrt/math.sqrt") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`sqrt`', '`math.sqrt`', "integer-safe") }
) "restore WASM target sqrt/math.sqrt integer lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_variadic_min_max_lowering" "WASM variadic min/max lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("New-WasmFoldMinCode", "New-WasmFoldMaxCode", "variadic min/max") },
    @{ path = "test_wasm_target.sura"; needles = @("assert_eq(math.min(8, 2, 5), 2)", "assert_eq(math.max(3, 9, 1), 9)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("variadic min/max", "fold variadic min/max") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("variadic", '`math.min`', '`math.max`') }
) "restore WASM target variadic min/max integer lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_decimal_rounding_lowering" "WASM decimal floor/ceil/round literal lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("Convert-WasmRoundedDecimalCall", "decimal floor/ceil/round literal folding", "[System.Globalization.CultureInfo]::InvariantCulture") },
    @{ path = "test_wasm_target.sura"; needles = @("assert_eq(floor(3.9), 3)", "assert_eq(ceil(3.1), 4)", "assert_eq(round(3.5), 4)", "assert_eq(math.floor(2.9), 2)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("decimal floor/ceil/round literal folding", '3\.9|3\.1|3\.5|2\.9') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("decimal literal calls", '`floor`', '`round`') }
) "restore WASM target decimal literal floor/ceil/round folding, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_signed_decimal_rounding_lowering" "WASM signed decimal rounding parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("signed round-half-away-from-zero", '$literal -ge 0', "[System.Math]::Ceiling") },
    @{ path = "test_wasm_target.sura"; needles = @("assert_eq(round(-3.4), -3)", "assert_eq(round(-3.5), -4)", "assert_eq(round(-3.6), -4)", "assert_eq(floor(-3.1), -4)", "assert_eq(ceil(-3.9), -3)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("signed decimal floor/ceil/round literals", 'i32\.const -4', 'i32\.const -3') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("signed decimal literal calls", "round-half-away-from-zero") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("signed decimal literal rounding", "WASM target parity") }
) "restore WASM target signed decimal literal round/floor/ceil folding parity, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_decimal_comparison_lowering" "WASM decimal literal comparison lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("Convert-WasmDecimalComparison", "decimal comparison literal folding", "3.1415926535897932384626433833") },
    @{ path = "test_wasm_target.sura"; needles = @("assert(math.pi > 3.14)", "assert(math.pi < 3.15)", "assert(3.14 < math.pi)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("decimal comparison literal folding", '3\.14|3\.15') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("decimal literal comparisons", '`math.pi > 3.14`', '`3.14 < math.pi`') }
) "restore WASM target decimal literal comparison folding, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_numeric_conversion_lowering" "WASM to_int/to_float/to_bool numeric conversion lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('"to_int"', '"to_float"', '"to_bool"', "numeric conversion aliases to_int/to_float/to_bool") },
    @{ path = "test_wasm_target.sura"; needles = @("assert_eq(to_int(result), 32)", "assert_eq(to_float(result), 32)", "assert_eq(to_bool(result), true)", "assert_eq(to_bool(0), false)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('call \$(to_int|to_float|to_bool)', "numeric conversion aliases to_int/to_float/to_bool") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`to_int`', '`to_float`', '`to_bool`', "direct numeric aliases") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('`to_int`/`to_float`/`to_bool`', "WASM target parity") }
) "restore WASM target to_int/to_float/to_bool numeric conversion lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_is_reassignment_lowering" "WASM is reassignment lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("is assignment/reassignment", "Add-WasmLocal", "local.set") },
    @{ path = "test_wasm_target.sura"; needles = @("assign_score is assign_score + 5", "x is x + 2", "assert_eq(bump_param(5), 7)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("is assignment/reassignment", 'local\.set \$assign_score', 'local\.set \$x') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`is` assignment/reassignment', '`is`') }
) "restore WASM target is reassignment lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_print_call_lowering" "WASM print(...) lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("print/print_n/print_no_nl/print()-as-main-result", "WASM print/print_n/print_no_nl() requires exactly one numeric expression", 'local.set `$__result') },
    @{ path = "test_wasm_target.sura"; needles = @("print(result)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('call \$(print|print_n)', 'local\.set \$__result', "print/print_n/print_no_nl calls") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`print(...)`', "main result") }
) "restore WASM target print(...) lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_print_n_lowering" "WASM print_n lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("print/print_n/print_no_nl/print()-as-main-result", "print(?:_n|_no_nl)?", "WASM print/print_n/print_no_nl() requires exactly one numeric expression") },
    @{ path = "test_wasm_target.sura"; needles = @("print_n(result)", "print_n result") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('call \$(print|print_n)', "print/print_n/print_no_nl calls") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`print_n(...)`', "main result") }
) "restore WASM target print_n statement/call lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_print_no_nl_lowering" "WASM print_no_nl lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("print(?:_n|_no_nl)?", "print/print_n/print_no_nl/print()-as-main-result", "WASM print/print_n/print_no_nl() requires exactly one numeric expression") },
    @{ path = "test_wasm_target.sura"; needles = @("print_no_nl(result)", "print_no_nl result") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("print/print_n/print_no_nl calls", 'local\.set \$__result') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`print_no_nl(...)`', '`print_no_nl value`') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('`print_no_nl(...)`', '`print_no_nl` main-result lowering') }
) "restore WASM target print_no_nl statement/call lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_bool_literal_lowering" "WASM true/false literal lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("true/false bool literals", 'if ($t -eq "true")', 'if ($t -eq "false")') },
    @{ path = "test_wasm_target.sura"; needles = @("assert(true)", "assert(not false)", "if true and not false then") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @('local\.get \$true', 'local\.get \$false', "true/false bool literals") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('`true`/`false` bool literals', 'numeric `and`/`or`/`not`') }
) "restore WASM target true/false bool literal lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_throw_lowering" "WASM throw statement trap lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("side-effect-preserving throw expression evaluation before traps", "unreachable", '^throw') },
    @{ path = "test_wasm_target.sura"; needles = @("throw throw_value_source(41)", "assert_eq(throw_guard, 1)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("side-effect-preserving throw expression evaluation before traps", '(?s)call \$throw_value_source\s+local\.set \$__sura_wasm_call_tmp', 'local\.set \$throw_guard') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('side-effect-preserving `throw` expression evaluation before trap lowering', "WASM target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('side-effect-preserving `throw` expression evaluation before trap lowering', "WASM target parity") }
) "restore WASM target throw statement trap lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_numeric_enum_lowering" "WASM enum declaration lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("wasmEnumConsts", "wasmEnumStringConsts", "numeric enum declarations", "WASM AST enum member requires") },
    @{ path = "test_wasm_target.sura"; needles = @("enum WasmMode do", "READY is 7", "assert_eq(enum_pick, 18)", "enum WasmLabel do", 'CUSTOM is "custom"', 'assert_eq(enum_label, "READY-custom")') },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("numeric enum declarations", "string/bare enum declarations", 'i32\.const 7', 'i32\.const 11', 'local\.set \$enum_label') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("numeric enum declarations", "bare enum", "WASM target") }
) "restore WASM target numeric/string/bare enum declaration lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_bitwise_shift_lowering" "WASM bitwise and shift expression lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("Parse-BitOr", "Parse-Shift", "i32.xor", "i32.shl", "i32.shr_s") },
    @{ path = "test_wasm_target.sura"; needles = @("bit_score is (6 & 3) + (4 | 1) + (7 ^ 2)", "shift_score is (1 << 4) + (16 >> 2)", "assert_eq(shift_score, 20)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("bitwise/shift expressions", 'i32\.xor', 'i32\.shl', 'i32\.shr_s') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("bitwise", "shift expressions", "WASM target") }
) "restore WASM target bitwise/shift expression lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_bitwise_not_lowering" "JS/WASM unary bitwise-not lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @('Match-Token "~"', "i32.const -1", "unary bitwise-not") },
    @{ path = "test_wasm_target.sura"; needles = @("not_score is ~7", "assert_eq(not_score, -8)") },
    @{ path = "test_js_target.sura"; needles = @("bit_not_js is ~7", "assert_eq(bit_not_js, -8)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("unary bitwise-not", 'local\.set \$not_score') },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("unary bitwise-not", "bit_not_js") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("unary bitwise-not", "JS target", "WASM target") }
) "restore JS/WASM unary bitwise-not lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "js_wasm_ternary_lowering" "JS/WASM ternary expression lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("Parse-Ternary", "if (result i32)", "ternary expressions") },
    @{ path = "test_wasm_target.sura"; needles = @("ternary_score is result == 32 ? 100 : 200", "assert_eq(ternary_score, 100)") },
    @{ path = "test_js_target.sura"; needles = @("ternary_js is score_js == 3 ? `"three`" : `"other`"", 'assert_eq(ternary_js, "three")') },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("numeric ternary expressions", 'local\.set \$ternary_score') },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("preserve ternary expressions", "ternary_js") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("ternary expressions", "numeric ternary expressions") }
) "restore JS/WASM ternary expression lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_inline_branch_local_collection" "WASM inline branch-local declaration collection" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("Add-WasmLocalsFromStatement", "inline branch-local declarations", '\bthen\s+(.+)$') },
    @{ path = "test_wasm_target.sura"; needles = @("inline_if_local is 41", "inline_else_local is 42", "inline_elif_local is 43", "inline_when_local is 44", "inline_match_local is 45") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("inline branch-local declarations", 'local\.set \$inline_if_local', 'local\.set \$inline_match_local') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("inline branch-local declarations", "WASM target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("inline branch-local declarations", "WASM target parity") }
) "restore WASM local declaration collection for inline if/elif/else/when/match arms, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_repeat_limit_snapshot_lowering" "WASM fixed-count repeat lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("fixed-count repeat", "__repeat_limit", 'local.set `$$limit') },
    @{ path = "test_wasm_target.sura"; needles = @("repeat_limit is 3", "repeat repeat_limit do", "repeat_limit is 0", "assert_eq(repeat_fixed_count, 3)") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("snapshot repeat count expressions", 'local\.set \$__(?:ast_)?repeat_limit', 'local\.set \$repeat_fixed_count') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @('fixed-count `repeat`', "WASM target") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('fixed-count `repeat`', "WASM target parity") }
) "restore WASM repeat count snapshot lowering, smoke, and docs evidence"
Add-MultiTextEvidence $checks "world_class_frontier" "wasm_numeric_array_lowering" "WASM numeric array literal/index/len lowering parity" @(
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("numeric array literals/indexing/len via linear memory", "array.len/array.sum/array.avg/array.min/array.max/array.range/array.index_of/array.contains helpers", "inline numeric array literals as call arguments", "indexed numeric array for-in loops", "__sura_array_sum", "__sura_array_avg", "__sura_array_min", "__sura_array_max", "__sura_array_range", "__sura_array_index_of", "__sura_array_contains", "__sura_alloc", "i32.store", "i32.load") },
    @{ path = "test_wasm_target.sura"; needles = @("wasm_values is [4, 5, 6]", "assert_eq(wasm_values.len(), 3)", "assert_eq(array.len(wasm_values), 3)", "assert_eq(array.sum(wasm_values), 17)", "assert_eq(array.avg([2, 4, 6]), 4)", "assert_eq(array.average([3, 6, 9]), 6)", "assert_eq(array_avg([10, 20, 30]), 20)", "assert_eq(array.min(wasm_values), 4)", "assert_eq(array.max(wasm_values), 7)", "assert_eq(array_min([8, -3, 5]), -3)", "assert_eq(array_max([8, -3, 5]), 8)", "range_values is array.range(2, 8, 2)", "assert_eq(range_values.len(), 3)", "assert_eq(array.sum(range_values), 12)", "assert_eq(array_range(4)[3], 3)", "assert_eq(array.range(5, 1, -2)[1], 3)", "assert_eq(array.index_of(wasm_values, 7), 1)", "assert_eq(array.index(wasm_values, 99), -1)", "assert_eq(array.contains(wasm_values, 6), true)", "assert_eq(array.contains(wasm_values, 99), false)", "assert_eq(array_sum([2, 3, 5]), 10)", "wasm_values[1] is 7", "assert_eq(sum3(wasm_values), 17)", "assert_eq(sum3([8, 9, 10]), 27)", "make_values()[0]", "sum3(make_values())", "for item in wasm_values do", "for item in [1, 2, 3] do", "for idx_item, indexed_item in wasm_values do", "for inline_idx, inline_item in [2, 4, 6] do") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("array.len/array.sum/array.avg/array.min/array.max/array.range/array.index_of/array.contains aliases", "numeric array literals, inline array literals, indexing, len(), array.range(), array for-in, indexed array for-in, and array arguments", 'call \$__sura_array_sum', 'call \$__sura_(?:array_avg|value_array_avg)', 'call \$__sura_array_min', 'call \$__sura_array_max', 'call \$__sura_array_range', 'call \$__sura_array_index_of', 'call \$__sura_array_contains', 'call \$sum3', 'call \$make_values', 'local\.set \$range_values', 'call \$__sura_make_array_\d+', 'local\.set \$array_indexed_total') },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("Numeric array literals lower to linear memory", '`arr[index]`', '`array.len(arr)`', '`array.sum(arr)`', '`array.avg(arr)`', '`array.min(arr)`', '`array.max(arr)`', '`array.range(end)`', '`array.range(start, end, step)`', '`array.index_of(arr, value)`', '`array.contains(arr, value)`', "inline numeric array literals", "function-call result indexing", "indexed numeric array loops") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("numeric array literals/indexing", '`array.len`/`array.sum`/`array.avg`/`array.min`/`array.max`/`array.range`/`array.index_of`/`array.contains`', "inline numeric array literals", "function-call result indexing", 'numeric array `for ... in` loops', "indexed numeric array", "array pointer arguments") }
) "restore WASM numeric array literal/index/len, array helper, and indexed for-in lowering, smoke, and docs evidence"
Add-FileEvidence $checks "interop" "wasm_target" "WebAssembly target" @("tools/sura_to_wasm.ps1", "tools/sura_wasm_target_smoke.ps1", "tools/sura_wasm_function_dispatch_smoke.ps1") "restore WASM target smoke coverage"
Add-FileEvidence $checks "interop" "c_header_bindgen" "C/C++ header binding generation" @("tools/sura_bind_c_smoke.ps1", "tools/bindgen_c.ps1") "restore C/C++ bindgen smoke coverage"

Add-FileEvidence $checks "developer_tools" "lsp" "Language server coverage" @("tools/sura_lsp_smoke.js", "sura-vscode/package.json") "restore LSP smoke coverage"
Add-MultiTextEvidence $checks "developer_tools" "lsp_flow_code_actions" "Flow-aware LSP hover and runtime code actions" @(
    @{ path = "main.cpp"; needles = @("(?:is|=)", "add_call_name_conversion_actions", "python_call", "plugin_load_manifest") },
    @{ path = "tools/sura_lsp_smoke.js"; needles = @("count is `"forty-two`"", "Convert python_call to python.call", "Convert plugin_load_manifest to plugin.load_manifest") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("LSP flow/action coverage", "assignment and reassignment", "direct Python/FFI/plugin runtime calls") }
) "restore LSP reassignment-aware hover inference, direct interop code actions, and smoke/doc evidence"
Add-MultiTextEvidence $checks "developer_tools" "vscode_one_click_run" "VS Code one-click Sura run command" @(
    @{ path = "sura-vscode/package.json"; needles = @('"editor/title"', '"editor/title/run"', '"sura.runFile"', '"sura.debugFile"', '"$(play)"', '"$(debug-alt)"', '"default": "sura"', '"sura.language"', '"sura.showRunCodeLens"') },
    @{ path = "sura-vscode/extension.ts"; needles = @("COMPLETION_TRIGGER_CHARS", "currentWordPrefix", "matchesPrefix") },
    @{ path = "tools/sura_vscode_run_button_smoke.ps1"; needles = @("editor/title", "editor/title/run", "resourceLangId == sura", "CodeLens", "COMPLETION_TRIGGER_CHARS", "sura.debugFile", "source checkout override", "--lang") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("VS Code one-click run", "CodeLens", "editor/title/run", "play icon", "first typed character", "sura.enginePath") }
) "restore VS Code editor title run button, Run menu button, Python-like CodeLens, first-character completions, context menu run/debug commands, installed-runtime default, and smoke/docs evidence"
Add-MultiTextEvidence $checks "developer_tools" "diagnostic_language_policy" "English default and Korean optional diagnostics" @(
    @{ path = "main.cpp"; needles = @("--lang", "SURA_LANG", "Runtime Error", "localize_diagnostic", "nil dereference") },
    @{ path = "tools/sura_language_policy_smoke.ps1"; needles = @("Default diagnostics should be English", "SURA_LANG=ko", "--lang ko") },
    @{ path = "sura-vscode/package.json"; needles = @('"sura.language"', '"auto"', '"en"', '"ko"') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("English by default", "Korean opt-in", "SURA_LANG") }
) "restore diagnostic language policy implementation, VS Code setting, smoke, and roadmap evidence"
Add-MultiTextEvidence $checks "developer_tools" "text_encoding_guard" "UTF-8 guide and diagnostic text integrity" @(
    @{ path = "GUIDE.md"; needles = @("SURA_LANG", "sura --lang ko", "Python") },
    @{ path = "Guide/GUIDE.md"; needles = @("SURA_LANG=ko", "sura app.sura", "Python") },
    @{ path = "tools/sura_text_encoding_smoke.ps1"; needles = @("invalid UTF-8 bytes", "mojibake", "sura_text_encoding_smoke") },
    @{ path = "tools/sura_guide_syntax_smoke.ps1"; needles = @("sura_guide_syntax_smoke", "for item in items do", "finally do") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("mojibake", "UTF-8") }
) "restore clean UTF-8 guide text and CI guard against mojibake"
Add-FileEvidence $checks "developer_tools" "formatter" "Formatter coverage" @("tools/sura_engine_format_smoke.ps1", "tools/sura_pkg_format_smoke.ps1") "restore formatter smoke coverage"
Add-FileEvidence $checks "developer_tools" "linter" "Linter coverage" @("tools/sura_engine_lint_smoke.ps1", "tools/sura_pkg_lint_smoke.ps1") "restore linter smoke coverage"
Add-FileEvidence $checks "developer_tools" "debugger" "Debugger coverage" @("tools/sura_debug_smoke.js", "tools/sura_debug_locals_smoke.js", "tools/sura_debug_exception_smoke.js") "restore debugger smoke coverage"
Add-FileEvidence $checks "developer_tools" "profiler" "Profiler coverage" @("tools/sura_profile_smoke.ps1", "tools/sura_pkg_profile_smoke.ps1") "restore profiler smoke coverage"
Add-MultiTextEvidence $checks "developer_tools" "ast_json_export" "Machine-readable AST JSON export" @(
    @{ path = "main.cpp"; needles = @("--ast-json", "sura.ast.v1", "sura_ast_json") },
    @{ path = "tools/sura_ast_json_smoke.ps1"; needles = @("sura.ast.v1", "CLASS_DEF", "METHOD_CALL", "DICT_LIT", "MATCH") },
    @{ path = "tools/sura_to_js.ps1"; needles = @("AstJson", "Convert-JsAstExpr", "Expand-JsAstImports", "AST JSON input: sura.ast.v1") },
    @{ path = "tools/sura_js_target_smoke.ps1"; needles = @("-AstJson", "ast-js-ok", "imported_js_ast") },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("AstJson", "Convert-WasmAstExpr", "Expand-WasmAstImports", "AST JSON input: sura.ast.v1") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("-AstJson", "ast_wasm_target", "imported_ast") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @('machine-readable `--ast-json` export', "sura.ast.v1") }
) "restore AST JSON export, smoke coverage, and roadmap evidence"
Add-FileEvidence $checks "developer_tools" "test_runner" "Test runner coverage" @("tools/sura_test_runner_smoke.ps1", "tools/sura_engine_test_smoke.ps1") "restore package and engine test runner smoke coverage"
Add-FileEvidence $checks "developer_tools" "docs_site" "Searchable docs and package metadata" @("tools/sura_pkg_docs_smoke.ps1", "tools/sura_pkg_info_smoke.ps1", "tools/sura_pkg_search_smoke.ps1") "restore package docs/info/search smoke coverage"

Add-TextEvidence $checks "ai_native" "json_schema" "JSON and schema helpers" "Guide/WORLD_CLASS_ROADMAP.md" @("schema_validate", "schema_to_json_schema") "restore JSON/schema helper evidence"
Add-TextEvidence $checks "ai_native" "http_api" "HTTP/API client helpers" "Guide/WORLD_CLASS_ROADMAP.md" @("http_request", "HTTP helpers") "restore HTTP/API helper evidence"
Add-FileEvidence $checks "ai_native" "tool_calling_policy" "Tool calling and policy gates" @("tools/sura_tool_approval_smoke.ps1", "tools/sura_tool_policy_audit_smoke.ps1", "tools/sura_tool_audit_log_smoke.ps1") "restore tool calling policy smoke coverage"
Add-MultiTextEvidence $checks "ai_native" "embedded_tool_approval_ui" "Embedded/editor tool approval bridge evidence" @(
    @{ path = "stdlib.hpp"; needles = @("SURA_TOOL_APPROVAL_REQUEST_FILE", "SURA_TOOL_APPROVAL_RESPONSE_FILE", "tool_approval_file_granted", "approvalTokenConfigured") },
    @{ path = "tools/sura_tool_approval_smoke.ps1"; needles = @("SURA_TOOL_APPROVAL_REQUEST_FILE", "SURA_TOOL_APPROVAL_RESPONSE_FILE", "approval request/response files", "host UI bridge", "approval file denied") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("SURA_TOOL_APPROVAL_REQUEST_FILE", "SURA_TOOL_APPROVAL_RESPONSE_FILE", "editor/embedded UI JSON approval bridges") }
) "restore embedded/editor approval request-response bridge implementation, docs, and allow/deny smoke coverage"
Add-TextEvidence $checks "ai_native" "async_stream" "Async and stream helpers" "Guide/ECOSYSTEM.md" @("async_http_request", "stream_batch") "restore async/stream helper evidence"
Add-TextEvidence $checks "ai_native" "vector_tensor_llm" "Vector, tensor, RAG, and LLM helpers" "Guide/WORLD_CLASS_ROADMAP.md" @("vectors", "tensors", "RAG", "llm") "restore vector/tensor/RAG/LLM helper evidence"
Add-MultiTextEvidence $checks "runtime_safety" "scheduled_runtime_soak" "Scheduled multi-platform runtime soak evidence" @(
    @{ path = "tools/sura_runtime_soak.ps1"; needles = @("sura.runtime.soak.v1", "runtime_soak_cycle.sura", "PeakWorkingSetBytes", "gc_objects_reclaimed", "PerRunTimeoutSeconds") },
    @{ path = "tools/sura_test_process.ps1"; needles = @("WorkingDirectory", "PeakWorkingSetBytes", "Dispose") },
    @{ path = "tests/runtime_soak_cycle.sura"; needles = @("rounds is 20000", "make_counter", "maybe_throw", "runtime_soak_cycle: PASS") },
    @{ path = ".github/workflows/runtime-soak.yml"; needles = @("schedule:", "windows-latest", "ubuntu-24.04-arm", "macos-15", 'runtime-soak-${{ runner.os }}-${{ runner.arch }}') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("Runtime soak evidence", "not an independent security audit") }
) "restore the bounded soak runner, deterministic allocation/exception fixture, multi-platform schedule, JSON artifacts, and factual limitation note"

Add-MultiTextEvidence $checks "runtime_safety" "public_security_boundary" "Public security policy and honest audit boundary" @(
    @{ path = "SECURITY.md"; needles = @("Supported releases", "not an operating-system sandbox", "FFI and native plugins load native code into the runtime process", "no report from an independent external security audit") },
    @{ path = ".github/ISSUE_TEMPLATE/security_contact.yml"; needles = @("This issue is public", "affected_version", "Do not include exploit details") },
    @{ path = "tools/sura_security_policy_smoke.ps1"; needles = @("maintenance_not_before", "forbiddenClaims", "unsupported assurance claim") },
    @{ path = ".github/workflows/cross-platform-smoke.yml"; needles = @("sura_security_policy_smoke.ps1") },
    @{ path = ".github/workflows/bench-dashboard.yml"; needles = @("sura_security_policy_smoke.ps1") }
) "restore the public security policy, private-contact fallback template, unsupported-claim guard, and both CI integrations"
Add-MultiTextEvidence $checks "runtime_safety" "external_audit_handoff" "Reproducible external security review handoff" @(
    @{ path = "SECURITY_AUDIT.md"; needles = @("independent source review", "not an audit report", "Reproduction commands", "Audit handoff bundle") },
    @{ path = "tools/sura_security_audit_bundle.ps1"; needles = @("sura.security.audit.handoff.v1", "independent_external_audit", "source_files", "included_in_bundle = `$false") },
    @{ path = "tools/sura_security_audit_bundle_smoke.ps1"; needles = @("HANDOFF_CREATED", "manifest integrity mismatch", "forbidden binary", "already exists") },
    @{ path = ".github/workflows/cross-platform-smoke.yml"; needles = @("sura_security_audit_bundle_smoke.ps1") },
    @{ path = ".github/workflows/bench-dashboard.yml"; needles = @("sura_security_audit_bundle.ps1", "security_audit_bundle.json", "sura-security-audit-1.11.1.zip") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("External-review handoff", "no independent external audit report") }
) "restore the source-review scope, hashed handoff generator, integrity smoke, CI bundle artifact, and honest no-audit boundary"

Add-MultiTextEvidence $checks "ai_native" "bounded_bpe" "Bounded deterministic byte-level BPE" @(
    @{ path = "tokenizer.hpp"; needles = @("b_tokenizer_train_bpe", "TOKDATA_MAX_BPE_TRAIN_WORK", "tokenizer.bpe.v1") },
    @{ path = "tests/70_bpe_tokenizer.sura"; needles = @("tokenizer.train_bpe", "corruption_rejected", "forged_rejected", "chunk_bytes: 1") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("tokenizer.train_bpe", "chunk-boundary-preserving dataset packing") },
    @{ path = ".github/workflows/cross-platform-smoke.yml"; needles = @("tests/70_bpe_tokenizer.sura") }
) "restore bounded BPE implementation, safety tests, documentation, and cross-platform coverage"
Add-MultiTextEvidence $checks "ai_native" "bounded_onnx_execution" "Bounded validated CPU ONNX execution" @(
    @{ path = "onnx_weights.hpp"; needles = @("b_autograd_run_onnx", "onnx_exec_transpose", "onnx_exec_flatten", "onnx_exec_reshape", "INT64 initializers are limited", "ONNX_EXEC_MAX_NODES", "unsupported operator") },
    @{ path = "tests/71_onnx_execution.sura"; needles = @("autograd.run_onnx", "autograd.matmul", "autograd.softmax", "shape_outputs", "bad_perm_rejected", "bad_axis_rejected", "reshape_outputs", "reshape_bad_shape_rejected", "reshape_allowzero_rejected", "reshape_wrong_use_rejected", "integer_weight_rejected", "autograd.backward", "typed_dtype_rejected", "typed_shape_rejected", "unsupported_rejected", "opset_rejected", "mutations_completed") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("autograd.run_onnx", "bounded CPU ONNX execution", "Arbitrary graphs") },
    @{ path = ".github/workflows/cross-platform-smoke.yml"; needles = @("tests/71_onnx_execution.sura") }
) "restore bounded ONNX parsing/execution, autograd correctness and rejection tests, documentation, and cross-platform coverage"
Add-MultiTextEvidence $checks "ai_native" "native_neural_networks" "Native neural-network training" @(
    @{ path = "stdlib.hpp"; needles = @("b_nn_mlp", "b_nn_train", "nn_accumulate_gradients", "nn_update_parameters") },
    @{ path = "jit_vm.hpp"; needles = @('module == "nn"', '"train", "nn_train"') },
    @{ path = "tests/12_native_nn.sura"; needles = @("nn.train", "nn.classify", 'task: "regression"') },
    @{ path = "tools/sura_nn_smoke.ps1"; needles = @("bad_width", "bad_target", "bad_standardizer") },
    @{ path = "Guide/AI.md"; needles = @("Adam", "binary", "multiclass", "regression") },
    @{ path = ".github/workflows/cross-platform-smoke.yml"; needles = @("tests/12_native_nn.sura", "ASan + UBSan") }
) "restore native neural-network implementation, tests, documentation, and cross-platform coverage"
Add-MultiTextEvidence $checks "ai_native" "vector_3d_helpers" "3D vector helper coverage" @(
    @{ path = "stdlib.hpp"; needles = @("b_vec3_cross", "b_vec3_distance", '"vec3"') },
    @{ path = "jit_vm.hpp"; needles = @('"vec3"', '"cross"', '"distance3"') },
    @{ path = "tools/sura_to_js.ps1"; needles = @("function vec3", "function vec3_cross", "vec3_distance") },
    @{ path = "tools/sura_to_wasm.ps1"; needles = @("__sura_vec3_cross", "vector.distance3", "integer 3D vector helpers") },
    @{ path = "test_world_features.sura"; needles = @("vec3_cross", "vec3_distance") },
    @{ path = "test_js_target.sura"; needles = @("vector.vec3", "vector.cross") },
    @{ path = "test_wasm_target.sura"; needles = @("vec3_cross", "vector.distance3") },
    @{ path = "tools/sura_stdlib_modules_smoke.ps1"; needles = @("vector.vec3", "vector.distance3") },
    @{ path = "tools/sura_wasm_target_smoke.ps1"; needles = @("__sura_vec3_cross", "integer 3D vector helpers") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("dedicated 3D vector helpers", '`vec3_cross`') },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("3D vector helpers", '`vec3_distance`') }
) "restore direct/native/JS/WASM/module 3D vector helper implementation, tests, and docs"

Add-MultiTextEvidence $checks "performance" "physics_3d_benchmark" "3D physics benchmark coverage" @(
    @{ path = "bench_physics3d.sura"; needles = @("struct Vec3 do", "func cross", "physics 3d step 100k") },
    @{ path = "bench_physics3d.py"; needles = @("class Vec3", "def step3", "avg ({runs} runs)") },
    @{ path = "tools/sura_bench_dashboard.ps1"; needles = @("bench_physics3d.sura", "game physics Vec3 loop") },
    @{ path = "tools/sura_release_evidence_gate.ps1"; needles = @("bench_physics3d.sura", "game physics Vec3 loop") },
    @{ path = "Guide/ECOSYSTEM.md"; needles = @("bench_physics3d", "3D game-physics") },
    @{ path = "Guide/WORLD_CLASS_ROADMAP.md"; needles = @("3D game-physics", "bench_physics3d") }
) "restore the 3D physics benchmark and required dashboard/release-gate evidence"
Add-FileEvidence $checks "ai_native" "agent_scaffold" "AI agent scaffold" @("tools/sura_agent_smoke.ps1") "restore surapkg agent smoke coverage"

Add-MultiTextEvidence $checks "package_ecosystem" "installed_example_gallery" "Installed searchable example gallery and project scaffolding" @(
    @{ path = "surapkg.cpp"; needles = @("sura.package.examples.v1", "sura.package.example.v1", "sura.example.provenance.v1", "SURA_EXAMPLES", "surapkg example <id>") },
    @{ path = "tools/sura_pkg_examples_smoke.ps1"; needles = @("installed-layout example listing", "source_sha256", "unknown or traversal ids", "generated example project did not run") },
    @{ path = "tools/sura_make_installer.ps1"; needles = @('"examples/"', 'Join-Path $InstallDir "examples"') },
    @{ path = "tools/sura_store_msix.ps1"; needles = @("examples/starter/01_hello.sura", "example_count") },
    @{ path = "examples/README.md"; needles = @("surapkg examples", "surapkg example algorithms/word_frequency") },
    @{ path = ".github/workflows/bench-dashboard.yml"; needles = @("sura_pkg_examples_smoke.ps1") },
    @{ path = ".github/workflows/cross-platform-smoke.yml"; needles = @("sura_pkg_examples_smoke.ps1") }
) "restore installed example discovery, byte-preserving scaffolding, provenance, installer/Store payloads, docs, and multi-platform CI"

Add-FileEvidence $checks "performance" "jit" "Native JIT evidence" @("tools/sura_jit_mod_smoke.ps1", "tools/sura_jit_collections_smoke.ps1", "tests/test_jit_collections.sura", "jit_target.hpp", "tools/sura_jit_target_smoke.ps1", "tools/sura_jit_portable_baseline_smoke.ps1", "tests/jit_sysv_baseline_test.cpp", "tools/sura_jit_arm64_baseline_smoke.ps1", "tests/jit_arm64_baseline_test.cpp") "restore native JIT target, Win64 x86-64, Linux System V, and ARM64 baseline smoke coverage"
Add-MultiTextEvidence $checks "performance" "portable_jit_numeric_baseline" "Portable native JIT numeric baseline coverage" @(
    @{ path = "jit_native.hpp"; needles = @("definitely_nonzero", "JitOp::DIV", "JitOp::CMP_EQ", "JitOp::CMP_GTE", "fdiv_d", "fcmp_d", "csel_x") },
    @{ path = "tests/jit_sysv_baseline_test.cpp"; needles = @("JitOp::DIV", "JitOp::CMP_EQ", "JitOp::CMP_GTE", "NaN semantics", "zero_divisor", "dynamic_comparison") },
    @{ path = "tests/jit_arm64_baseline_test.cpp"; needles = @("0x1E611800U", "0x1E612000U", "JitOp::CMP_GTE", "zero_divisor", "dynamic_comparison") },
    @{ path = "COMPATIBILITY.md"; needles = @("System V baseline", "AAPCS64 baseline", "register VM") }
) "restore proven-numeric division/comparison machine lowering, NaN semantics, guarded fallback tests, and compatibility documentation"
Add-FileEvidence $checks "performance" "aot" "AOT launcher evidence" @("tools/sura_aot.ps1", "tools/sura_aot_smoke.ps1") "restore AOT smoke coverage"
Add-FileEvidence $checks "performance" "wasm_output" "WASM output evidence" @("tools/sura_to_wasm.ps1", "tools/sura_wasm_target_smoke.ps1", "tests/wasm_array_addition.sura", "tests/wasm_dynamic_numeric_errors.sura", "tests/wasm_dynamic_decimal_value.sura") "restore WASM target, deep array-producer metadata, catchable dynamic numeric errors, and tagged decimal Value smoke coverage"
Add-FileEvidence $checks "performance" "benchmark_dashboard" "Benchmark dashboard and Python comparisons" @("tools/sura_bench_dashboard.ps1", "tools/sura_bench_gate.ps1", "tools/sura_bench_dashboard_smoke.ps1") "restore benchmark dashboard and Python-comparison gate coverage"
Add-NativePerformanceEvidence $checks $NativePerfJson

Add-FileEvidence $checks "stdlib" "http_server" "HTTP server helpers" @("tools/sura_http_server_smoke.ps1") "restore HTTP server smoke coverage"
Add-FileEvidence $checks "stdlib" "console_api" "Console API helpers" @("tools/sura_console_smoke.ps1", "stdlib.hpp", "jit_vm.hpp", "sura-vscode/extension.ts") "restore native, JS, and editor console API coverage"
Add-TextEvidence $checks "stdlib" "data_time_security_storage" "Regex, datetime, crypto, database, and filesystem helpers" "Guide/WORLD_CLASS_ROADMAP.md" @("use regex", "use datetime", "use crypto", "use db", "use fs") "restore expanded stdlib docs and smoke coverage"
Add-TextEvidence $checks "stdlib" "cli_logging_testing_serialization" "CLI, logging, testing, and serialization helpers" "Guide/WORLD_CLASS_ROADMAP.md" @("use cli", "use log", "use test", "serialization") "restore CLI/log/test/serialization docs and smoke coverage"
Add-FileEvidence $checks "stdlib" "concurrency_random" "Concurrency and random helpers" @("tools/sura_async_smoke.ps1", "tools/sura_random_smoke.ps1") "restore async and random smoke coverage"

Add-OpenRoadmapWork $checks "world1_hosted_registry_service" "Always-on hosted registry with abuse review queues" "Replace the local tokenized HTTP registry workflow with an always-on hosted registry service" "build and verify a hosted registry service with production abuse reporting/review queues"
Add-OpenRoadmapWork $checks "world1_full_wasm_lowering" "Full dynamic AST/bytecode lowering for WASM target" "Finish full dynamic AST/bytecode lowering for the WASM target" "replace proof WASM output with full dynamic AST/bytecode lowering plus parity tests"
Add-OpenRoadmapWork $checks "world1_deeper_lsp_flow" "Broader runtime LSP code actions and flow-aware intelligence" "Continue expanding the runtime LSP with broader code actions and deeper flow-aware hover/signature data" "add deeper flow-aware hover/signature data and broader runtime LSP code actions"
Add-OpenRoadmapWork $checks "world1_native_plugin_cancellation" "Cancellable native plugin execution and per-call cancellation checks" "Expand native plugin lifecycle beyond optional load/unload hooks with cancellable native execution" "expand plugin lifecycle with cancellable native execution and richer per-call cancellation verification"
Add-OpenRoadmapWork $checks "world1_embedded_tool_approval_ui" "Embedded UI tool approval integrations" "Expand tool approval flows beyond" "add richer embedded/editor UI approval integrations beyond command callbacks"
Add-OpenRoadmapWork $checks "world1_more_real_world_benchmarks" "More real-world Python comparison benchmarks" "Keep adding real-world Python comparisons beyond the local recursion" "add more realistic benchmark cases beyond current recursion, AI, tool-routing, policy, dependency, physics, and market simulations"
Add-FileEvidence $checks "world_class_frontier" "target_lowering_audit_visibility" "JS/WASM target lowering audit evidence" @("tools/sura_target_lowering_audit.ps1", "tools/sura_target_lowering_audit_smoke.ps1", "artifacts/target_lowering_audit.json", "artifacts/target_lowering_audit.md") "keep target lowering audit JSON/Markdown evidence in CI before goal and release evidence gates"
Add-FileEvidence $checks "world_class_frontier" "world1_goal_audit_release_visibility" "Goal audit remains in release and Discord evidence flow" @("tools/sura_goal_audit.ps1", "tools/sura_discord_goal_status.ps1", ".github/workflows/bench-dashboard.yml") "keep goal audit and Discord progress reporting in the release evidence flow"

$failed = @($checks | Where-Object { -not $_.passed })
$blockingFailed = @($failed | Where-Object { $_.blocking })
$categories = New-Object System.Collections.Generic.List[object]
foreach ($category in @($checks | ForEach-Object { [string]$_.category } | Sort-Object -Unique)) {
    $items = @($checks | Where-Object { $_.category -eq $category })
    $failedItems = @($items | Where-Object { -not $_.passed })
    $categories.Add([pscustomobject]@{
        name = $category
        required = $items.Count
        passed = ($items.Count - $failedItems.Count)
        failed = $failedItems.Count
        status = $(if ($failedItems.Count -eq 0) { "PASS" } else { "INCOMPLETE" })
    })
}

$checkArray = @()
foreach ($check in $checks) { $checkArray += $check }
$categoryArray = @()
foreach ($category in $categories) { $categoryArray += $category }
$remainingWork = @($failed | ForEach-Object {
    [pscustomobject]@{
        category = $_.category
        id = $_.id
        requirement = $_.name
        message = $_.message
        next_action = $_.next_action
        evidence = $_.evidence
    }
})
$blockers = @($blockingFailed | ForEach-Object {
    [pscustomobject]@{
        category = $_.category
        id = $_.id
        requirement = $_.name
        message = $_.message
        next_action = $_.next_action
    }
})

$progress = if ($checks.Count -gt 0) { [Math]::Round((($checks.Count - $failed.Count) * 100.0) / $checks.Count, 1) } else { 0.0 }
$passed = ($failed.Count -eq 0)
$status = if ($passed) { "PASS" } else { "INCOMPLETE" }

$report = [ordered]@{
    schema = "sura.goal.audit.v1"
    generated_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    repo_root = $root
    passed = $passed
    status = $status
    progress_percent = $progress
    required_count = $checks.Count
    passed_count = ($checks.Count - $failed.Count)
    failed_count = $failed.Count
    blocker_count = $blockingFailed.Count
    max_native_ratio = $MaxNativeRatio
    max_native_evidence_age_hours = $MaxNativeEvidenceAgeHours
    categories = $categoryArray
    checks = $checkArray
    remaining_work = $remainingWork
    blockers = $blockers
}

$jsonPath = Resolve-OutputPath $JsonOut $root
$summaryPath = Resolve-OutputPath $SummaryOut $root
$jsonParent = Split-Path -Parent $jsonPath
if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
[System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 9), (New-Object System.Text.UTF8Encoding($false)))

$tick = [char]96
$summary = New-Object System.Collections.Generic.List[string]
$summary.Add("# Sura Goal Audit")
$summary.Add("")
$summary.Add("Generated UTC: $($report.generated_utc)")
$summary.Add("Status: $status")
$summary.Add("Progress: $progress%")
$summary.Add("Checks: $($report.passed_count)/$($report.required_count) passed")
$summary.Add("Blockers: $($report.blocker_count)")
$summary.Add("")
$summary.Add("## Categories")
foreach ($category in $categoryArray) {
    $summary.Add("- $($category.name): $($category.status) ($($category.passed)/$($category.required))")
}
$summary.Add("")
$summary.Add("## Remaining Work")
if ($remainingWork.Count -eq 0) {
    $summary.Add("- none")
} else {
    foreach ($item in $remainingWork) {
        $summary.Add("- [$($item.category)] $($item.requirement): $($item.message)")
        $summary.Add("  Next: $($item.next_action)")
        if ($item.evidence.Count -gt 0) {
            $joinedEvidence = $item.evidence -join "$tick, $tick"
            $summary.Add("  Evidence: $tick$joinedEvidence$tick")
        }
    }
}
$summaryParent = Split-Path -Parent $summaryPath
if ($summaryParent) { New-Item -ItemType Directory -Force -Path $summaryParent | Out-Null }
[System.IO.File]::WriteAllText($summaryPath, ($summary -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))

if (-not $passed -and $FailOnIncomplete) {
    foreach ($item in $failed) {
        [Console]::Error.WriteLine("[goal audit] $($item.category)/$($item.id): $($item.message)")
    }
    throw "Sura goal audit incomplete with $($failed.Count) remaining requirement(s)"
}

Write-Host ("goal_audit: {0} ({1}% complete, {2}/{3} checks)" -f $status, $progress, $report["passed_count"], $report["required_count"])
