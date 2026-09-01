param(
    [string]$Surapkg = ".\surapkg.exe",
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_agent_smoke_" + [System.Guid]::NewGuid().ToString("N"))

function Run-Checked {
    param(
        [string]$Label,
        [scriptblock]$Command,
        [string]$Expect
    )
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Command 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    $text = $out -join "`n"
    if ($code -ne 0 -or ($Expect -and $text -notmatch $Expect)) {
        Write-Output $text
        throw "expected $Label to pass"
    }
    return $text
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    Push-Location $temp
    try {
        Run-Checked "surapkg agent" { & $SurapkgPath agent smart_agent --json agent-report.json } "created AI agent project" | Out-Null
        $project = Join-Path $temp "smart_agent"
        $agentReportPath = Join-Path $temp "agent-report.json"
        $main = Join-Path $project "src/smart_agent.sura"
        $policy = Join-Path $project "sura.tools.json"
        $test = Join-Path $project "tests/agent_test.sura"
        $projectKnowledge = Join-Path $project "knowledge/project.md"
        $suraKnowledge = Join-Path $project "knowledge/sura.md"
        if (-not (Test-Path -LiteralPath $agentReportPath)) {
            throw "expected agent JSON report to be created"
        }
        $agentReport = Get-Content -Raw -Encoding UTF8 -Path $agentReportPath | ConvertFrom-Json
        $agentFiles = @($agentReport.files)
        if ($agentReport.schema -ne "sura.package.agent.v1" -or
            -not $agentReport.passed -or
            $agentReport.package -ne "smart_agent" -or
            $agentReport.main -notmatch "src/smart_agent\.sura" -or
            $agentReport.policy -notmatch "sura\.tools\.json" -or
            $agentReport.knowledge_dir -notmatch "knowledge" -or
            [int]$agentReport.file_count -lt 7 -or
            -not ($agentFiles | Where-Object { $_.kind -eq "main" -and $_.path -match "src/smart_agent\.sura" }) -or
            -not ($agentFiles | Where-Object { $_.kind -eq "tool_policy" -and $_.path -match "sura\.tools\.json" }) -or
            -not ($agentFiles | Where-Object { $_.kind -eq "knowledge" -and $_.path -match "knowledge/project\.md" })) {
            Get-Content -Raw -Encoding UTF8 -Path $agentReportPath | Write-Output
            throw "expected agent JSON report to describe generated template files"
        }

        $mainText = Get-Content -Raw -Encoding UTF8 -Path $main
        $policyText = Get-Content -Raw -Encoding UTF8 -Path $policy
        $testText = Get-Content -Raw -Encoding UTF8 -Path $test
        $projectKnowledgeText = Get-Content -Raw -Encoding UTF8 -Path $projectKnowledge
        $suraKnowledgeText = Get-Content -Raw -Encoding UTF8 -Path $suraKnowledge
        if ($mainText -notmatch "json\.schema_errors" -or
            $mainText -notmatch "http_request" -or
            $mainText -notmatch "tool\.spec" -or
            $mainText -notmatch "tool\.call_policy" -or
            $mainText -notmatch "rag\.prepare" -or
            $mainText -notmatch "llm\.next_messages" -or
            $mainText -notmatch "llm\.next_schema_request" -or
            $mainText -notmatch "llm\.request_tools_schema_json" -or
            $mainText -notmatch "llm\.chat_request" -or
            $mainText -notmatch "llm\.extract_json" -or
            $mainText -notmatch "llm\.request_schema_json" -or
            $mainText -notmatch "agent_tool_names" -or
            $mainText -notmatch "agent_output_schema" -or
            $mainText -notmatch "load_knowledge_docs" -or
            $mainText -notmatch 'fs\.walk\("knowledge", "\.md"\)' -or
            $mainText -notmatch "knowledge/project\.md" -or
            $mainText -notmatch "run_tool_round") {
            throw "generated agent main should use namespaced JSON/tool/RAG/LLM APIs and policy-constrained http_request"
        }
        if ($projectKnowledgeText -notmatch "Project Context" -or
            $projectKnowledgeText -notmatch "games, automation, and AI agents" -or
            $suraKnowledgeText -notmatch "Sura Agent Tools" -or
            $suraKnowledgeText -notmatch "prepared\.sources") {
            throw "generated agent should include source-grounded knowledge documents"
        }
        if ($policyText -notmatch '"http_request"' -or $policyText -notmatch '"http_methods"') {
            throw "generated tool policy should constrain http_request methods"
        }
        if ($testText -notmatch "json\.schema_errors" -or
            $testText -notmatch "tool\.schema\(`"http_request`"\)" -or
            $testText -notmatch "rag\.prepare" -or
            $testText -notmatch "llm\.request_tools_schema_json" -or
            $testText -notmatch "llm\.chat_request" -or
            $testText -notmatch "llm\.extract_json" -or
            $testText -notmatch "llm\.request_tools_json" -or
            $testText -notmatch "llm\.request_schema_json" -or
            $testText -notmatch "llm\.next_messages" -or
            $testText -notmatch "llm\.next_request" -or
            $testText -notmatch "llm\.next_schema_request") {
            throw "generated test should cover JSON schema errors, schema-constrained and tool-enabled LLM requests, http_request schema, RAG prep, and model tool runs through namespaced APIs"
        }

        Push-Location $project
        try {
            $env:SURA_ENGINE = $EnginePath
            Run-Checked "agent runtime" { & $EnginePath --jit ".\src\smart_agent.sura" } '"messages"' | Out-Null
            Run-Checked "agent tests" { & $SurapkgPath test } "1 passed, 0 failed" | Out-Null
            Run-Checked "agent audit" { & $SurapkgPath audit . } "audit passed" | Out-Null
            Run-Checked "agent docs" { & $SurapkgPath docs docs_site } "generated docs" | Out-Null
            $docs = Get-Content -Raw -Encoding UTF8 -Path ".\docs_site\index.html"
            if ($docs -notmatch "API Reference" -or
                $docs -notmatch "Search API" -or
                $docs -notmatch "search-index\.json" -or
                $docs -notmatch "func validate_input\(goal, max_steps\) do" -or
                $docs -notmatch "src/smart_agent\.sura:" -or
                $docs -notmatch "Tool Policy Summary" -or
                $docs -notmatch "http_request" -or
                $docs -notmatch "http_methods" -or
                $docs -notmatch "GET" -or
                $docs -notmatch "validate_input" -or
                $docs -notmatch "agent_tool_names" -or
                $docs -notmatch "agent_output_schema" -or
                $docs -notmatch "build_schema_request" -or
                $docs -notmatch "send_request" -or
                $docs -notmatch "load_knowledge_docs" -or
                $docs -notmatch "run_tool_round") {
                throw "generated docs should include search, tool policy summary, and agent functions"
            }
            $searchIndex = Get-Content -Raw -Encoding UTF8 -Path ".\docs_site\search-index.json"
            if ($searchIndex -notmatch '"type"\s*:\s*"symbol"' -or
                $searchIndex -notmatch '"name"\s*:\s*"validate_input"' -or
                $searchIndex -notmatch '"name"\s*:\s*"agent_tool_names"' -or
                $searchIndex -notmatch '"name"\s*:\s*"agent_output_schema"' -or
                $searchIndex -notmatch '"name"\s*:\s*"build_schema_request"' -or
                $searchIndex -notmatch '"name"\s*:\s*"send_request"' -or
                $searchIndex -notmatch '"name"\s*:\s*"load_knowledge_docs"' -or
                $searchIndex -notmatch '"name"\s*:\s*"run_tool_round"' -or
                $searchIndex -notmatch '"type"\s*:\s*"tool_policy"' -or
                $searchIndex -notmatch '"name"\s*:\s*"http_request"') {
                throw "generated docs search index should include agent functions and tool policy entries"
            }
            Run-Checked "agent quality" { & $SurapkgPath quality . } "quality:\s+PASS" | Out-Null
        }
        finally {
            Remove-Item Env:\SURA_ENGINE -ErrorAction SilentlyContinue
            Pop-Location
        }
    }
    finally {
        Pop-Location
    }

    "agent_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
