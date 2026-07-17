param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$gate = Join-Path $root "tools/sura_goal_audit.ps1"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_goal_audit_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$powerShellExe = (Get-Command pwsh -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if (-not $powerShellExe) {
    $powerShellExe = (Get-Command powershell -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source)
}
$enginePath = @(
    (Join-Path $root "SuraLanguage.exe"),
    (Join-Path $root "SuraLanguage")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $enginePath) { throw "SuraLanguage engine is required for engine-bound goal audit smoke coverage" }
$engineItem = Get-Item -LiteralPath $enginePath
$engineSha256 = (Get-FileHash -LiteralPath $enginePath -Algorithm SHA256).Hash.ToLowerInvariant()

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Write-NativePerf {
    param(
        [string]$Path,
        [double]$Ratio,
        [Nullable[double]]$Vec3Ratio = $null,
        [string]$GeneratedUtc = "",
        [string]$EngineSha256 = "",
        [string]$Schema = "sura.native.performance.v1"
    )
    $vec3RatioValue = if ($null -eq $Vec3Ratio) { $Ratio } else { [double]$Vec3Ratio }
    if ([string]::IsNullOrWhiteSpace($GeneratedUtc)) { $GeneratedUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ") }
    if ([string]::IsNullOrWhiteSpace($EngineSha256)) { $EngineSha256 = $script:engineSha256 }
    $report = [ordered]@{
        schema = $Schema
        generated_utc = $GeneratedUtc
        passed = $true
        engine = [ordered]@{
            file = $script:engineItem.Name
            version_output = "Sura Language test"
            bytes = $script:engineItem.Length
            sha256 = $EngineSha256
        }
        compiler = "test-cxx"
        benchmark = "game physics Vec2 loop"
        sura_file = "bench_physics.sura"
        native_file = "bench_physics_native.cpp"
        measurement_scope = [ordered]@{
            timed_region = "inner physics loop only"
            workload = "Vec2 p = step(p, v, dt)"
            steps = 100000
            step_label = "100k"
            sura_runs = 2
            native_runs = 2
            cxx_flags = "-O3 -DNDEBUG -std=c++17"
        }
        fair_scope_passed = $true
        expected_final_x = 1600.0
        sura_final_x_runs = @(1600.0, 1600.0)
        native_final_x = 1600.0
        sura_jit_ms = $Ratio
        sura_jit_time_source = "script_loop"
        sura_loop_scope = "100k"
        sura_jit_runs = @($Ratio, $Ratio)
        sura_jit_run_count = 2
        native_ms = 1.0
        native_time_source = "native_loop"
        native_loop_scope = "100k"
        sura_native_ratio = $Ratio
        native_faster_by = $Ratio
        baseline_count = 2
        primary_baseline_id = "vec2"
        three_d_baseline_id = "vec3"
        native_3d_available = $true
        native_3d_sura_native_ratio = $vec3RatioValue
        native_3d_sura_jit_ms = $vec3RatioValue
        native_3d_ms = 1.0
        native_3d_fair_scope_passed = $true
        baselines = @(
            [ordered]@{
                id = "vec2"
                benchmark = "game physics Vec2 loop"
                sura_file = "bench_physics.sura"
                native_file = "bench_physics_native.cpp"
                dimension = "vec2"
                measurement_scope = [ordered]@{ timed_region = "inner physics loop only"; steps = 100000; sura_runs = 2; native_runs = 2 }
                fair_scope_passed = $true
                passed = $true
                sura_jit_ms = $Ratio
                sura_jit_time_source = "script_loop"
                sura_loop_scope = "100k"
                sura_jit_runs = @($Ratio, $Ratio)
                sura_jit_run_count = 2
                native_ms = 1.0
                native_time_source = "native_loop"
                native_loop_scope = "100k"
                sura_native_ratio = $Ratio
                warnings = @()
            },
            [ordered]@{
                id = "vec3"
                benchmark = "game physics Vec3 loop"
                sura_file = "bench_physics3d.sura"
                native_file = "bench_physics_native.cpp"
                dimension = "vec3"
                measurement_scope = [ordered]@{ timed_region = "inner physics loop only"; steps = 100000; sura_runs = 2; native_runs = 2 }
                fair_scope_passed = $true
                passed = $true
                sura_jit_ms = $vec3RatioValue
                sura_jit_time_source = "script_loop"
                sura_loop_scope = "100k"
                sura_jit_runs = @($vec3RatioValue, $vec3RatioValue)
                sura_jit_run_count = 2
                native_ms = 1.0
                native_time_source = "native_loop"
                native_loop_scope = "100k"
                sura_native_ratio = $vec3RatioValue
                warnings = @()
            }
        )
        warnings = @()
    }
    Write-Text $Path ($report | ConvertTo-Json -Depth 8)
}

function Invoke-AuditReport {
    param([string]$NativePerfPath, [string]$Name)
    $jsonOut = Join-Path $temp ("goal_audit_" + $Name + ".json")
    $summaryOut = Join-Path $temp ("goal_audit_" + $Name + ".md")
    $auditOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -RepoRoot $root -NativePerfJson $NativePerfPath -JsonOut $jsonOut -SummaryOut $summaryOut 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $auditOut -notmatch "goal_audit:\s+INCOMPLETE") {
        Write-Output $auditOut
        throw "expected $Name goal audit to complete as INCOMPLETE"
    }
    return [System.IO.File]::ReadAllText($jsonOut, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $slowNative = Join-Path $temp "native_perf_slow.json"
    $slowJson = Join-Path $temp "goal_audit_slow.json"
    $slowMd = Join-Path $temp "goal_audit_slow.md"
    Write-NativePerf $slowNative 758.0

    $out = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -RepoRoot $root -NativePerfJson $slowNative -JsonOut $slowJson -SummaryOut $slowMd 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $out -notmatch "goal_audit:\s+INCOMPLETE") {
        Write-Output $out
        throw "expected incomplete goal audit to exit successfully"
    }
    $slowReport = [System.IO.File]::ReadAllText($slowJson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $slowSummary = [System.IO.File]::ReadAllText($slowMd, [System.Text.Encoding]::UTF8)
    if ($slowReport.schema -ne "sura.goal.audit.v1" -or
        $slowReport.passed -ne $false -or
        $slowReport.progress_percent -lt 75 -or
        $slowReport.progress_percent -ge 100 -or
        $slowReport.remaining_work_count -ne $null -or
        -not ($slowReport.remaining_work | Where-Object { $_.id -eq "native_cpp_speed_goal" }) -or
        -not ($slowReport.remaining_work | Where-Object { $_.id -eq "world1_full_wasm_lowering" }) -or
        ($slowReport.remaining_work | Where-Object { $_.id -eq "world1_hosted_registry_service" }) -or
        -not ($slowReport.blockers | Where-Object { $_.id -eq "native_cpp_speed_goal" }) -or
        $slowSummary -notmatch "# Sura Goal Audit" -or
        $slowSummary -notmatch "Remaining Work") {
        $slowReport | ConvertTo-Json -Depth 9
        throw "unexpected incomplete goal audit report"
    }

    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $failOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -RepoRoot $root -NativePerfJson $slowNative -JsonOut (Join-Path $temp "fail.json") -SummaryOut (Join-Path $temp "fail.md") -FailOnIncomplete 2>&1) | Out-String
    $failCode = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($failCode -eq 0 -or $failOut -notmatch "Sura goal audit incomplete") {
        Write-Output $failOut
        throw "expected -FailOnIncomplete to fail"
    }

    $fastNative = Join-Path $temp "native_perf_fast.json"
    $fastJson = Join-Path $temp "goal_audit_fast.json"
    $fastMd = Join-Path $temp "goal_audit_fast.md"
    Write-NativePerf $fastNative 5.0
    $fastOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -RepoRoot $root -NativePerfJson $fastNative -JsonOut $fastJson -SummaryOut $fastMd 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $fastOut -notmatch "goal_audit:\s+INCOMPLETE") {
        Write-Output $fastOut
        throw "expected frontier-incomplete goal audit even with fast native ratio"
    }
    $fastReport = [System.IO.File]::ReadAllText($fastJson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($fastReport.passed -ne $false -or
        $fastReport.progress_percent -ge 100 -or
        $fastReport.failed_count -lt 1 -or
        $fastReport.max_native_evidence_age_hours -ne 168 -or
        $fastReport.required_count -lt 45 -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "hosted_registry_service_packaging" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "native_plugin_cancellation" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "embedded_tool_approval_ui" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "lsp_flow_code_actions" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_class_exception_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_super_init_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_when_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_inline_elif_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "if_inline_else_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_match_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_print_n_space_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_enum_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_struct_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_lambda_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_func_expr_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_optional_coalesce_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_nested_closure_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_indexed_for_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_dict_indexed_for_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_safe_for_iter_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_space_assert_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_loop_control_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_loop_control_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_step_for_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_tilde_for_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_loop_limit_snapshot_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_tilde_when_range_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_when_first_else_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "native_reverse_step_for" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_when_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_match_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_block_match_arm_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_match_first_wildcard_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_nonterminal_default_arm_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_recursive_function_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_inline_elif_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_assert_ne_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_assert_between_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_assert_approx_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_pow_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_sqrt_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_variadic_min_max_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_decimal_rounding_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_signed_decimal_rounding_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_decimal_comparison_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_is_reassignment_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_print_call_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_print_n_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_print_no_nl_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_bool_literal_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_numeric_enum_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_bitwise_shift_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_bitwise_not_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "js_wasm_ternary_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_inline_branch_local_collection" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "wasm_repeat_limit_snapshot_lowering" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "hosted_registry_abuse_review_ops" -and $_.passed -eq $true }) -or
        -not ($fastReport.checks | Where-Object { $_.id -eq "world1_hosted_registry_service" -and $_.passed -eq $true }) -or
        -not ($fastReport.categories | Where-Object { $_.name -eq "performance" -and $_.status -eq "PASS" }) -or
        -not ($fastReport.categories | Where-Object { $_.name -eq "world_class_frontier" -and $_.status -eq "INCOMPLETE" }) -or
        -not ($fastReport.remaining_work | Where-Object { $_.id -eq "world1_full_wasm_lowering" }) -or
        ($fastReport.remaining_work | Where-Object { $_.id -eq "world1_hosted_registry_service" })) {
        $fastReport | ConvertTo-Json -Depth 9
        throw "unexpected frontier-incomplete goal audit report"
    }

    $vec3SlowNative = Join-Path $temp "native_perf_vec3_slow.json"
    Write-NativePerf -Path $vec3SlowNative -Ratio 5.0 -Vec3Ratio 11.0
    $vec3SlowReport = Invoke-AuditReport $vec3SlowNative "vec3_slow"
    if (-not ($vec3SlowReport.checks | Where-Object { $_.id -eq "native_performance_evidence" -and $_.passed -eq $true }) -or
        -not ($vec3SlowReport.checks | Where-Object { $_.id -eq "native_cpp_speed_goal" -and $_.passed -eq $false -and $_.message -match "Vec2 5\.00x and Vec3 11\.00x" })) {
        $vec3SlowReport | ConvertTo-Json -Depth 9
        throw "Vec3 above MaxNativeRatio must fail the combined native speed goal"
    }

    $ratioTamperNative = Join-Path $temp "native_perf_ratio_tamper.json"
    Write-NativePerf $ratioTamperNative 5.0
    $ratioTamper = [System.IO.File]::ReadAllText($ratioTamperNative, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    ($ratioTamper.baselines | Where-Object { $_.id -eq "vec3" }).sura_native_ratio = 1.0
    Write-Text $ratioTamperNative ($ratioTamper | ConvertTo-Json -Depth 8)
    $ratioTamperReport = Invoke-AuditReport $ratioTamperNative "ratio_tamper"
    if (-not ($ratioTamperReport.checks | Where-Object { $_.id -eq "native_performance_evidence" -and $_.passed -eq $false -and $_.message -match "ratio does not equal" }) -or
        -not ($ratioTamperReport.checks | Where-Object { $_.id -eq "native_cpp_speed_goal" -and $_.passed -eq $false })) {
        $ratioTamperReport | ConvertTo-Json -Depth 9
        throw "tampered ratio must fail evidence integrity"
    }

    $runCountTamperNative = Join-Path $temp "native_perf_run_count_tamper.json"
    Write-NativePerf $runCountTamperNative 5.0
    $runCountTamper = [System.IO.File]::ReadAllText($runCountTamperNative, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    ($runCountTamper.baselines | Where-Object { $_.id -eq "vec2" }).sura_jit_run_count = 99
    Write-Text $runCountTamperNative ($runCountTamper | ConvertTo-Json -Depth 8)
    $runCountTamperReport = Invoke-AuditReport $runCountTamperNative "run_count_tamper"
    if (-not ($runCountTamperReport.checks | Where-Object { $_.id -eq "native_performance_evidence" -and $_.passed -eq $false -and $_.message -match "run counts are missing or inconsistent" })) {
        $runCountTamperReport | ConvertTo-Json -Depth 9
        throw "tampered run count must fail evidence integrity"
    }

    $staleNative = Join-Path $temp "native_perf_stale.json"
    Write-NativePerf -Path $staleNative -Ratio 5.0 -GeneratedUtc ([DateTime]::UtcNow.AddHours(-169).ToString("yyyy-MM-ddTHH:mm:ssZ"))
    $staleReport = Invoke-AuditReport $staleNative "stale"
    if (-not ($staleReport.checks | Where-Object { $_.id -eq "native_performance_evidence" -and $_.passed -eq $false -and $_.message -match "older than 168 hours" })) {
        $staleReport | ConvertTo-Json -Depth 9
        throw "stale native performance evidence must fail"
    }

    $wrongEngineSha = if ($engineSha256 -eq ("0" * 64)) { "1" * 64 } else { "0" * 64 }
    $wrongEngineNative = Join-Path $temp "native_perf_wrong_engine.json"
    Write-NativePerf -Path $wrongEngineNative -Ratio 5.0 -EngineSha256 $wrongEngineSha
    $wrongEngineReport = Invoke-AuditReport $wrongEngineNative "wrong_engine"
    if (-not ($wrongEngineReport.checks | Where-Object { $_.id -eq "native_performance_evidence" -and $_.passed -eq $false -and $_.message -match "engine SHA-256" })) {
        $wrongEngineReport | ConvertTo-Json -Depth 9
        throw "evidence for a different engine binary must fail"
    }

    $oldSchemaNative = Join-Path $temp "native_perf_old_schema.json"
    Write-NativePerf -Path $oldSchemaNative -Ratio 5.0 -Schema "sura.native.performance.v0"
    $oldSchemaReport = Invoke-AuditReport $oldSchemaNative "old_schema"
    if (-not ($oldSchemaReport.checks | Where-Object { $_.id -eq "native_performance_evidence" -and $_.passed -eq $false -and $_.message -match "schema is not sura\.native\.performance\.v1" })) {
        $oldSchemaReport | ConvertTo-Json -Depth 9
        throw "non-current native performance schema must fail"
    }

    "goal_audit_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
