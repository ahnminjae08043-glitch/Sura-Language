param(
    [string]$RepoRoot = ".",
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$Path)
    return (Resolve-Path -LiteralPath $Path).Path
}

function Read-Text {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "file not found: $Path"
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

function Get-Path-Needles {
    param([string]$RelativePath)
    $slash = $RelativePath.Replace("\", "/")
    $back = $RelativePath.Replace("/", "\")
    $leaf = Split-Path -Leaf $RelativePath
    return @(
        $slash,
        $back,
        "./$slash",
        ".\$back",
        ".\\$back",
        $leaf
    ) | Select-Object -Unique
}

function Text-Contains-Any {
    param([string]$Text, [string[]]$Needles)
    foreach ($needle in $Needles) {
        if ([string]::IsNullOrWhiteSpace($needle)) { continue }
        if ($Text.Contains($needle)) { return $true }
    }
    return $false
}

function Text-Contains-Path-Filter {
    param([string]$Text, [string]$RelativePath)
    $slash = $RelativePath.Replace("\", "/")
    $back = $RelativePath.Replace("/", "\")
    $needles = @(
        "- `"$slash`"",
        "- '$slash'",
        "- $slash",
        "- `"$back`"",
        "- '$back'",
        "- $back",
        "tools/**",
        "tests/**",
        "tests/compat/**"
    )
    return Text-Contains-Any $Text $needles
}

function New-Coverage-Check {
    param(
        [string]$Category,
        [string]$Name,
        [string]$Path = "",
        [string[]]$Workflows = @(),
        [string[]]$PathTriggers = @(),
        [string]$Token = "",
        [string]$TokenWorkflow = ""
    )
    return [pscustomobject]@{
        category = $Category
        name = $Name
        path = $Path
        workflows = @(As-Array $Workflows)
        path_triggers = @(As-Array $PathTriggers)
        token = $Token
        token_workflow = $TokenWorkflow
    }
}

function Add-Script-Coverage {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Category,
        [string]$Path,
        [string[]]$Workflows,
        [string[]]$PathTriggers = @("bench")
    )
    $name = Split-Path -Leaf $Path
    $Checks.Add((New-Coverage-Check -Category $Category -Name $name -Path $Path -Workflows $Workflows -PathTriggers $PathTriggers))
}

function Add-Workflow-Token {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Category,
        [string]$Name,
        [string]$Workflow,
        [string]$Token
    )
    $Checks.Add((New-Coverage-Check -Category $Category -Name $Name -Token $Token -TokenWorkflow $Workflow))
}

$root = Resolve-RepoPath $RepoRoot
$workflowFiles = @{
    bench = ".github/workflows/bench-dashboard.yml"
    cross = ".github/workflows/cross-platform-smoke.yml"
    soak = ".github/workflows/runtime-soak.yml"
}

$workflowText = @{}
foreach ($key in $workflowFiles.Keys) {
    $path = Join-Path $root $workflowFiles[$key]
    $workflowText[$key] = Read-Text $path
}

$required = New-Object System.Collections.Generic.List[object]

Add-Script-Coverage $required "ci_coverage_gate" "tools/sura_ci_coverage_gate.ps1" @("bench") @("bench")
Add-Script-Coverage $required "ci_coverage_gate" "tools/sura_ci_coverage_gate_smoke.ps1" @("bench", "cross") @("bench")
Add-Workflow-Token $required "ci_coverage_gate" "benchmark workflow artifact upload" "bench" "artifacts/ci_coverage_gate.json"

Add-Script-Coverage $required "goal_audit" "tools/sura_goal_audit.ps1" @("bench") @("bench")
Add-Script-Coverage $required "goal_audit" "tools/sura_goal_audit_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "goal_audit" "tools/sura_discord_goal_status.ps1" @("bench") @("bench")
Add-Script-Coverage $required "goal_audit" "tools/sura_discord_goal_status_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "goal_audit" "tools/sura_target_lowering_audit.ps1" @("bench") @("bench")
Add-Script-Coverage $required "goal_audit" "tools/sura_target_lowering_audit_smoke.ps1" @("bench") @("bench")
Add-Workflow-Token $required "goal_audit" "goal audit JSON upload" "bench" "artifacts/goal_audit.json"
Add-Workflow-Token $required "goal_audit" "goal audit Markdown upload" "bench" "artifacts/goal_audit.md"
Add-Workflow-Token $required "goal_audit" "target lowering audit JSON upload" "bench" "artifacts/target_lowering_audit.json"
Add-Workflow-Token $required "goal_audit" "target lowering audit Markdown upload" "bench" "artifacts/target_lowering_audit.md"
Add-Workflow-Token $required "goal_audit" "Discord goal status notification" "bench" "sura_discord_goal_status.ps1"
Add-Workflow-Token $required "goal_audit" "Discord webhook secret wiring" "bench" "SURA_DISCORD_WEBHOOK"

Add-Script-Coverage $required "release_evidence" "tools/sura_release_evidence_gate.ps1" @("bench") @("bench")
Add-Script-Coverage $required "release_evidence" "tools/sura_release_evidence_gate_smoke.ps1" @("bench") @("bench")
Add-Workflow-Token $required "release_evidence" "release evidence JSON upload" "bench" "artifacts/release_evidence.json"
Add-Workflow-Token $required "release_evidence" "release evidence Markdown upload" "bench" "artifacts/release_evidence.md"

Add-Workflow-Token $required "cross_platform" "Windows benchmark runner" "bench" "windows-latest"
Add-Workflow-Token $required "cross_platform" "Ubuntu smoke runner" "cross" "ubuntu-latest"
Add-Workflow-Token $required "cross_platform" "Ubuntu ARM64 smoke runner" "cross" "ubuntu-24.04-arm"
Add-Workflow-Token $required "cross_platform" "macOS ARM64 smoke runner" "cross" "macos-15"
Add-Workflow-Token $required "cross_platform" "macOS Intel smoke runner" "cross" "macos-15-intel"
Add-Workflow-Token $required "cross_platform" "PowerShell cross-platform shell" "cross" "shell: pwsh"
Add-Workflow-Token $required "cross_platform" "Ubuntu sanitizer runner" "cross" "Ubuntu ASan + UBSan"
Add-Workflow-Token $required "cross_platform" "ASan and UBSan instrumentation" "cross" "-fsanitize=address,undefined"
Add-Workflow-Token $required "cross_platform" "GC sanitizer execution" "cross" "./tools/sura_gc_memory_safety_smoke.ps1 -Cxx g++ -Sanitize"
Add-Workflow-Token $required "cross_platform" "untrusted-input sanitizer execution" "cross" "./tools/sura_untrusted_input_smoke.ps1 -Cxx g++ -Sanitize"
Add-Workflow-Token $required "cross_platform" "TSan instrumentation" "cross" "-fsanitize=thread"
Add-Workflow-Token $required "cross_platform" "async C++ TSan execution" "cross" "./tools/sura_async_runtime_concurrency_smoke.ps1 -Cxx g++ -ThreadSanitize"
Add-Script-Coverage $required "cross_platform" "tools/sura_jit_target_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "cross_platform" "tools/sura_jit_portable_baseline_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "cross_platform" "tools/sura_jit_arm64_baseline_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "cross_platform" "tests/jit_sysv_baseline_test.cpp" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "cross_platform" "tests/jit_portable_baseline.sura" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "cross_platform" "tests/jit_arm64_baseline_test.cpp" @("bench", "cross") @("bench", "cross")

Add-Script-Coverage $required "stable_engine" "run_stable_tests.ps1" @("bench") @()
Add-Script-Coverage $required "stable_engine" "tools/sura_project_governance_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "SCOPE.md" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "CONTRIBUTING.md" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "runtime_safety" "SECURITY.md" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "runtime_safety" "tools/sura_security_policy_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "runtime_safety" "SECURITY_AUDIT.md" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "runtime_safety" "tools/sura_security_audit_bundle.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "runtime_safety" "tools/sura_security_audit_bundle_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Workflow-Token $required "runtime_safety" "security audit handoff ZIP artifact" "bench" "artifacts/sura-security-audit-1.11.1.zip"
Add-Workflow-Token $required "runtime_safety" "security audit handoff JSON artifact" "bench" "artifacts/security_audit_bundle.json"
Add-Script-Coverage $required "stable_engine" ".github/ISSUE_TEMPLATE/feature_request.yml" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tools/sura_build_contract_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_typechecker_hardening_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_untrusted_input_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tests/parser_untrusted_input_test.cpp" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tests/bytecode_validation_test.cpp" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tools/sura_jit_vm_safety_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_gc_memory_safety_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tools/sura_gc_stats_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "runtime_safety" "tools/sura_runtime_soak.ps1" @("bench", "cross", "soak") @("bench", "cross")
Add-Workflow-Token $required "runtime_safety" "weekly runtime soak schedule" "soak" "schedule:"
Add-Workflow-Token $required "runtime_safety" "runtime soak artifact upload" "soak" 'runtime-soak-${{ runner.os }}-${{ runner.arch }}'
Add-Workflow-Token $required "runtime_safety" "Windows x64 runtime soak" "soak" "windows-latest"
Add-Workflow-Token $required "runtime_safety" "Ubuntu ARM64 runtime soak" "soak" "ubuntu-24.04-arm"
Add-Workflow-Token $required "runtime_safety" "macOS ARM64 runtime soak" "soak" "macos-15"
Add-Workflow-Token $required "runtime_safety" "allocation and exception soak program" "soak" "VM, JIT, GC, exception"
Add-Workflow-Token $required "stable_engine" "VS Code TypeScript, bundle, and extension smoke" "bench" "npm run check"
Add-Script-Coverage $required "stable_engine" "tools/sura_check_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_engine_format_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_engine_lint_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_engine_test_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_undefined_variable_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_cli_args_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_language_policy_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_text_encoding_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_guide_syntax_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_reference_freshness_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tools/sura_compatibility_api_snapshot.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tools/sura_compatibility_gate.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tools/sura_compatibility_gate_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "compatibility.json" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "stable_engine" "tests/compat/1.11/core.sura" @("bench", "cross") @("bench", "cross")
Add-Workflow-Token $required "stable_engine" "compatibility report upload" "bench" "artifacts/compatibility_report.json"
Add-Script-Coverage $required "stable_engine" "tools/sura_utf8_path_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "stable_engine" "tools/sura_instance_fields_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "native_ai" "tests/12_native_nn.sura" @("bench", "cross") @("bench")
Add-Script-Coverage $required "native_ai" "tools/sura_nn_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "native_ai" "tests/13_autograd_core.sura" @("bench", "cross") @("bench")
Add-Script-Coverage $required "native_ai" "tests/14_autograd_gradcheck.sura" @("bench", "cross") @("bench")
Add-Script-Coverage $required "native_ai" "tests/15_autograd_training.sura" @("bench", "cross") @("bench")
Add-Script-Coverage $required "native_ai" "tools/sura_autograd_smoke.ps1" @("bench", "cross") @("bench")

Add-Script-Coverage $required "package_ecosystem" "tools/sura_doctor_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_quality_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_ci_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_release_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_make_installer.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_installer_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_version_sync.ps1" @("bench") @("bench")
Add-Script-Coverage $required "protected_release" "tools/sura_windows_signature_gate.ps1" @("bench") @("bench")
Add-Script-Coverage $required "protected_release" "tools/sura_windows_signature_gate_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_test_runner_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_format_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_check_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_lint_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_profile_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_bench_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_list_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_info_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_docs_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_search_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_tree_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_restore_lock_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_remove_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_version_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_run_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_scaffold_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_pkg_examples_smoke.ps1" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "package_ecosystem" "examples/README.md" @("bench", "cross") @("bench", "cross")
Add-Script-Coverage $required "package_ecosystem" "tools/sura_clean_smoke.ps1" @("bench", "cross") @("bench")

Add-Script-Coverage $required "performance" "tools/sura_bench_gate.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_bench_gate_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_bench_dashboard.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_bench_dashboard_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_bench_dashboard_cli_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_jit_mod_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_jit_collections_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_aot_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_native_perf_baseline.ps1" @("bench") @("bench")
Add-Script-Coverage $required "performance" "tools/sura_native_perf_baseline_smoke.ps1" @("bench") @("bench")
Add-Workflow-Token $required "performance" "native performance JSON upload" "bench" "artifacts/native_perf.json"
Add-Workflow-Token $required "performance" "native performance Markdown upload" "bench" "artifacts/native_perf.md"

Add-Script-Coverage $required "protected_release" "tools/sura_release_pack_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "protected_release" "tools/sura_pkg_protect_smoke.ps1" @("bench") @("bench")

Add-Script-Coverage $required "interop" "tools/sura_python_bridge_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_bind_c_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_embed_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_embed_template_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_js_target_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_wasm_target_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_wasm_function_dispatch_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_wasm_exception_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "interop" "tools/sura_wasm_memory_safety_smoke.ps1" @("bench", "cross") @("bench")
Add-Workflow-Token $required "interop" "benchmark WASM exception runtime execution" "bench" "sura_wasm_exception_smoke.ps1 -Engine"
Add-Workflow-Token $required "interop" "cross-platform WASM exception runtime execution" "cross" "sura_wasm_exception_smoke.ps1 -Engine"
Add-Workflow-Token $required "interop" "benchmark WASM memory safety execution" "bench" "sura_wasm_memory_safety_smoke.ps1 -Engine"
Add-Workflow-Token $required "interop" "cross-platform WASM memory safety execution" "cross" "sura_wasm_memory_safety_smoke.ps1 -Engine"

Add-Script-Coverage $required "registry_security" "tools/sura_registry_client_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "registry_security" "tools/sura_registry_account_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "registry_security" "tools/sura_registry_report_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "registry_security" "tools/sura_registry_service_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "registry_security" "tools/sura_registry_verify_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "registry_security" "tools/sura_public_signature_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "registry_security" "tools/sura_update_smoke.ps1" @("bench", "cross") @("bench")

Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_stdlib_modules_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tests/70_bpe_tokenizer.sura" @("bench", "cross") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tests/71_onnx_execution.sura" @("bench", "cross") @("bench")
Add-Script-Coverage $required "media" "tests/52_media_text_frames.sura" @("bench", "cross") @("bench")
Add-Script-Coverage $required "media" "tools/sura_media_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "media" "tools/sura_bad_apple_ascii_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_console_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_async_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_async_runtime_concurrency_smoke.ps1" @("cross") @("cross")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_random_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_http_server_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_agent_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_policy_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_tool_policy_audit_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_tool_approval_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_tool_audit_log_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "ai_stdlib_automation" "tools/sura_audit_report_smoke.ps1" @("bench") @("bench")

Add-Script-Coverage $required "developer_tools" "tools/sura_profile_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "developer_tools" "tools/sura_ast_json_smoke.ps1" @("bench", "cross") @("bench")
Add-Script-Coverage $required "developer_tools" "tools/sura_lsp_smoke.js" @("bench") @("bench")
Add-Script-Coverage $required "developer_tools" "tools/sura_vscode_run_button_smoke.ps1" @("bench") @("bench")
Add-Script-Coverage $required "developer_tools" "tools/sura_debug_smoke.js" @("bench") @("bench")
Add-Script-Coverage $required "developer_tools" "tools/sura_debug_locals_smoke.js" @("bench") @("bench")
Add-Script-Coverage $required "developer_tools" "tools/sura_debug_exception_smoke.js" @("bench") @("bench")

$results = New-Object System.Collections.Generic.List[object]
$categoryMap = @{}

foreach ($check in $required) {
    $messages = New-Object System.Collections.Generic.List[string]
    $exists = $true
    if (-not [string]::IsNullOrWhiteSpace($check.path)) {
        $full = Join-Path $root $check.path
        $exists = Test-Path -LiteralPath $full
        if (-not $exists) {
            $messages.Add("required file is missing")
        }
    }

    $referencedIn = New-Object System.Collections.Generic.List[string]
    foreach ($workflow in (As-Array $check.workflows)) {
        if (-not $workflowText.ContainsKey($workflow)) {
            $messages.Add("workflow not configured: $workflow")
            continue
        }
        $needles = Get-Path-Needles $check.path
        if (Text-Contains-Any $workflowText[$workflow] $needles) {
            $referencedIn.Add($workflow)
        } else {
            $messages.Add("workflow does not reference $($check.path): $workflow")
        }
    }

    $triggeredIn = New-Object System.Collections.Generic.List[string]
    foreach ($workflow in (As-Array $check.path_triggers)) {
        if (-not $workflowText.ContainsKey($workflow)) {
            $messages.Add("path trigger workflow not configured: $workflow")
            continue
        }
        $needles = Get-Path-Needles $check.path
        if (Text-Contains-Path-Filter $workflowText[$workflow] $check.path) {
            $triggeredIn.Add($workflow)
        } else {
            $messages.Add("workflow path filters do not include $($check.path): $workflow")
        }
    }

    $tokenPresent = $true
    if (-not [string]::IsNullOrWhiteSpace($check.token)) {
        $tokenPresent = $false
        if ($workflowText.ContainsKey($check.token_workflow) -and $workflowText[$check.token_workflow].Contains($check.token)) {
            $tokenPresent = $true
        } else {
            $messages.Add("workflow token missing: $($check.token_workflow) -> $($check.token)")
        }
    }

    $passed = $exists -and $tokenPresent -and ($messages.Count -eq 0)
    $result = [pscustomobject]@{
        category = $check.category
        name = $check.name
        path = $check.path
        exists = $exists
        referenced_in = @($referencedIn)
        path_triggered_in = @($triggeredIn)
        token_workflow = $check.token_workflow
        token = $check.token
        passed = $passed
        messages = @($messages)
    }
    $results.Add($result)

    if (-not $categoryMap.ContainsKey($check.category)) {
        $categoryMap[$check.category] = New-Object System.Collections.Generic.List[object]
    }
    $categoryMap[$check.category].Add($result)
}

$categories = New-Object System.Collections.Generic.List[object]
$categoryKeys = @($results | ForEach-Object { [string]$_.category } | Sort-Object -Unique)
foreach ($category in $categoryKeys) {
    $items = @($results | Where-Object { $_.category -eq $category })
    $failedItems = @($items | Where-Object { -not $_.passed })
    $categories.Add([pscustomobject]@{
        name = $category
        required = $items.Count
        passed = ($items.Count - $failedItems.Count)
        failed = $failedItems.Count
        status = $(if ($failedItems.Count -eq 0) { "PASS" } else { "FAIL" })
    })
}

$failed = @($results | Where-Object { -not $_.passed })
$workflowRecords = @(
    [pscustomobject]@{ name = "bench"; path = ".github/workflows/bench-dashboard.yml" },
    [pscustomobject]@{ name = "cross"; path = ".github/workflows/cross-platform-smoke.yml" }
)
$nextActions = @($failed | ForEach-Object {
    if (-not [string]::IsNullOrWhiteSpace($_.path)) {
        "restore CI coverage for $($_.path)"
    } else {
        "restore workflow token '$($_.token)' in $($_.token_workflow)"
    }
} | Select-Object -Unique)
$categoryArray = @()
foreach ($categoryItem in $categories) { $categoryArray += $categoryItem }
$resultArray = @()
foreach ($resultItem in $results) { $resultArray += $resultItem }

$report = [ordered]@{
    schema = "sura.ci.coverage_gate.v1"
    generated_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    repo_root = $root
    passed = ($failed.Count -eq 0)
    required_count = $results.Count
    passed_count = ($results.Count - $failed.Count)
    failed_count = $failed.Count
    workflows = $workflowRecords
    categories = $categoryArray
    checks = $resultArray
    next_actions = $nextActions
}

if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $jsonPath = $JsonOut
    if (-not [System.IO.Path]::IsPathRooted($jsonPath)) {
        $jsonPath = Join-Path $root $jsonPath
    }
    $parent = Split-Path -Parent $jsonPath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($jsonPath, ($report | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
}

if ($failed.Count -gt 0) {
    foreach ($item in $failed) {
        $detail = ($item.messages -join "; ")
        [Console]::Error.WriteLine("[ci coverage gate] $($item.category)/$($item.name): $detail")
    }
    throw "CI coverage gate failed with $($failed.Count) missing requirement(s)"
}

Write-Host ("ci_coverage_gate: PASS ({0}/{1} required checks)" -f $report["passed_count"], $report["required_count"])
