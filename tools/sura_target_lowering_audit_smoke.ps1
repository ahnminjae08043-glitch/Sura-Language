param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$tool = Join-Path $root "tools/sura_target_lowering_audit.ps1"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_target_lowering_audit_" + [System.Guid]::NewGuid().ToString("N"))
$powerShellExe = (Get-Command pwsh -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if (-not $powerShellExe) {
    $powerShellExe = (Get-Command powershell -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source)
}

try {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "target lowering audit tool not found: $tool"
    }
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $json = Join-Path $temp "target_lowering_audit.json"
    $md = Join-Path $temp "target_lowering_audit.md"

    $out = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $tool -RepoRoot $root -JsonOut $json -SummaryOut $md 2>&1) | Out-String
    if ($LASTEXITCODE -ne 0 -or $out -notmatch "target_lowering_audit:\s+INCOMPLETE") {
        Write-Output $out
        throw "expected target lowering audit to complete with INCOMPLETE status"
    }

    $report = [System.IO.File]::ReadAllText($json, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $summary = [System.IO.File]::ReadAllText($md, [System.Text.Encoding]::UTF8)
    if ($report.schema -ne "sura.target.lowering_audit.v1" -or
        $report.passed -ne $true -or
        $report.lowering_complete -ne $false -or
        $report.pipeline.ast_or_bytecode_frontend -ne $true -or
        $report.pipeline.ast_json_export -ne $true -or
        $report.pipeline.js_ast_json_input -ne $true -or
        $report.pipeline.js_ast_import_expansion -ne $true -or
        $report.pipeline.js_ast_full_target_smoke -ne $true -or
        $report.pipeline.wasm_ast_json_input -ne $true -or
        $report.pipeline.wasm_ast_import_expansion -ne $true -or
        [int]$report.ast_node_count -lt 35 -or
        -not ($report.node_audits | Where-Object { $_.node -eq "STR_LIT" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "SUPER_CALL" -and $_.wasm_status -eq "partial" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "STR_INTERP" -and $_.wasm_status -eq "partial" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "NIL_LIT" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "DICT_LIT" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "NEW_EXPR" -and $_.wasm_status -eq "partial" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "FUNC_EXPR" -and $_.wasm_status -eq "partial" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "NEW_INST" -and $_.js_status -eq "ignored" -and $_.wasm_status -eq "ignored" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "CMD" -and $_.js_status -eq "full" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "CLASS_DEF" -and $_.js_status -eq "full" -and $_.wasm_status -eq "partial" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "DOT_ASSIGN" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "BREAK" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "CONTINUE" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "TRY" -and $_.wasm_status -eq "partial" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "ENUM_DEF" -and $_.wasm_status -eq "full" }) -or
        -not ($report.node_audits | Where-Object { $_.node -eq "IMPORT" -and $_.js_status -eq "full" -and $_.wasm_status -eq "full" }) -or
        $summary -notmatch "# Sura Target Lowering Audit" -or
        $summary -notmatch "Status: INCOMPLETE" -or
        $summary -notmatch "AST JSON export: True" -or
        $summary -notmatch "JS AST JSON input: True" -or
        $summary -notmatch "JS AST import expansion: True" -or
        $summary -notmatch "JS full AST target smoke: True" -or
        $summary -notmatch "WASM AST JSON input: True" -or
        $summary -notmatch "WASM AST import expansion: True" -or
        $summary -notmatch "Full AST/bytecode frontend: True") {
        $report | ConvertTo-Json -Depth 8
        throw "unexpected target lowering audit report"
    }

    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $failOut = (& $powerShellExe -NoProfile -ExecutionPolicy Bypass -File $tool -RepoRoot $root -JsonOut (Join-Path $temp "fail.json") -SummaryOut (Join-Path $temp "fail.md") -FailOnIncomplete 2>&1) | Out-String
    $failCode = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($failCode -eq 0 -or $failOut -notmatch "target lowering incomplete") {
        Write-Output $failOut
        throw "expected -FailOnIncomplete to fail while target lowering is incomplete"
    }

    Write-Host "target_lowering_audit_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
