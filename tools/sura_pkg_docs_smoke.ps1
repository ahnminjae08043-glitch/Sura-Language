param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_docs_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    Push-Location $temp
    try {
        Write-Text (Join-Path $temp "sura.pkg.json") @"
{
  "name": "docs_pkg",
  "version": "1.2.3",
  "main": "src/docs_pkg.sura",
  "audit_report": "audit-report.json",
  "test_report": "test-report.json",
  "quality_report": "quality-report.json",
  "dependencies": {}
}
"@
        Write-Text (Join-Path $temp "src/docs_pkg.sura") @"
PACKAGE_NAME is "docs-package"
PACKAGE_LIMIT is 42

func add(a, b) do
  local_value is 99
  return a + b
end

struct Point do
  x = 0
  y = 0
end
"@
        Write-Text (Join-Path $temp "sura.tools.json") @"
{
  "version": 1,
  "tools": ["http_request"],
  "url_prefixes": ["https://api.example.com/"],
  "http_methods": ["GET", "POST"],
  "allowed_headers": ["X-Agent"],
  "required_headers": {
    "X-Agent": "docs-smoke"
  },
  "max_body_bytes": 4096,
  "max_timeout": 30,
  "command_prefixes": ["git status"],
  "approval": false,
  "allow_shell": true
}
"@
        Write-Text (Join-Path $temp "sura.plugins.json") @"
{
  "version": 1,
  "sandbox": "manifest-locked",
  "manifests": ["native/sample.sura-plugin.json"],
  "allowed_exports": ["native_add"],
  "host_capabilities": ["memory"],
  "max_memory_bytes": 64,
  "max_call_ms": 100
}
"@
        Write-Text (Join-Path $temp "native/sample.sura-plugin.json") @"
{
  "path": "sample.dll",
  "name": "sample",
  "version": "0.1.0",
  "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "exports": ["native_add"]
}
"@
        Write-Text (Join-Path $temp "audit-report.json") @"
{
  "version": 1,
  "root": ".",
  "passed": false,
  "finding_count": 2,
  "findings": [
    {"kind":"network access","message":"[audit] network access at src/docs_pkg.sura:8","file":"src/docs_pkg.sura","line":8},
    {"kind":"tool_policy","message":"[audit] tool policy requires review","file":"sura.tools.json","line":0}
  ]
}
"@
        Write-Text (Join-Path $temp "test-report.json") @"
{
  "schema": "sura.package.test.v1",
  "root": ".",
  "ok": true,
  "total": 2,
  "passed": 2,
  "failed": 0,
  "jit": true,
  "tests": [
    {"file":"tests/docs_pkg_test.sura","ok":true,"exit_code":0},
    {"file":"tests/docs_pkg_jit_test.sura","ok":true,"exit_code":0}
  ]
}
"@
        Write-Text (Join-Path $temp "quality-report.json") @"
{
  "schema": "sura.package.quality.v1",
  "root": ".",
  "package": "docs_pkg",
  "version": "1.2.3",
  "score": 86,
  "possible": 100,
  "grade": "A",
  "passed": true,
  "warnings": 1,
  "errors": 0,
  "next_actions": [
    {"category":"docs","status":"warn","message":"add release examples","action":"add examples to README.md"}
  ],
  "items": [
    {"status":"pass","earned":20,"possible":20,"category":"manifest","message":"manifest present","action":"review this quality finding"},
    {"status":"warn","earned":6,"possible":10,"category":"docs","message":"docs need release examples","action":"add examples to README.md"}
  ]
}
"@

        $reportPath = Join-Path $temp "docs-report.json"
        $docs = Run-Pkg -PkgArgs @("docs", "docs_site", "--json", $reportPath)
        if ($docs.Code -ne 0 -or
            -not (Test-Path $reportPath) -or
            $docs.Output -notmatch "docs report written" -or
            $docs.Output -notmatch "generated docs") {
            Write-Output $docs.Output
            throw "expected docs --json to generate docs and report"
        }

        $indexPath = Join-Path $temp "docs_site/index.html"
        $apiPath = Join-Path $temp "docs_site/api.json"
        $searchPath = Join-Path $temp "docs_site/search-index.json"
        if (-not (Test-Path $indexPath) -or
            -not (Test-Path $apiPath) -or
            -not (Test-Path $searchPath)) {
            throw "expected docs output files to exist"
        }

        $index = Get-Content -Raw -Encoding UTF8 -Path $indexPath
        if ($index -notmatch "API Reference" -or
            $index -notmatch "Search Docs" -or
            $index -notmatch "Standard Library Modules" -or
            $index -notmatch "Search Index" -or
            $index -notmatch "search-index-table" -or
            $index -notmatch "PACKAGE_NAME is &quot;docs-package&quot;" -or
            $index -notmatch "PACKAGE_LIMIT is 42" -or
            $index -match "local_value is 99" -or
            $index -notmatch "func add\(a, b\) do" -or
            $index -notmatch "struct Point do" -or
            $index -notmatch "array\.slice\(array, start, \[end\]\)" -or
            $index -notmatch "math\.pow\(base, exponent\)" -or
            $index -notmatch "path\.join\(part, \.\.\.\)" -or
            $index -notmatch "python\.call_json\(module, function, \[args\], \[kwargs\]\)" -or
            $index -notmatch "ffi\.call\(lib, symbol, signature, \.\.\.args\)" -or
            $index -notmatch "plugin\.call\(plugin, export, \.\.\.args\)" -or
            $index -notmatch "fs\.read_json\(path\)" -or
            $index -notmatch "fs\.read_bytes\(path\)" -or
            $index -notmatch "fs\.sha256\(path\)" -or
            $index -notmatch "fs\.remove_tree\(path\)" -or
            $index -notmatch "fs\.glob\(pattern\)" -or
            $index -notmatch "crypto\.file_sha256\(path\)" -or
            $index -notmatch "crypto\.file_hmac_sha256\(key, path\)" -or
            $index -notmatch "crypto\.random_bytes\(count\)" -or
            $index -notmatch "crypto\.random_hex\(count\)" -or
            $index -notmatch "crypto\.constant_time_eq\(left, right\)" -or
            $index -notmatch "string\.chunks\(text, \[max_chars\], \[overlap\]\)" -or
            $index -notmatch "os\.env_load\(path, \[override\]\)" -or
            $index -notmatch "os\.temp_dir\(\)" -or
            $index -notmatch "os\.which\(command\)" -or
            $index -notmatch "os\.cmd_quote\(text\)" -or
            $index -notmatch "os\.cmd_join\(args\)" -or
            $index -notmatch "os\.run\(command\)" -or
            $index -notmatch "os\.run_checked\(command\)" -or
            $index -notmatch "cli\.parse\(text, \[value_flags\]\)" -or
            $index -notmatch "autograd\.backward\(tensor, \[gradient\], \[retain_graph\]\)" -or
            $index -notmatch "autograd\.adam\(parameters, learning_rate, \[options\]\)" -or
            $index -notmatch "autograd\.requires_grad\(tensor\)" -or
            $index -notmatch "json\.template_render\(text, data, \[missing\]\)" -or
            $index -notmatch "json\.count_by\(rows, path\)" -or
            $index -notmatch "json\.sse_data\(text, \[parse_json\]\)" -or
            $index -notmatch "json\.ini_parse\(text\)" -or
            $index -notmatch "json\.pretty\(value, \[indent\]\)" -or
            $index -notmatch "test\.approx\(actual, expected, \[epsilon\], \[message\]\)" -or
            $index -notmatch "async\.sleep\(milliseconds, \[scope_id\]\)" -or
            $index -notmatch "async\.scope_close\(scope_id, \[milliseconds\]\)" -or
            $index -notmatch "http\.request_json_checked\(spec\)" -or
            $index -notmatch "http\.request_retry_json_checked\(spec, \[attempts\], \[delay_ms\]\)" -or
            $index -notmatch "http\.auth_bearer\(token\)" -or
            $index -notmatch "http\.headers_merge\(headers, \.\.\.\)" -or
            $index -notmatch "http\.query_parse\(query\)" -or
            $index -notmatch "random\.seed\(seed\)" -or
            $index -notmatch "llm\.request_json\(model, messages, \[temperature\]\)" -or
            $index -notmatch "llm\.request_schema_json\(model, messages, schema, \[temperature\], \[name\], \[strict\]\)" -or
            $index -notmatch "llm\.request_tools_schema_json\(model, messages, tool_names, schema, \[temperature\], \[name\], \[strict\]\)" -or
            $index -notmatch "llm\.chat_request\(endpoint, api_key, request\)" -or
            $index -notmatch "llm\.extract_json\(response, \[schema\]\)" -or
            $index -notmatch "llm\.tool_calls\(response\)" -or
            $index -notmatch "llm\.tool_result\(call_or_id, result\)" -or
            $index -notmatch "llm\.run_tools\(response, policy\)" -or
            $index -notmatch "llm\.next_messages\(messages, response, policy\)" -or
            $index -notmatch "llm\.next_request\(model, messages, response, policy, tool_names, \[temperature\]\)" -or
            $index -notmatch "llm\.next_schema_request\(model, messages, response, policy, tool_names, schema, \[temperature\], \[name\], \[strict\]\)") {
            throw "expected docs HTML to include generated API reference"
        }
        if ($index -notmatch "Plugin Policy Summary" -or
            $index -notmatch "Plugin Policy" -or
            $index -notmatch "plugin_policy" -or
            $index -notmatch "manifest-locked" -or
            $index -notmatch "native/sample\.sura-plugin\.json" -or
            $index -notmatch "native_add" -or
            $index -notmatch "memory" -or
            $index -notmatch "max_memory_bytes" -or
            $index -notmatch "max_call_ms") {
            throw "expected docs HTML to include package plugin policy summary"
        }
        if ($index -notmatch "Tool Policy Summary" -or
            $index -notmatch "Tool Policy" -or
            $index -notmatch "tool_policy" -or
            $index -notmatch "allowed_header" -or
            $index -notmatch "required_header" -or
            $index -notmatch "command_prefix" -or
            $index -notmatch "http_request" -or
            $index -notmatch "https://api\.example\.com/" -or
            $index -notmatch "GET" -or
            $index -notmatch "POST" -or
            $index -notmatch "X-Agent" -or
            $index -notmatch "git status" -or
            $index -notmatch "max_body_bytes" -or
            $index -notmatch "max_timeout") {
            throw "expected docs HTML to include package tool policy summary"
        }
        if ($index -notmatch "Security Audit Summary" -or
            $index -notmatch "audit-report\.json" -or
            $index -notmatch "network access" -or
            $index -notmatch "src/docs_pkg\.sura:8" -or
            $index -notmatch "tool_policy") {
            throw "expected docs HTML to include package security audit summary"
        }
        if ($index -notmatch "Test Summary" -or
            $index -notmatch "test-report\.json" -or
            $index -notmatch "<code>true</code>" -or
            $index -notmatch "<code>2</code>") {
            throw "expected docs HTML to include package test summary"
        }
        if ($index -notmatch "Quality Summary" -or
            $index -notmatch "quality-report\.json" -or
            $index -notmatch "<code>86/100</code>" -or
            $index -notmatch "<code>A</code>" -or
            $index -notmatch "<code>true</code>" -or
            $index -notmatch "<code>1</code>") {
            throw "expected docs HTML to include package quality summary"
        }

        $api = Get-Content -Raw -Encoding UTF8 -Path $apiPath | ConvertFrom-Json
        if (-not $api.toolPolicy -or
            $api.toolPolicy.source -ne "sura.tools.json" -or
            @($api.toolPolicy.tools) -notcontains "http_request" -or
            @($api.toolPolicy.urlPrefixes) -notcontains "https://api.example.com/" -or
            @($api.toolPolicy.httpMethods) -notcontains "GET" -or
            @($api.toolPolicy.httpMethods) -notcontains "POST" -or
            @($api.toolPolicy.allowedHeaders) -notcontains "X-Agent" -or
            $api.toolPolicy.requiredHeaders.'X-Agent' -ne "docs-smoke" -or
            $api.toolPolicy.maxBodyBytes -ne 4096 -or
            $api.toolPolicy.maxTimeout -ne 30 -or
            @($api.toolPolicy.commandPrefixes) -notcontains "git status" -or
            $api.toolPolicy.allowShell -ne $true -or
            $api.toolPolicy.approval -ne $false -or
            $api.toolPolicy.approvalTokenConfigured -ne $false) {
            $api.toolPolicy | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected api.json to include package tool policy metadata"
        }
        if (-not $api.pluginPolicy -or
            $api.pluginPolicy.source -ne "sura.plugins.json" -or
            $api.pluginPolicy.sandbox -ne "manifest-locked" -or
            @($api.pluginPolicy.manifests) -notcontains "native/sample.sura-plugin.json" -or
            @($api.pluginPolicy.allowedExports) -notcontains "native_add" -or
            @($api.pluginPolicy.hostCapabilities) -notcontains "memory" -or
            $api.pluginPolicy.maxMemoryBytes -ne 64 -or
            $api.pluginPolicy.maxCallMs -ne 100) {
            $api.pluginPolicy | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected api.json to include package plugin policy metadata"
        }
        if (-not $api.audit -or
            $api.audit.source -ne "audit-report.json" -or
            $api.audit.passed -ne $false -or
            $api.audit.findingCount -ne 2 -or
            @($api.audit.findings).Count -ne 2 -or
            $api.audit.findings[0].kind -ne "network access" -or
            $api.audit.findings[0].source -ne "src/docs_pkg.sura" -or
            $api.audit.findings[0].line -ne 8 -or
            $api.audit.findings[1].kind -ne "tool_policy") {
            $api.audit | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected api.json to include package security audit metadata"
        }
        if (-not $api.tests -or
            $api.tests.source -ne "test-report.json" -or
            $api.tests.ok -ne $true -or
            $api.tests.total -ne 2 -or
            $api.tests.passed -ne 2 -or
            $api.tests.failed -ne 0 -or
            $api.tests.jit -ne $true) {
            $api.tests | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected api.json to include package test metadata"
        }
        if (-not $api.quality -or
            $api.quality.source -ne "quality-report.json" -or
            $api.quality.score -ne 86 -or
            $api.quality.possible -ne 100 -or
            $api.quality.grade -ne "A" -or
            $api.quality.passed -ne $true -or
            $api.quality.warnings -ne 1 -or
            $api.quality.errors -ne 0) {
            $api.quality | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected api.json to include package quality metadata"
        }
        $cliModule = @($api.stdlibModules) | Where-Object { $_.name -eq "cli" } | Select-Object -First 1
        $cliParse = @($cliModule.symbols) | Where-Object {
            $_.name -eq "parse" -and $_.signature -eq "cli.parse(text, [value_flags])"
        } | Select-Object -First 1
        $autogradModule = @($api.stdlibModules) | Where-Object { $_.name -eq "autograd" } | Select-Object -First 1
        $autogradBackward = @($autogradModule.symbols) | Where-Object {
            $_.name -eq "backward" -and $_.signature -eq "autograd.backward(tensor, [gradient], [retain_graph])"
        } | Select-Object -First 1
        $autogradAdam = @($autogradModule.symbols) | Where-Object {
            $_.name -eq "adam" -and $_.signature -eq "autograd.adam(parameters, learning_rate, [options])"
        } | Select-Object -First 1
        $autogradRequiresGrad = @($autogradModule.symbols) | Where-Object {
            $_.name -eq "requires_grad" -and $_.signature -eq "autograd.requires_grad(tensor)"
        } | Select-Object -First 1
        $llmModule = @($api.stdlibModules) | Where-Object { $_.name -eq "llm" } | Select-Object -First 1
        $llmRequestJson = @($llmModule.symbols) | Where-Object {
            $_.name -eq "request_json" -and $_.signature -eq "llm.request_json(model, messages, [temperature])"
        } | Select-Object -First 1
        $llmRequestSchemaJson = @($llmModule.symbols) | Where-Object {
            $_.name -eq "request_schema_json" -and $_.signature -eq "llm.request_schema_json(model, messages, schema, [temperature], [name], [strict])"
        } | Select-Object -First 1
        $llmRequestToolsSchemaJson = @($llmModule.symbols) | Where-Object {
            $_.name -eq "request_tools_schema_json" -and $_.signature -eq "llm.request_tools_schema_json(model, messages, tool_names, schema, [temperature], [name], [strict])"
        } | Select-Object -First 1
        $llmChatRequest = @($llmModule.symbols) | Where-Object {
            $_.name -eq "chat_request" -and $_.signature -eq "llm.chat_request(endpoint, api_key, request)"
        } | Select-Object -First 1
        $llmExtractJson = @($llmModule.symbols) | Where-Object {
            $_.name -eq "extract_json" -and $_.signature -eq "llm.extract_json(response, [schema])"
        } | Select-Object -First 1
        $llmToolCalls = @($llmModule.symbols) | Where-Object {
            $_.name -eq "tool_calls" -and $_.signature -eq "llm.tool_calls(response)"
        } | Select-Object -First 1
        $llmToolResult = @($llmModule.symbols) | Where-Object {
            $_.name -eq "tool_result" -and $_.signature -eq "llm.tool_result(call_or_id, result)"
        } | Select-Object -First 1
        $llmRunTools = @($llmModule.symbols) | Where-Object {
            $_.name -eq "run_tools" -and $_.signature -eq "llm.run_tools(response, policy)"
        } | Select-Object -First 1
        $llmNextMessages = @($llmModule.symbols) | Where-Object {
            $_.name -eq "next_messages" -and $_.signature -eq "llm.next_messages(messages, response, policy)"
        } | Select-Object -First 1
        $llmNextRequest = @($llmModule.symbols) | Where-Object {
            $_.name -eq "next_request" -and $_.signature -eq "llm.next_request(model, messages, response, policy, tool_names, [temperature])"
        } | Select-Object -First 1
        $llmNextRequestJson = @($llmModule.symbols) | Where-Object {
            $_.name -eq "next_request_json" -and $_.signature -eq "llm.next_request_json(model, messages, response, policy, tool_names, [temperature])"
        } | Select-Object -First 1
        $llmNextSchemaRequest = @($llmModule.symbols) | Where-Object {
            $_.name -eq "next_schema_request" -and $_.signature -eq "llm.next_schema_request(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])"
        } | Select-Object -First 1
        $llmNextSchemaRequestJson = @($llmModule.symbols) | Where-Object {
            $_.name -eq "next_schema_request_json" -and $_.signature -eq "llm.next_schema_request_json(model, messages, response, policy, tool_names, schema, [temperature], [name], [strict])"
        } | Select-Object -First 1
        $jsonModule = @($api.stdlibModules) | Where-Object { $_.name -eq "json" } | Select-Object -First 1
        $jsonTemplate = @($jsonModule.symbols) | Where-Object {
            $_.name -eq "template_render" -and $_.signature -eq "json.template_render(text, data, [missing])"
        } | Select-Object -First 1
        $jsonCountBy = @($jsonModule.symbols) | Where-Object {
            $_.name -eq "count_by" -and $_.signature -eq "json.count_by(rows, path)"
        } | Select-Object -First 1
        $jsonSseData = @($jsonModule.symbols) | Where-Object {
            $_.name -eq "sse_data" -and $_.signature -eq "json.sse_data(text, [parse_json])"
        } | Select-Object -First 1
        $jsonIniParse = @($jsonModule.symbols) | Where-Object {
            $_.name -eq "ini_parse" -and $_.signature -eq "json.ini_parse(text)"
        } | Select-Object -First 1
        $jsonPretty = @($jsonModule.symbols) | Where-Object {
            $_.name -eq "pretty" -and $_.signature -eq "json.pretty(value, [indent])"
        } | Select-Object -First 1
        $asyncModule = @($api.stdlibModules) | Where-Object { $_.name -eq "async" } | Select-Object -First 1
        $asyncSleep = @($asyncModule.symbols) | Where-Object {
            $_.name -eq "sleep" -and $_.signature -eq "async.sleep(milliseconds, [scope_id])"
        } | Select-Object -First 1
        $testModule = @($api.stdlibModules) | Where-Object { $_.name -eq "test" } | Select-Object -First 1
        $testApprox = @($testModule.symbols) | Where-Object {
            $_.name -eq "approx" -and $_.signature -eq "test.approx(actual, expected, [epsilon], [message])"
        } | Select-Object -First 1
        $testCheckMatch = @($testModule.symbols) | Where-Object {
            $_.name -eq "check_match" -and $_.signature -eq "test.check_match(name, text, pattern, [message])"
        } | Select-Object -First 1
        $httpModule = @($api.stdlibModules) | Where-Object { $_.name -eq "http" } | Select-Object -First 1
        $httpCheckedJson = @($httpModule.symbols) | Where-Object {
            $_.name -eq "request_json_checked" -and $_.signature -eq "http.request_json_checked(spec)"
        } | Select-Object -First 1
        $httpRetryCheckedJson = @($httpModule.symbols) | Where-Object {
            $_.name -eq "request_retry_json_checked" -and $_.signature -eq "http.request_retry_json_checked(spec, [attempts], [delay_ms])"
        } | Select-Object -First 1
        $httpAuthBearer = @($httpModule.symbols) | Where-Object {
            $_.name -eq "auth_bearer" -and $_.signature -eq "http.auth_bearer(token)"
        } | Select-Object -First 1
        $httpHeadersMerge = @($httpModule.symbols) | Where-Object {
            $_.name -eq "headers_merge" -and $_.signature -eq "http.headers_merge(headers, ...)"
        } | Select-Object -First 1
        $httpQueryParse = @($httpModule.symbols) | Where-Object {
            $_.name -eq "query_parse" -and $_.signature -eq "http.query_parse(query)"
        } | Select-Object -First 1
        $randomModule = @($api.stdlibModules) | Where-Object { $_.name -eq "random" } | Select-Object -First 1
        $randomSeed = @($randomModule.symbols) | Where-Object {
            $_.name -eq "seed" -and $_.signature -eq "random.seed(seed)"
        } | Select-Object -First 1
        $mathModule = @($api.stdlibModules) | Where-Object { $_.name -eq "math" } | Select-Object -First 1
        $mathPow = @($mathModule.symbols) | Where-Object {
            $_.name -eq "pow" -and $_.signature -eq "math.pow(base, exponent)"
        } | Select-Object -First 1
        $stringModule = @($api.stdlibModules) | Where-Object { $_.name -eq "string" } | Select-Object -First 1
        $stringChunks = @($stringModule.symbols) | Where-Object {
            $_.name -eq "chunks" -and $_.signature -eq "string.chunks(text, [max_chars], [overlap])"
        } | Select-Object -First 1
        $osModule = @($api.stdlibModules) | Where-Object { $_.name -eq "os" } | Select-Object -First 1
        $osEnvLoad = @($osModule.symbols) | Where-Object {
            $_.name -eq "env_load" -and $_.signature -eq "os.env_load(path, [override])"
        } | Select-Object -First 1
        $osTempDir = @($osModule.symbols) | Where-Object {
            $_.name -eq "temp_dir" -and $_.signature -eq "os.temp_dir()"
        } | Select-Object -First 1
        $osName = @($osModule.symbols) | Where-Object {
            $_.name -eq "name" -and $_.signature -eq "os.name()"
        } | Select-Object -First 1
        $osWhich = @($osModule.symbols) | Where-Object {
            $_.name -eq "which" -and $_.signature -eq "os.which(command)"
        } | Select-Object -First 1
        $osCmdExists = @($osModule.symbols) | Where-Object {
            $_.name -eq "cmd_exists" -and $_.signature -eq "os.cmd_exists(command)"
        } | Select-Object -First 1
        $osCmdQuote = @($osModule.symbols) | Where-Object {
            $_.name -eq "cmd_quote" -and $_.signature -eq "os.cmd_quote(text)"
        } | Select-Object -First 1
        $osCmdJoin = @($osModule.symbols) | Where-Object {
            $_.name -eq "cmd_join" -and $_.signature -eq "os.cmd_join(args)"
        } | Select-Object -First 1
        $osRun = @($osModule.symbols) | Where-Object {
            $_.name -eq "run" -and $_.signature -eq "os.run(command)"
        } | Select-Object -First 1
        $osRunChecked = @($osModule.symbols) | Where-Object {
            $_.name -eq "run_checked" -and $_.signature -eq "os.run_checked(command)"
        } | Select-Object -First 1
        $arrayModule = @($api.stdlibModules) | Where-Object { $_.name -eq "array" } | Select-Object -First 1
        $arraySlice = @($arrayModule.symbols) | Where-Object {
            $_.name -eq "slice" -and $_.signature -eq "array.slice(array, start, [end])"
        } | Select-Object -First 1
        $pathModule = @($api.stdlibModules) | Where-Object { $_.name -eq "path" } | Select-Object -First 1
        $pathJoin = @($pathModule.symbols) | Where-Object {
            $_.name -eq "join" -and $_.signature -eq "path.join(part, ...)"
        } | Select-Object -First 1
        $fsModule = @($api.stdlibModules) | Where-Object { $_.name -eq "fs" } | Select-Object -First 1
        $fsReadJson = @($fsModule.symbols) | Where-Object {
            $_.name -eq "read_json" -and $_.signature -eq "fs.read_json(path)"
        } | Select-Object -First 1
        $fsReadBytes = @($fsModule.symbols) | Where-Object {
            $_.name -eq "read_bytes" -and $_.signature -eq "fs.read_bytes(path)"
        } | Select-Object -First 1
        $fsSha256 = @($fsModule.symbols) | Where-Object {
            $_.name -eq "sha256" -and $_.signature -eq "fs.sha256(path)"
        } | Select-Object -First 1
        $fsRemoveTree = @($fsModule.symbols) | Where-Object {
            $_.name -eq "remove_tree" -and $_.signature -eq "fs.remove_tree(path)"
        } | Select-Object -First 1
        $fsGlob = @($fsModule.symbols) | Where-Object {
            $_.name -eq "glob" -and $_.signature -eq "fs.glob(pattern)"
        } | Select-Object -First 1
        $cryptoModule = @($api.stdlibModules) | Where-Object { $_.name -eq "crypto" } | Select-Object -First 1
        $cryptoFileSha256 = @($cryptoModule.symbols) | Where-Object {
            $_.name -eq "file_sha256" -and $_.signature -eq "crypto.file_sha256(path)"
        } | Select-Object -First 1
        $cryptoFileHmacSha256 = @($cryptoModule.symbols) | Where-Object {
            $_.name -eq "file_hmac_sha256" -and $_.signature -eq "crypto.file_hmac_sha256(key, path)"
        } | Select-Object -First 1
        $cryptoRandomBytes = @($cryptoModule.symbols) | Where-Object {
            $_.name -eq "random_bytes" -and $_.signature -eq "crypto.random_bytes(count)"
        } | Select-Object -First 1
        $cryptoRandomHex = @($cryptoModule.symbols) | Where-Object {
            $_.name -eq "random_hex" -and $_.signature -eq "crypto.random_hex(count)"
        } | Select-Object -First 1
        $cryptoConstantTimeEq = @($cryptoModule.symbols) | Where-Object {
            $_.name -eq "constant_time_eq" -and $_.signature -eq "crypto.constant_time_eq(left, right)"
        } | Select-Object -First 1
        $pythonModule = @($api.stdlibModules) | Where-Object { $_.name -eq "python" } | Select-Object -First 1
        $pythonCallJson = @($pythonModule.symbols) | Where-Object {
            $_.name -eq "call_json" -and $_.signature -eq "python.call_json(module, function, [args], [kwargs])"
        } | Select-Object -First 1
        $ffiModule = @($api.stdlibModules) | Where-Object { $_.name -eq "ffi" } | Select-Object -First 1
        $ffiCall = @($ffiModule.symbols) | Where-Object {
            $_.name -eq "call" -and $_.signature -eq "ffi.call(lib, symbol, signature, ...args)"
        } | Select-Object -First 1
        $pluginModule = @($api.stdlibModules) | Where-Object { $_.name -eq "plugin" } | Select-Object -First 1
        $pluginCall = @($pluginModule.symbols) | Where-Object {
            $_.name -eq "call" -and $_.signature -eq "plugin.call(plugin, export, ...args)"
        } | Select-Object -First 1
        $graphics3dModule = @($api.stdlibModules) | Where-Object { $_.name -eq "graphics3d" } | Select-Object -First 1
        $graphics3dProject = @($graphics3dModule.symbols) | Where-Object {
            $_.name -eq "project" -and $_.signature -eq "graphics3d.project(point, camera, [width], [height])"
        } | Select-Object -First 1
        $graphics3dCube = @($graphics3dModule.symbols) | Where-Object {
            $_.name -eq "cube" -and $_.signature -eq "graphics3d.cube([size], [center])"
        } | Select-Object -First 1
        if ($api.name -ne "docs_pkg" -or
            $api.version -ne "1.2.3" -or
            $api.package_symbol_count -ne 4 -or
            $api.symbols.Count -ne 4 -or
            $api.symbols[0].kind -ne "constant" -or
            $api.symbols[0].name -ne "PACKAGE_NAME" -or
            $api.symbols[0].signature -ne 'PACKAGE_NAME is "docs-package"' -or
            $api.symbols[1].kind -ne "constant" -or
            $api.symbols[1].name -ne "PACKAGE_LIMIT" -or
            $api.symbols[2].name -ne "add" -or
            $api.symbols[3].name -ne "Point" -or
            (@($api.symbols) | Where-Object { $_.name -eq "local_value" }) -or
            $api.stdlib_module_count -lt 27 -or
            $api.stdlib_symbol_count -lt 185 -or
            -not $cliModule -or
            -not $cliParse -or
            -not $autogradModule -or
            -not $autogradBackward -or
            -not $autogradAdam -or
            -not $autogradRequiresGrad -or
            -not $llmModule -or
            -not $llmRequestJson -or
            -not $llmRequestSchemaJson -or
            -not $llmRequestToolsSchemaJson -or
            -not $llmChatRequest -or
            -not $llmExtractJson -or
            -not $llmToolCalls -or
            -not $llmToolResult -or
            -not $llmRunTools -or
            -not $llmNextMessages -or
            -not $llmNextRequest -or
            -not $llmNextRequestJson -or
            -not $llmNextSchemaRequest -or
            -not $llmNextSchemaRequestJson -or
            -not $jsonModule -or
            -not $jsonTemplate -or
            -not $jsonCountBy -or
            -not $jsonSseData -or
            -not $jsonIniParse -or
            -not $jsonPretty -or
            -not $asyncModule -or
            -not $asyncSleep -or
            -not $testModule -or
            -not $testApprox -or
            -not $testCheckMatch -or
            -not $httpModule -or
            -not $httpCheckedJson -or
            -not $httpRetryCheckedJson -or
            -not $httpAuthBearer -or
            -not $httpHeadersMerge -or
            -not $httpQueryParse -or
            -not $randomModule -or
            -not $randomSeed -or
            -not $mathModule -or
            -not $mathPow -or
            -not $arrayModule -or
            -not $arraySlice -or
            -not $pathModule -or
            -not $pathJoin -or
            -not $fsModule -or
            -not $fsReadJson -or
            -not $fsReadBytes -or
            -not $fsSha256 -or
            -not $fsRemoveTree -or
            -not $fsGlob -or
            -not $cryptoModule -or
            -not $cryptoFileSha256 -or
            -not $cryptoFileHmacSha256 -or
            -not $cryptoRandomBytes -or
            -not $cryptoRandomHex -or
            -not $cryptoConstantTimeEq -or
            -not $pythonModule -or
            -not $pythonCallJson -or
            -not $ffiModule -or
            -not $ffiCall -or
            -not $pluginModule -or
            -not $pluginCall -or
            -not $graphics3dModule -or
            -not $graphics3dProject -or
            -not $graphics3dCube -or
            -not $stringModule -or
            -not $stringChunks -or
            -not $osModule -or
            -not $osEnvLoad -or
            -not $osTempDir -or
            -not $osName -or
            -not $osWhich -or
            -not $osCmdExists -or
            -not $osCmdQuote -or
            -not $osCmdJoin -or
            -not $osRun -or
            -not $osRunChecked) {
            $api | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected api.json to include package and stdlib symbols"
        }

        $search = Get-Content -Raw -Encoding UTF8 -Path $searchPath | ConvertFrom-Json
        $toolPolicySourceSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "policy" -and $_.name -eq "sura.tools.json"
        } | Select-Object -First 1
        $toolRequestSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "tool" -and $_.name -eq "http_request"
        } | Select-Object -First 1
        $toolGetSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "http_method" -and $_.name -eq "GET"
        } | Select-Object -First 1
        $toolPrefixSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "url_prefix" -and $_.name -eq "https://api.example.com/"
        } | Select-Object -First 1
        $toolAllowedHeaderSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "allowed_header" -and $_.name -eq "X-Agent"
        } | Select-Object -First 1
        $toolRequiredHeaderSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "required_header" -and $_.name -eq "X-Agent"
        } | Select-Object -First 1
        $toolCommandPrefixSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "command_prefix" -and $_.name -eq "git status"
        } | Select-Object -First 1
        $toolMaxBodySearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "limit" -and $_.name -eq "max_body_bytes"
        } | Select-Object -First 1
        $toolMaxTimeoutSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "limit" -and $_.name -eq "max_timeout"
        } | Select-Object -First 1
        $toolAllowShellSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "shell" -and $_.name -eq "allow_shell"
        } | Select-Object -First 1
        $toolApprovalSearch = @($search.entries) | Where-Object {
            $_.type -eq "tool_policy" -and $_.kind -eq "approval" -and $_.name -eq "approval"
        } | Select-Object -First 1
        $pluginPolicySourceSearch = @($search.entries) | Where-Object {
            $_.type -eq "plugin_policy" -and $_.kind -eq "policy" -and $_.name -eq "sura.plugins.json"
        } | Select-Object -First 1
        $pluginManifestSearch = @($search.entries) | Where-Object {
            $_.type -eq "plugin_policy" -and $_.kind -eq "manifest" -and $_.name -eq "native/sample.sura-plugin.json"
        } | Select-Object -First 1
        $pluginExportSearch = @($search.entries) | Where-Object {
            $_.type -eq "plugin_policy" -and $_.kind -eq "allowed_export" -and $_.name -eq "native_add"
        } | Select-Object -First 1
        $pluginHostCapabilitySearch = @($search.entries) | Where-Object {
            $_.type -eq "plugin_policy" -and $_.kind -eq "host_capability" -and $_.name -eq "memory"
        } | Select-Object -First 1
        $pluginMaxMemorySearch = @($search.entries) | Where-Object {
            $_.type -eq "plugin_policy" -and $_.kind -eq "quota" -and $_.name -eq "max_memory_bytes"
        } | Select-Object -First 1
        $pluginMaxCallSearch = @($search.entries) | Where-Object {
            $_.type -eq "plugin_policy" -and $_.kind -eq "quota" -and $_.name -eq "max_call_ms"
        } | Select-Object -First 1
        $auditSummarySearch = @($search.entries) | Where-Object {
            $_.type -eq "audit" -and $_.kind -eq "security" -and $_.name -eq "audit"
        } | Select-Object -First 1
        $auditNetworkSearch = @($search.entries) | Where-Object {
            $_.type -eq "audit" -and $_.kind -eq "network access" -and $_.source -eq "src/docs_pkg.sura"
        } | Select-Object -First 1
        $auditToolPolicySearch = @($search.entries) | Where-Object {
            $_.type -eq "audit" -and $_.kind -eq "tool_policy" -and $_.source -eq "sura.tools.json"
        } | Select-Object -First 1
        $qualitySummarySearch = @($search.entries) | Where-Object {
            $_.type -eq "quality" -and $_.kind -eq "readiness" -and $_.name -eq "quality"
        } | Select-Object -First 1
        $testSummarySearch = @($search.entries) | Where-Object {
            $_.type -eq "test" -and $_.kind -eq "verification" -and $_.name -eq "tests"
        } | Select-Object -First 1
        $cliSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "cli.parse"
        } | Select-Object -First 1
        $autogradBackwardSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "autograd.backward"
        } | Select-Object -First 1
        $autogradAdamSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "autograd.adam"
        } | Select-Object -First 1
        $autogradRequiresGradSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "autograd.requires_grad"
        } | Select-Object -First 1
        $llmSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.request_json"
        } | Select-Object -First 1
        $llmSchemaSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.request_schema_json"
        } | Select-Object -First 1
        $llmToolsSchemaSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.request_tools_schema_json"
        } | Select-Object -First 1
        $llmChatRequestSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.chat_request"
        } | Select-Object -First 1
        $llmExtractJsonSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.extract_json"
        } | Select-Object -First 1
        $llmToolCallsSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.tool_calls"
        } | Select-Object -First 1
        $llmToolResultSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.tool_result"
        } | Select-Object -First 1
        $llmRunToolsSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.run_tools"
        } | Select-Object -First 1
        $llmNextMessagesSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.next_messages"
        } | Select-Object -First 1
        $llmNextRequestSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.next_request"
        } | Select-Object -First 1
        $llmNextRequestJsonSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.next_request_json"
        } | Select-Object -First 1
        $llmNextSchemaRequestSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.next_schema_request"
        } | Select-Object -First 1
        $llmNextSchemaRequestJsonSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "llm.next_schema_request_json"
        } | Select-Object -First 1
        $jsonTemplateSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "json.template_render"
        } | Select-Object -First 1
        $jsonPrettySearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "json.pretty"
        } | Select-Object -First 1
        $asyncSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "async.sleep"
        } | Select-Object -First 1
        $testSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "test.approx"
        } | Select-Object -First 1
        $httpSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "http.request_json_checked"
        } | Select-Object -First 1
        $httpRetrySearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "http.request_retry_json_checked"
        } | Select-Object -First 1
        $httpHeadersSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "http.headers_merge"
        } | Select-Object -First 1
        $httpQuerySearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "http.query_parse"
        } | Select-Object -First 1
        $randomSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "random.seed"
        } | Select-Object -First 1
        $mathSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "math.pow"
        } | Select-Object -First 1
        $stringSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "string.chunks"
        } | Select-Object -First 1
        $osSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "os.env_load"
        } | Select-Object -First 1
        $osTempDirSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "os.temp_dir"
        } | Select-Object -First 1
        $osWhichSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "os.which"
        } | Select-Object -First 1
        $osRunSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "os.run"
        } | Select-Object -First 1
        $osRunCheckedSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "os.run_checked"
        } | Select-Object -First 1
        $osCmdQuoteSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "os.cmd_quote"
        } | Select-Object -First 1
        $osCmdJoinSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "os.cmd_join"
        } | Select-Object -First 1
        $arraySearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "array.slice"
        } | Select-Object -First 1
        $pathSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "path.join"
        } | Select-Object -First 1
        $fsSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "fs.glob"
        } | Select-Object -First 1
        $fsBytesSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "fs.read_bytes"
        } | Select-Object -First 1
        $fsSha256Search = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "fs.sha256"
        } | Select-Object -First 1
        $fsRemoveTreeSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "fs.remove_tree"
        } | Select-Object -First 1
        $cryptoFileSha256Search = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "crypto.file_sha256"
        } | Select-Object -First 1
        $cryptoFileHmacSha256Search = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "crypto.file_hmac_sha256"
        } | Select-Object -First 1
        $cryptoRandomHexSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "crypto.random_hex"
        } | Select-Object -First 1
        $cryptoConstantTimeEqSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "crypto.constant_time_eq"
        } | Select-Object -First 1
        $pythonSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "python.call_json"
        } | Select-Object -First 1
        $ffiSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "ffi.call"
        } | Select-Object -First 1
        $pluginSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "plugin.call"
        } | Select-Object -First 1
        $graphics3dSearch = @($search.entries) | Where-Object {
            $_.type -eq "stdlib_symbol" -and $_.name -eq "graphics3d.project"
        } | Select-Object -First 1
        if ($search.entries.Count -lt 185 -or
            $search.entries[0].type -ne "symbol" -or
            $search.entries[0].kind -ne "constant" -or
            $search.entries[0].name -ne "PACKAGE_NAME" -or
            $search.entries[1].name -ne "PACKAGE_LIMIT" -or
            $search.entries[3].name -ne "Point" -or
            (@($search.entries) | Where-Object { $_.name -eq "local_value" }) -or
            -not $toolPolicySourceSearch -or
            -not $toolRequestSearch -or
            -not $toolGetSearch -or
            -not $toolPrefixSearch -or
            -not $toolAllowedHeaderSearch -or
            -not $toolRequiredHeaderSearch -or
            -not $toolCommandPrefixSearch -or
            -not $toolMaxBodySearch -or
            -not $toolMaxTimeoutSearch -or
            -not $toolAllowShellSearch -or
            -not $toolApprovalSearch -or
            -not $pluginPolicySourceSearch -or
            -not $pluginManifestSearch -or
            -not $pluginExportSearch -or
            -not $pluginHostCapabilitySearch -or
            -not $pluginMaxMemorySearch -or
            -not $pluginMaxCallSearch -or
            -not $auditSummarySearch -or
            -not $auditNetworkSearch -or
            -not $auditToolPolicySearch -or
            -not $testSummarySearch -or
            -not $qualitySummarySearch -or
            -not $cliSearch -or
            -not $autogradBackwardSearch -or
            -not $autogradAdamSearch -or
            -not $autogradRequiresGradSearch -or
            -not $llmSearch -or
            -not $llmSchemaSearch -or
            -not $llmToolsSchemaSearch -or
            -not $llmChatRequestSearch -or
            -not $llmExtractJsonSearch -or
            -not $llmToolCallsSearch -or
            -not $llmToolResultSearch -or
            -not $llmRunToolsSearch -or
            -not $llmNextMessagesSearch -or
            -not $llmNextRequestSearch -or
            -not $llmNextRequestJsonSearch -or
            -not $llmNextSchemaRequestSearch -or
            -not $llmNextSchemaRequestJsonSearch -or
            -not $jsonTemplateSearch -or
            -not $jsonPrettySearch -or
            -not $asyncSearch -or
            -not $testSearch -or
            -not $httpSearch -or
            -not $httpRetrySearch -or
            -not $httpHeadersSearch -or
            -not $httpQuerySearch -or
            -not $randomSearch -or
            -not $mathSearch -or
            -not $arraySearch -or
            -not $pathSearch -or
            -not $fsSearch -or
            -not $fsBytesSearch -or
            -not $fsSha256Search -or
            -not $fsRemoveTreeSearch -or
            -not $cryptoFileSha256Search -or
            -not $cryptoFileHmacSha256Search -or
            -not $cryptoRandomHexSearch -or
            -not $cryptoConstantTimeEqSearch -or
            -not $pythonSearch -or
            -not $ffiSearch -or
            -not $pluginSearch -or
            -not $graphics3dSearch -or
            -not $stringSearch -or
            -not $osSearch -or
            -not $osTempDirSearch -or
            -not $osWhichSearch -or
            -not $osCmdQuoteSearch -or
            -not $osCmdJoinSearch -or
            -not $osRunSearch -or
            -not $osRunCheckedSearch) {
            $search | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected search-index.json to include package and stdlib symbol entries"
        }

        $report = Get-Content -Raw -Encoding UTF8 -Path $reportPath | ConvertFrom-Json
        if ($report.schema -ne "sura.package.docs.v1" -or
            $report.package -ne "docs_pkg" -or
            $report.version -ne "1.2.3" -or
            $report.passed -ne $true -or
            $report.symbol_count -ne 4 -or
            $report.stdlib_module_count -lt 27 -or
            $report.stdlib_symbol_count -lt 185 -or
            $report.search_entry_count -lt 185 -or
            $report.tool_policy_present -ne $true -or
            $report.plugin_policy_present -ne $true -or
            $report.benchmark_present -ne $false -or
            $report.test_present -ne $true -or
            $report.test_ok -ne $true -or
            $report.test_total -ne 2 -or
            $report.test_passed -ne 2 -or
            $report.test_failed -ne 0 -or
            $report.audit_present -ne $true -or
            $report.audit_passed -ne $false -or
            $report.audit_finding_count -ne 2 -or
            $report.quality_present -ne $true -or
            $report.quality_passed -ne $true -or
            $report.quality_score -ne 86 -or
            $report.tool_policy_search_entry_count -lt 12 -or
            $report.plugin_policy_search_entry_count -lt 6 -or
            $report.benchmark_search_entry_count -ne 0 -or
            $report.test_search_entry_count -ne 1 -or
            $report.audit_search_entry_count -lt 3 -or
            $report.quality_search_entry_count -ne 1 -or
            -not $report.search_entry_type_counts -or
            $report.search_entry_type_counts.symbol -ne 4 -or
            $report.search_entry_type_counts.stdlib_module -lt 27 -or
            $report.search_entry_type_counts.stdlib_symbol -lt 185 -or
            $report.search_entry_type_counts.tool_policy -lt 12 -or
            $report.search_entry_type_counts.plugin_policy -lt 6 -or
            $report.search_entry_type_counts.test -ne 1 -or
            $report.search_entry_type_counts.audit -lt 3 -or
            $report.search_entry_type_counts.quality -ne 1 -or
            $report.index_bytes -le 0 -or
            $report.api_bytes -le 0 -or
            $report.search_index_bytes -le 0 -or
            $report.index -notmatch "docs_site/index.html" -or
            $report.api -notmatch "docs_site/api.json" -or
            $report.search_index -notmatch "docs_site/search-index.json") {
            $report | ConvertTo-Json -Depth 8 | Write-Output
            throw "expected docs JSON report to capture generated docs artifacts"
        }
    }
    finally {
        Pop-Location
    }

    "pkg_docs_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
