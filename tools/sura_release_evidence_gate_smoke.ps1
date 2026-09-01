param()

$ErrorActionPreference = "Stop"

$gate = Join-Path $PSScriptRoot "sura_release_evidence_gate.ps1"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_release_evidence_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$powerShellExe = (Get-Command pwsh -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if (-not $powerShellExe) {
    $powerShellExe = (Get-Command powershell -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source)
}

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

try {
    $artifacts = Join-Path $temp "artifacts"
    New-Item -ItemType Directory -Force -Path $artifacts | Out-Null

    $benchmarks = @(
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
    )
    $comparisons = @(
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
    )

    $benchItems = @($benchmarks | ForEach-Object { [pscustomobject]@{ benchmark = $_; speedup = 2.0; jit_ms = 1.0 } })
    $pyItems = @($comparisons | ForEach-Object { [pscustomobject]@{ label = $_; sura_faster_by = 2.0; sura_jit_ms = 1.0; python_ms = 2.0 } })
    $generated = "2026-05-18T00:00:00Z"
    $nativePerf = [ordered]@{
        schema = "sura.native.performance.v1"
        generated_utc = $generated
        passed = $true
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
        sura_jit_ms = 10.0
        sura_jit_time_source = "script_loop"
        sura_loop_scope = "100k"
        native_ms = 1.0
        native_time_source = "native_loop"
        native_loop_scope = "100k"
        sura_native_ratio = 10.0
        native_faster_by = 10.0
        baseline_count = 2
        native_3d_available = $true
        native_3d_sura_native_ratio = 10.0
        native_3d_sura_jit_ms = 20.0
        native_3d_ms = 2.0
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
                sura_jit_ms = 10.0
                native_ms = 1.0
                sura_native_ratio = 10.0
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
                sura_jit_ms = 20.0
                native_ms = 2.0
                sura_native_ratio = 10.0
                warnings = @()
            }
        )
        warnings = @()
    }
    $dashboard = [ordered]@{
        generated_utc = $generated
        summary = [ordered]@{
            benchmark_count = $benchItems.Count
            python_comparison_count = $pyItems.Count
            native_performance_available = $true
        }
        release_notes = [ordered]@{
            headline = "Sura benchmark evidence"
            native_performance = $nativePerf
        }
        native_performance = $nativePerf
        benchmarks = $benchItems
        python_comparisons = $pyItems
    }
    Write-Text (Join-Path $artifacts "bench_dashboard.json") ($dashboard | ConvertTo-Json -Depth 8)
    Write-Text (Join-Path $artifacts "bench_dashboard.html") "<html><body><h1>Benchmark Summary</h1><h2>Native C++ Baseline</h2><p>Sura/native ratio: 10.00x</p><p>game physics Vec3 loop</p><h2>JIT Speedup Chart</h2><svg></svg></body></html>"
    Write-Text (Join-Path $artifacts "bench_summary.md") "# Sura Benchmark Summary`nAverage JIT speedup: 2x`n## Native C++ Baseline`nNative C++ 3D comparison`nSura/native ratio: 10.00x`nPython Comparison`n"
    Write-Text (Join-Path $artifacts "bench_release_notes.md") "# Sura Benchmark Release Notes`nFaster-than-Python cases`n## Native C++ Baseline`nNative C++ 3D baseline`nSura/native ratio: 10.00x`nPython Comparison Highlights`nDashboard JSON includes evidence`n"
    $history = [ordered]@{
        updated_utc = $generated
        entries = @([ordered]@{ generated_utc = $generated; benchmarks = $benchItems; python_comparisons = $pyItems })
    }
    Write-Text (Join-Path $artifacts "bench_history.json") ($history | ConvertTo-Json -Depth 8)
    Write-Text (Join-Path $artifacts "native_perf.json") ($nativePerf | ConvertTo-Json -Depth 8)
    Write-Text (Join-Path $artifacts "native_perf.md") "# Sura Native Performance Baseline`n- C++ flags: -O3 -DNDEBUG -std=c++17`n- Timed region: inner physics loop only`n- Fair scope check: True`n- Sura/native ratio: 10.00x`n- 3D Sura/native ratio: 10.00x`n"
    $targetLowering = [ordered]@{
        schema = "sura.target.lowering_audit.v1"
        generated_utc = $generated
        passed = $true
        status = "INCOMPLETE"
        lowering_complete = $false
        ast_node_count = 42
        pipeline = [ordered]@{
            ast_or_bytecode_frontend = $false
            js_frontend = "source-line transpiler"
            wasm_frontend = "source-line numeric subset transpiler"
        }
    }
    Write-Text (Join-Path $artifacts "target_lowering_audit.json") ($targetLowering | ConvertTo-Json -Depth 8)
    Write-Text (Join-Path $artifacts "target_lowering_audit.md") "# Sura Target Lowering Audit`nStatus: INCOMPLETE`n## Pipeline`n- Full AST/bytecode frontend: False`n## Partial Or Missing Nodes`n"
    $goalAudit = [ordered]@{
        schema = "sura.goal.audit.v1"
        generated_utc = $generated
        passed = $false
        status = "INCOMPLETE"
        progress_percent = 87.0
        required_count = 46
        passed_count = 40
        failed_count = 6
        blocker_count = 6
        categories = @(
            "positioning",
            "package_ecosystem",
            "interop",
            "developer_tools",
            "ai_native",
            "performance",
            "stdlib",
            "world_class_frontier"
        ) | ForEach-Object { [pscustomobject]@{ name = $_; status = $(if ($_ -eq "performance" -or $_ -eq "world_class_frontier") { "INCOMPLETE" } else { "PASS" }); required = 1; passed = $(if ($_ -eq "performance" -or $_ -eq "world_class_frontier") { 0 } else { 1 }); failed = $(if ($_ -eq "performance" -or $_ -eq "world_class_frontier") { 1 } else { 0 }) } }
        remaining_work = @([ordered]@{
            category = "performance"
            id = "native_cpp_speed_goal"
            requirement = "Rust/C++-class native speed proof"
            message = "Sura/native ratio exceeds target"
            next_action = "optimize JIT/AOT hot loops"
        }, [ordered]@{
            category = "world_class_frontier"
            id = "world1_full_js_wasm_lowering"
            requirement = "Full AST/bytecode lowering for JS and WASM targets"
            message = "roadmap still lists this as non-negotiable remaining work"
            next_action = "replace portable-subset JS and proof WASM output"
        })
        blockers = @([ordered]@{
            category = "performance"
            id = "native_cpp_speed_goal"
            requirement = "Rust/C++-class native speed proof"
            message = "Sura/native ratio exceeds target"
            next_action = "optimize JIT/AOT hot loops"
        }, [ordered]@{
            category = "world_class_frontier"
            id = "world1_full_js_wasm_lowering"
            requirement = "Full AST/bytecode lowering for JS and WASM targets"
            message = "roadmap still lists this as non-negotiable remaining work"
            next_action = "replace portable-subset JS and proof WASM output"
        })
    }
    Write-Text (Join-Path $artifacts "goal_audit.json") ($goalAudit | ConvertTo-Json -Depth 8)
    Write-Text (Join-Path $artifacts "goal_audit.md") "# Sura Goal Audit`nProgress: 87.0%`n## Remaining Work`n- [performance] Rust/C++-class native speed proof`n- [world_class_frontier] Full AST/bytecode lowering for JS and WASM targets`n"
    $securityZip = Join-Path $artifacts "sura-security-audit-1.11.1.zip"
    Write-Text $securityZip "security handoff fixture"
    $securityAudit = [ordered]@{
        schema = "sura.security.audit.bundle.report.v1"
        version = "1.11.1"
        status = "HANDOFF_CREATED"
        independent_external_audit_performed = $false
        source_file_count = 198
        archive = [ordered]@{
            path = "sura-security-audit-1.11.1.zip"
            bytes = (Get-Item -LiteralPath $securityZip).Length
            sha256 = (Get-FileHash -LiteralPath $securityZip -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    Write-Text (Join-Path $artifacts "security_audit_bundle.json") ($securityAudit | ConvertTo-Json -Depth 6)
    Write-Text (Join-Path $artifacts "security_audit_bundle.md") "# Sura security audit handoff`n- Independent external audit performed: no`nThis package is not a security certification and does not prove the absence of vulnerabilities.`n"
    $coverage = [ordered]@{
        schema = "sura.ci.coverage_gate.v1"
        passed = $true
        required_count = 72
        passed_count = 72
        failed_count = 0
        categories = @(
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
        ) | ForEach-Object { [pscustomobject]@{ name = $_; status = "PASS"; required = 1; passed = 1; failed = 0 } }
    }
    Write-Text (Join-Path $artifacts "ci_coverage_gate.json") ($coverage | ConvertTo-Json -Depth 8)
    $version = [string](([System.IO.File]::ReadAllText((Join-Path (Split-Path -Parent $PSScriptRoot) "version.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json).version)
    $versionContract = [ordered]@{
        schema = "sura.version.contract.report.v1"
        version = $version
        status = "pass"
        checks = @([ordered]@{ name = "fixture"; passed = $true })
    }
    Write-Text (Join-Path $artifacts "version_contract.json") ($versionContract | ConvertTo-Json -Depth 8)
    $compatibilityContract = [ordered]@{
        schema = "sura.compatibility.report.v1"
        generated_utc = $generated
        passed = $true
        language_version = $version
        stable_series = (($version -split '\.')[0..1] -join '.')
        bytecode = [ordered]@{ current = 3; accepted = @(2, 3) }
        release_package = [ordered]@{ current = 5; accepted = @(1, 2, 3, 4, 5) }
        plugin_abi = "1.1.0"
        ffi_abi = "1.2.0"
        fixtures = @(
            [ordered]@{ path = "tests/compat/1.11/core.sura"; strict_check = "passed"; vm = "passed"; jit_or_fallback = "passed"; bytecode = "passed" },
            [ordered]@{ path = "tests/compat/1.11/objects.sura"; strict_check = "passed"; vm = "passed"; jit_or_fallback = "passed"; bytecode = "passed" },
            [ordered]@{ path = "tests/compat/1.11/stdlib.sura"; strict_check = "passed"; vm = "passed"; jit_or_fallback = "passed"; bytecode = "passed" }
        )
        historical_probes = @(
            [ordered]@{
                series = "1.10"
                runtime_version = "1.10.0"
                status = "verification_only"
                current_runtime = [ordered]@{
                    status = "passed"
                    fixtures = @(
                        [ordered]@{ path = "tests/compat/1.10/core.sura" },
                        [ordered]@{ path = "tests/compat/1.10/objects.sura" },
                        [ordered]@{ path = "tests/compat/1.10/stdlib.sura" }
                    )
                }
                archived_runtime = [ordered]@{
                    status = "passed"
                    bytecode_forward_load = "passed"
                }
            }
        )
    }
    Write-Text (Join-Path $artifacts "compatibility_report.json") ($compatibilityContract | ConvertTo-Json -Depth 8)
    $windowsSignature = [ordered]@{
        schema = "sura.windows.signature.report.v1"
        version = $version
        status = "pass"
        valid_count = 0
        unsigned_count = 3
        direct_download_warning_expected = $true
        next_action = "Publish the Store-certified MSIX or sign direct downloads."
        files = @(
            [ordered]@{ file = "SuraLanguage.exe"; status = "NotSigned"; signed = $false; valid = $false },
            [ordered]@{ file = "surapkg.exe"; status = "NotSigned"; signed = $false; valid = $false },
            [ordered]@{ file = "SuraLanguageSetup.exe"; status = "NotSigned"; signed = $false; valid = $false }
        )
    }
    Write-Text (Join-Path $artifacts "windows_signature.json") ($windowsSignature | ConvertTo-Json -Depth 8)

    $jsonOut = Join-Path $artifacts "release_evidence.json"
    $summaryOut = Join-Path $artifacts "release_evidence.md"
    $out = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -ArtifactsDir $artifacts -JsonOut $jsonOut -SummaryOut $summaryOut 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $out -notmatch "release_evidence_gate:\s+PASS") {
        Write-Output $out
        throw "expected release evidence gate to pass"
    }

    $report = [System.IO.File]::ReadAllText($jsonOut, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $summaryText = [System.IO.File]::ReadAllText($summaryOut, [System.Text.Encoding]::UTF8)
    if ($report.schema -ne "sura.release.evidence_gate.v1" -or
        $report.passed -ne $true -or
        $report.failed_count -ne 0 -or
        $report.required_count -lt 40 -or
        -not ($report.artifacts | Where-Object { $_.path -eq "bench_release_notes.md" -and $_.sha256 }) -or
        -not ($report.artifacts | Where-Object { $_.path -eq "target_lowering_audit.json" -and $_.sha256 }) -or
        -not ($report.artifacts | Where-Object { $_.path -eq "windows_signature.json" -and $_.sha256 }) -or
        $summaryText -notmatch "# Sura Release Evidence" -or
        $summaryText -notmatch "Status: PASS") {
        $report | ConvertTo-Json -Depth 8
        throw "unexpected release evidence report"
    }

    Remove-Item -LiteralPath (Join-Path $artifacts "bench_release_notes.md") -Force
    $brokenJson = Join-Path $artifacts "broken_release_evidence.json"
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $brokenOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $gate -ArtifactsDir $artifacts -JsonOut $brokenJson -SummaryOut (Join-Path $artifacts "broken_release_evidence.md") 2>&1) | Out-String
    $brokenCode = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($brokenCode -eq 0 -or $brokenOut -notmatch "Release evidence gate failed") {
        Write-Output $brokenOut
        throw "expected release evidence gate to fail when release notes are missing"
    }
    $brokenReport = [System.IO.File]::ReadAllText($brokenJson, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($brokenReport.passed -ne $false -or
        $brokenReport.failed_count -lt 1 -or
        -not ($brokenReport.next_actions | Where-Object { $_ -match "bench_release_notes" })) {
        $brokenReport | ConvertTo-Json -Depth 8
        throw "unexpected failing release evidence report"
    }

    "release_evidence_gate_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# The last check above is a negative test, so this script printed PASS while
# inheriting its nonzero exit code. State the verdict explicitly.
exit 0
