param(
    [string]$Transpiler = (Join-Path $PSScriptRoot "sura_to_js.ps1"),
    [string]$Source = (Join-Path (Split-Path -Parent $PSScriptRoot) "test_js_target.sura"),
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_js_target_" + [System.Guid]::NewGuid().ToString("N"))
$serverProcess = $null

function Get-PowerShellRunner {
    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($pwsh) { return $pwsh.Source }
    $powershell = Get-Command powershell -ErrorAction SilentlyContinue
    if ($powershell) { return $powershell.Source }
    throw "PowerShell runner not found"
}

function Resolve-SuraEngine {
    param([string]$Engine)
    if ($Engine -and (Test-Path -LiteralPath $Engine)) {
        return (Resolve-Path -LiteralPath $Engine).Path
    }
    $root = Split-Path -Parent $PSScriptRoot
    foreach ($candidate in @((Join-Path $root "SuraLanguage.exe"), (Join-Path $root "SuraLanguage"))) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Sura engine not found for AST JSON JS smoke"
}

function Invoke-NodeCapture {
    param(
        [string]$NodePath,
        [string]$ScriptPath
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & $NodePath $ScriptPath 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        return [pscustomobject]@{
            ExitCode = $code
            Output = $output
            Joined = ($output -join "`n")
        }
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return [int]$listener.LocalEndpoint.Port
    }
    finally {
        $listener.Stop()
    }
}

try {
    if (-not (Test-Path -LiteralPath $Transpiler)) {
        throw "Sura JS transpiler not found: $Transpiler"
    }
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Sura JS target source not found: $Source"
    }
    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) {
        throw "node is required for JS target smoke test"
    }

    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $out = Join-Path $temp "test_js_target.js"
    $ps = Get-PowerShellRunner
    $enginePath = Resolve-SuraEngine $Engine
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $Source -Out $out | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "sura_to_js failed with exit code $LASTEXITCODE"
    }

    $jsText = [System.IO.File]::ReadAllText($out, [System.Text.Encoding]::UTF8)
    if ($jsText -notmatch "function stream_take" -or
        $jsText -notmatch "function stream_batch" -or
        $jsText -notmatch "function stream_join" -or
        $jsText -notmatch "function stream_sum" -or
        $jsText -notmatch "function stream_avg" -or
        $jsText -notmatch "function stream_lines" -or
        $jsText -notmatch "stream.sum" -or
        $jsText -notmatch "stream.avg" -or
        $jsText -notmatch "const stream =") {
        throw "generated JS should include stream runtime helpers"
    }
    if ($jsText -notmatch 'type\("js target type command"\);' -or
        $jsText -notmatch 'clock\(\);') {
        throw "generated JS should lower command-style type and clock statements"
    }
    $cmdSource = Join-Path $temp "cmd_forms.sura"
    $cmdOut = Join-Path $temp "cmd_forms.js"
@'
type "cmd type"
clock
input "prompt"
exit 0
random 1 ~ 3 cmd_roll
'@ | Set-Content -LiteralPath $cmdSource -Encoding UTF8
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $cmdSource -Out $cmdOut | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "command-style JS forms transpile failed"
    }
    $cmdJs = [System.IO.File]::ReadAllText($cmdOut, [System.Text.Encoding]::UTF8)
    if ($cmdJs -notmatch 'type\("cmd type"\);' -or
        $cmdJs -notmatch 'clock\(\);' -or
        $cmdJs -notmatch 'input\("prompt"\);' -or
        $cmdJs -notmatch 'exit\(0\);' -or
        $cmdJs -notmatch 'var cmd_roll = random\(1, 3\);') {
        throw "generated JS should lower command-style type/clock/input/exit/random statements"
    }

    $globalSource = Join-Path $temp "global_decl.sura"
    $globalOut = Join-Path $temp "global_decl.js"
@'
score is 0
func add_score() do
  global score
  score += 1
end
add_score()
assert_eq(score, 1)
print("global-js-ok")
'@ | Set-Content -LiteralPath $globalSource -Encoding UTF8
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $globalSource -Out $globalOut | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "global declaration JS source transpile failed"
    }
    $globalJs = [System.IO.File]::ReadAllText($globalOut, [System.Text.Encoding]::UTF8)
    if ($globalJs -match '\bglobal\s+score\s*;' -or $globalJs -notmatch '// global score') {
        throw "generated JS should treat Sura global declarations as compile-time scope markers"
    }
    $globalNodeResult = Invoke-NodeCapture $node.Source $globalOut
    if ($globalNodeResult.ExitCode -ne 0 -or $globalNodeResult.Joined -notmatch "global-js-ok") {
        Write-Output $globalNodeResult.Joined
        throw "generated JS global declaration script did not pass"
    }

    $astSource = Join-Path $temp "ast_js_target.sura"
    $astImportSource = Join-Path $temp "ast_js_import.sura"
    $astJson = Join-Path $temp "ast_js_target.json"
    $astJsOut = Join-Path $temp "ast_js_target.js"
@'
func imported_js_ast(x) do
  return x * 2
end
'@ | Set-Content -LiteralPath $astImportSource -Encoding UTF8
@'
import "ast_js_import.sura"

enum JsAstMode do
  READY is 7
  DONE is 11
end

class JsAstPoint do
  func init(x, y) do
    self.x is x
    self.y is y
  end
  func sum() do
    return self.x + self.y
  end
end

func add_ast(a, b) do
  global pick
  return a + b
end

point is new JsAstPoint(2, 5)
point2 is JsAstPoint(3, 4)
assert_eq(point.sum(), 7)
assert_eq(point2.sum(), 7)
assert_eq(add_ast(3, 4), 7)
assert_eq(imported_js_ast(5), 10)
values is [1, 2, 3]
info is {name: "ast", score: 9}
who_ast is "Sura"
assert_eq(values[1], 2)
assert_eq(info["name"], "ast")
assert_eq("hello {who_ast}", "hello Sura")
assert_eq("score {info[\"score\"] + 1}", "score 10")
assert_eq("json {} done", "json {} done")
assert_eq(JsAstMode.READY, 7)
pick is 0
match values[0]
when 1 then pick is 10
when _ then pick is 20
end
assert_eq(pick, 10)
print "ast-js-ok"
'@ | Set-Content -LiteralPath $astSource -Encoding UTF8
    & $enginePath --ast-json --out $astJson $astSource | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "AST JSON export for JS target smoke failed with exit code $LASTEXITCODE"
    }
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $astJson -Out $astJsOut -AstJson -Engine $enginePath | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "AST JSON JS target transpile failed with exit code $LASTEXITCODE"
    }
    $astJs = [System.IO.File]::ReadAllText($astJsOut, [System.Text.Encoding]::UTF8)
    if ($astJs -notmatch "AST JSON input: sura\.ast\.v1" -or
        $astJs -notmatch "class __SuraAstClass_JsAstPoint" -or
        $astJs -notmatch "function JsAstPoint\(\.\.\.args\)" -or
        $astJs -notmatch "const JsAstMode" -or
        $astJs -notmatch "function imported_js_ast" -or
        $astJs -notmatch "function add_ast" -or
        $astJs -notmatch "// global pick" -or
        $astJs -notmatch "__sura_ast_match") {
        throw "generated AST JSON JS should lower imported functions, callable class factory, enum, global declarations, function, and match nodes"
    }
    $astNodeResult = Invoke-NodeCapture $node.Source $astJsOut
    $astNodeCode = $astNodeResult.ExitCode
    $astNodeJoined = $astNodeResult.Joined
    if ($astNodeCode -ne 0 -or $astNodeJoined -notmatch "ast-js-ok") {
        Write-Output $astNodeJoined
        throw "generated AST JSON JS target did not pass"
    }

    if ($jsText -notmatch "class JsPoint" -or
        $jsText -notmatch "class JsNamedPoint extends JsPoint" -or
        $jsText -notmatch "constructor\(x, y\)" -or
        $jsText -notmatch "catch \(err\)" -or
        $jsText -notmatch "finally") {
        throw "generated JS should lower Sura class/extends and try/catch/finally blocks"
    }
    if ($jsText -notmatch "class JsScoredPoint extends JsPoint" -or
        $jsText -notmatch "constructor\(x, y, score\)" -or
        $jsText -notmatch "super\(x, y\);" -or
        $jsText -match "super\.init") {
        throw "generated JS should lower Sura super.init calls to JS super constructor calls"
    }
    if ($jsText -notmatch "class __SuraStruct_JsVec2" -or
        $jsText -notmatch "function JsVec2\(\.\.\.args\)" -or
        $jsText -notmatch "return new __SuraStruct_JsVec2\(\.\.\.args\)" -or
        $jsText -notmatch "vec_sum_js = vec_js\.add\(JsVec2\(4, 5\)\)") {
        throw "generated JS should lower Sura struct declarations and factory calls"
    }
    if ($jsText -notmatch "__sura_when" -or
        $jsText -notmatch "__eq\(__sura_when" -or
        $jsText -notmatch "__sura_when_matched" -or
        $jsText -notmatch 'if \(!__sura_when_matched\d+ && \(__eq\(__sura_when' -or
        $jsText -notmatch '__sura_when_matched\d+ = true;' -or
        $jsText -notmatch "inline_when_js") {
        throw "generated JS should lower Sura when/is/in/else blocks"
    }
    if ($jsText -notmatch "ternary_js" -or
        $jsText -notmatch 'ternary_js\s*=\s*score_js == 3 \? "three" : "other";') {
        throw "generated JS should preserve ternary expressions"
    }
    if ($jsText -notmatch 'optional_user_js\?\.profile\?\.name \?\? "anon"' -or
        $jsText -notmatch 'optional_profile_js\?\.profile\?\.name \?\? "anon"' -or
        $jsText -notmatch 'optional_profile_js\?\.missing\?\.name \?\? "anon"') {
        throw "generated JS should preserve optional chaining and null coalescing expressions"
    }
    if ($jsText -notmatch "inline_elif_js" -or
        $jsText -notmatch 'else if .*inline_elif_js = "two";' -or
        $jsText -notmatch 'else if .*inline_elif_js = "three";') {
        throw "generated JS should lower inline elif branches"
    }
    if ($jsText -notmatch "inline_else_js" -or
        $jsText -notmatch 'else \{ inline_else_js = "other";') {
        throw "generated JS should lower inline else branches"
    }
    if ($jsText -notmatch "__sura_match" -or
        $jsText -notmatch "__eq\(__sura_match" -or
        $jsText -notmatch "__sura_match_matched" -or
        $jsText -notmatch "match_js" -or
        $jsText -notmatch "match_wildcard_js" -or
        $jsText -notmatch 'match_wildcard_js = "fallback";') {
        throw "generated JS should lower Sura match/when/wildcard blocks"
    }
    if ($jsText -notmatch "match_block_js" -or
        $jsText -notmatch 'if \(!__sura_match_matched\d+ && \(__eq\(__sura_match\d+, 3\)\)\)' -or
        $jsText -notmatch 'match_block_js = "three";') {
        throw "generated JS should lower block-style match arms without then"
    }
    if ($jsText -notmatch "match_first_wildcard_js" -or
        $jsText -notmatch 'if \(!__sura_match_matched\d+\) \{' -or
        $jsText -notmatch 'match_first_wildcard_js = "any";') {
        throw "generated JS should lower first-wildcard match arms"
    }
    if ($jsText -notmatch "match_middle_wildcard_js" -or
        $jsText -notmatch 'match_middle_wildcard_js = "exact";' -or
        $jsText -notmatch "match_matched_before_wildcard_js" -or
        $jsText -notmatch 'match_matched_before_wildcard_js = "late";') {
        throw "generated JS should lower nonterminal wildcard match arms with native matched-arm semantics"
    }
    if ($jsText -notmatch "const print_n" -or
        $jsText -notmatch "const print_no_nl = print_n" -or
        $jsText -notmatch 'print_n\("js target print_n"\);') {
        throw "generated JS should lower print_n space-form statements"
    }
    if ($jsText -notmatch "__sura_for_items" -or
        $jsText -notmatch "__sura_for_entries" -or
        $jsText -notmatch 'function __sura_entries' -or
        $jsText -notmatch '__sura_entries\(__sura_for_items\d+\)' -or
        $jsText -notmatch 'for \(const \[idx_js, item_js\] of __sura_for_entries') {
        throw "generated JS should lower indexed array for-loops"
    }
    if ($jsText -notmatch "dict_indexed_total_js" -or
        $jsText -notmatch 'for \(const \[key_js, value_js\] of __sura_for_entries' -or
        $jsText -notmatch '__sura_entries\(__sura_for_items\d+\)') {
        throw "generated JS should lower indexed dict for-loops"
    }
    if ($jsText -notmatch "function __sura_iter" -or
        $jsText -notmatch 'for \(const dynamic_item_js of __sura_iter\(dynamic_items_js\)\)' -or
        $jsText -notmatch 'for \(const maybe_item_js of __sura_iter\(null\)\)' -or
        $jsText -notmatch 'for \(const dict_single_key_js of __sura_iter\(\{a: 1, b: 2\}\)\)' -or
        $jsText -notmatch "nil_indexed_iter_count_js") {
        throw "generated JS should lower nil/dict single for loops without runtime iteration errors"
    }
    if ($jsText -notmatch "tilde_total_js" -or
        $jsText -notmatch 'for \(let n = 1, __sura_for_end\d+ = 3; n <= __sura_for_end\d+; n\+\+\)') {
        throw "generated JS should lower tilde range-for loops"
    }
    if ($jsText -notmatch "range_count_js" -or
        $jsText -notmatch "repeat_fixed_count_js" -or
        $jsText -notmatch 'for \(let n_limit_js = 1, __sura_for_end\d+ = range_limit_js; n_limit_js <= __sura_for_end\d+; n_limit_js\+\+\)' -or
        $jsText -notmatch 'for \(let __sura_i\d+ = 0, __sura_repeat_limit\d+ = repeat_limit_js; __sura_i\d+ < __sura_repeat_limit\d+; __sura_i\d+\+\+\)') {
        throw "generated JS should snapshot repeat and range loop limits"
    }
    if ($jsText -notmatch "tilde_when_js" -or
        $jsText -notmatch '__sura_when\d+ >= 1 && __sura_when\d+ <= 3') {
        throw "generated JS should lower tilde range when arms"
    }
    if ($jsText -notmatch "when_first_else_js" -or
        $jsText -notmatch 'if \(!__sura_when_matched\d+\) \{' -or
        $jsText -notmatch 'when_first_else_js = "fallback";') {
        throw "generated JS should lower first-else when arms"
    }
    if ($jsText -notmatch "when_middle_else_js" -or
        $jsText -notmatch 'when_middle_else_js = "exact";' -or
        $jsText -notmatch "when_matched_before_else_js" -or
        $jsText -notmatch 'when_matched_before_else_js = "late";') {
        throw "generated JS should lower nonterminal else when arms with native matched-arm semantics"
    }
    if ($jsText -notmatch "control_sum_js" -or
        $jsText -notmatch 'if \(i_js_loop == 2\) \{ continue; \}' -or
        $jsText -notmatch 'if \(i_js_loop == 5\) \{ break; \}' -or
        $jsText -notmatch 'assert_eq\(control_sum_js, 24\);') {
        throw "generated JS should preserve break/continue loop control"
    }
    if ($jsText -notmatch 'assert\(indexed_total_js == 16\);' -or
        $jsText -notmatch 'assert_eq\(indexed_total_js, 16\);') {
        throw "generated JS should lower space-form assertion statements"
    }
    if ($jsText -notmatch "bit_not_js" -or
        $jsText -notmatch 'bit_not_js\s*=\s*~7;') {
        throw "generated JS should preserve unary bitwise-not expressions"
    }
    if ($jsText -notmatch "lambda_scale_js" -or
        $jsText -notmatch '\(\(value\) => value \* lambda_factor_js\)' -or
        $jsText -notmatch 'lambda_ready_js\s*=\s*\(\(\) => "ready"\);') {
        throw "generated JS should lower Sura lambda expressions to JavaScript arrow functions"
    }
    if ($jsText -notmatch "const json =" -or
        $jsText -notmatch "const string =" -or
        $jsText -notmatch "const array =" -or
        $jsText -notmatch "function array_sum" -or
        $jsText -notmatch "function array_range" -or
        $jsText -notmatch "array.zip" -or
        $jsText -notmatch "array.flatten" -or
        $jsText -notmatch "function string_lines" -or
        $jsText -notmatch "string.pad_left" -or
        $jsText -notmatch "const set =" -or
        $jsText -notmatch "function set_union" -or
        $jsText -notmatch "set.symmetric_difference" -or
        $jsText -notmatch "const dict =" -or
        $jsText -notmatch "function dict_keys" -or
        $jsText -notmatch "dict.merge" -or
        $jsText -notmatch "const math =" -or
        $jsText -notmatch "const os =" -or
        $jsText -notmatch "const fs =" -or
        $jsText -notmatch "const path =" -or
        $jsText -notmatch "const regex =" -or
        $jsText -notmatch "const datetime =" -or
        $jsText -notmatch "const log =" -or
        $jsText -notmatch "const console =" -or
        $jsText -notmatch "const test =" -or
        $jsText -notmatch "const db =" -or
        $jsText -notmatch "const http =" -or
        $jsText -notmatch "const async =" -or
        $jsText -notmatch "const vector =" -or
        $jsText -notmatch "const graphics3d =" -or
        $jsText -notmatch "const rag =" -or
        $jsText -notmatch "const tensor =" -or
        $jsText -notmatch "const tool =" -or
        $jsText -notmatch "const llm =" -or
        $jsText -notmatch "const python =" -or
        $jsText -notmatch "const ffi =" -or
        $jsText -notmatch "const plugin =" -or
        $jsText -notmatch "const cli =") {
        throw "generated JS should include portable stdlib module objects"
    }
    if ($jsText -notmatch "function vec3" -or
        $jsText -notmatch "function vec3_cross" -or
        $jsText -notmatch "dot3: vec3_dot" -or
        $jsText -notmatch "vec3_distance" -or
        $jsText -notmatch "function mesh_cube" -or
        $jsText -notmatch "function mesh_bounds" -or
        $jsText -notmatch "function camera_project" -or
        $jsText -notmatch "graphics3d.project") {
        throw "generated JS should include 3D vector, mesh, and camera helpers"
    }
    if ($jsText -notmatch "function sleep_ms" -or
        $jsText -notmatch "const wait = sleep_ms") {
        throw "generated JS should include wait/sleep runtime helpers"
    }
    if ($jsText -notmatch "function async_sleep" -or
        $jsText -notmatch "function async_http_get" -or
        $jsText -notmatch "function async_await_timeout" -or
        $jsText -notmatch "async.all_timeout") {
        throw "generated JS should include async task runtime helpers"
    }
    if ($jsText -notmatch "__sura_define_method" -or
        $jsText -notmatch "String.prototype, 'to_num'" -or
        $jsText -notmatch "Object.prototype, 'delete'" -or
        $jsText -notmatch "Object.prototype, 'keys'") {
        throw "generated JS should include Sura-style JS method aliases"
    }
    if ($jsText -notmatch "function __sura_div" -or
        $jsText -notmatch "division by zero") {
        throw "generated JS should include Sura division-by-zero semantics"
    }
    if ($jsText -notmatch "function clamp" -or
        $jsText -notmatch "const string_len = length" -or
        $jsText -notmatch "const string_upper = upper" -or
        $jsText -notmatch "function startsWith" -or
        $jsText -notmatch "const string_contains = contains" -or
        $jsText -notmatch "string_starts_with = startsWith" -or
        $jsText -notmatch "function substring" -or
        $jsText -notmatch "const string_substring = substring" -or
        $jsText -notmatch "function concat") {
        throw "generated JS should include direct stdlib aliases used by stable tests"
    }
    if ($jsText -notmatch "function make_js_adder\(base\)" -or
        $jsText -notmatch "function add_js_value\(value\)" -or
        $jsText -notmatch "return add_js_value;" -or
        $jsText -notmatch "var add_five_js = make_js_adder\(5\);") {
        throw "generated JS should lower nested function closures"
    }
    if ($jsText -notmatch "var block_double_js = function\(value\) \{" -or
        $jsText -notmatch "return value \* 2;" -or
        $jsText -notmatch "block_double_js\(6\)") {
        throw "generated JS should lower block function expressions assigned to variables"
    }
    if ($jsText -notmatch "function random_seed" -or
        $jsText -notmatch "function random_int" -or
        $jsText -notmatch "const sign =" -or
        $jsText -notmatch "math = .*sign" -or
        $jsText -notmatch "random.shuffle") {
        throw "generated JS should include random and numeric math runtime helpers"
    }
    if ($jsText -notmatch "function file_read" -or
        $jsText -notmatch "function file_write" -or
        $jsText -notmatch "function file_read_bytes" -or
        $jsText -notmatch "function file_write_bytes" -or
        $jsText -notmatch "function file_sha256" -or
        $jsText -notmatch "const sha256_file = file_sha256" -or
        $jsText -notmatch "function file_hmac_sha256" -or
        $jsText -notmatch "const hmac_sha256_file = file_hmac_sha256" -or
        $jsText -notmatch "function file_read_json" -or
        $jsText -notmatch "fs.read_bytes" -or
        $jsText -notmatch "fs.write_bytes" -or
        $jsText -notmatch "fs.sha256" -or
        $jsText -notmatch "fs.write_json" -or
        $jsText -notmatch "function file_lines" -or
        $jsText -notmatch "const read_file = file_read" -or
        $jsText -notmatch "function file_remove_tree" -or
        $jsText -notmatch "function file_walk" -or
        $jsText -notmatch "function file_glob" -or
        $jsText -notmatch "function file_info" -or
        $jsText -notmatch "fs.lines" -or
        $jsText -notmatch "fs.remove_tree" -or
        $jsText -notmatch "fs.walk" -or
        $jsText -notmatch "fs.glob" -or
        $jsText -notmatch "fs.write") {
        throw "generated JS should include filesystem runtime helpers"
    }
    if ($jsText -notmatch "function path_join" -or
        $jsText -notmatch "function path_relative" -or
        $jsText -notmatch "path.join") {
        throw "generated JS should include path runtime helpers"
    }
    if ($jsText -notmatch "function cli_parse" -or
        $jsText -notmatch "__sura_cli_tokens" -or
        $jsText -notmatch "cli.parse") {
        throw "generated JS should include CLI parser runtime helpers"
    }
    if ($jsText -notmatch "function env_get" -or
        $jsText -notmatch "function env_load" -or
        $jsText -notmatch "function home_dir" -or
        $jsText -notmatch "function temp_dir" -or
        $jsText -notmatch "function path_separator" -or
        $jsText -notmatch "function os_name" -or
        $jsText -notmatch "function is_windows" -or
        $jsText -notmatch "function which" -or
        $jsText -notmatch "function cmd_exists" -or
        $jsText -notmatch "function cmd_run" -or
        $jsText -notmatch "function cmd_run_checked" -or
        $jsText -notmatch "function cmd_quote" -or
        $jsText -notmatch "function cmd_join" -or
        $jsText -notmatch "const command_exists = cmd_exists" -or
        $jsText -notmatch "os.home_dir" -or
        $jsText -notmatch "os.temp_dir" -or
        $jsText -notmatch "name: os_name" -or
        $jsText -notmatch "os.which" -or
        $jsText -notmatch "cmd_exists" -or
        $jsText -notmatch "cmd_quote" -or
        $jsText -notmatch "cmd_join" -or
        $jsText -notmatch "run: cmd_run" -or
        $jsText -notmatch "run_checked: cmd_run_checked" -or
        $jsText -notmatch "os.env_get") {
        throw "generated JS should include environment runtime helpers"
    }
    if ($jsText -notmatch "function sha256" -or
        $jsText -notmatch "function file_sha256" -or
        $jsText -notmatch "function hmac_sha256" -or
        $jsText -notmatch "function file_hmac_sha256" -or
        $jsText -notmatch "function crypto_random_bytes" -or
        $jsText -notmatch "function crypto_random_hex" -or
        $jsText -notmatch "function constant_time_eq" -or
        $jsText -notmatch "function base64_url_encode" -or
        $jsText -notmatch "function base64_url_decode" -or
        $jsText -notmatch "crypto.file_sha256" -or
        $jsText -notmatch "crypto.file_hmac_sha256" -or
        $jsText -notmatch "crypto_constant_time_eq" -or
        $jsText -notmatch "random_hex: crypto_random_hex" -or
        $jsText -notmatch "constant_time_eq" -or
        $jsText -notmatch "const crypto =") {
        throw "generated JS should include crypto runtime helpers"
    }
    if ($jsText -notmatch "function regex_match" -or
        $jsText -notmatch "function regex_find_all" -or
        $jsText -notmatch "function regex_escape" -or
        $jsText -notmatch "function regex_capture" -or
        $jsText -notmatch "function regex_captures" -or
        $jsText -notmatch "function regex_split" -or
        $jsText -notmatch "function datetime_parse" -or
        $jsText -notmatch "function datetime_add" -or
        $jsText -notmatch "function datetime_diff" -or
        $jsText -notmatch "datetime.utc_format") {
        throw "generated JS should include regex and datetime runtime helpers"
    }
    if ($jsText -notmatch "function log_event" -or
        $jsText -notmatch "function log_set_level" -or
        $jsText -notmatch "function log_get_level" -or
        $jsText -notmatch "level: log_level" -or
        $jsText -notmatch "function console_log" -or
        $jsText -notmatch "const console_print" -or
        $jsText -notmatch "function console_write" -or
        $jsText -notmatch "const console_println" -or
        $jsText -notmatch "function console_raw" -or
        $jsText -notmatch "function console_flush" -or
        $jsText -notmatch "function console_json" -or
        $jsText -notmatch "function console_inspect" -or
        $jsText -notmatch "function console_hrtime" -or
        $jsText -notmatch "function console_beep" -or
        $jsText -notmatch "function console_time_log" -or
        $jsText -notmatch "function console_time_end" -or
        $jsText -notmatch "function console_time_stamp" -or
        $jsText -notmatch "function console_style" -or
        $jsText -notmatch "function console_color" -or
        $jsText -notmatch "function console_strip_ansi" -or
        $jsText -notmatch "function console_set_color" -or
        $jsText -notmatch "function console_is_tty" -or
        $jsText -notmatch "function console_size" -or
        $jsText -notmatch "function console_status" -or
        $jsText -notmatch "function console_table" -or
        $jsText -notmatch "function console_dirxml" -or
        $jsText -notmatch "function console_group_end" -or
        $jsText -notmatch "function console_profile_end" -or
        $jsText -notmatch "count_reset: console_count_reset" -or
        $jsText -notmatch "read_line: console_read_line" -or
        $jsText -notmatch "const console_readLine" -or
        $jsText -notmatch "raw: console_raw" -or
        $jsText -notmatch "json: console_json" -or
        $jsText -notmatch "status: console_status" -or
        $jsText -notmatch "readLine: console_read_line" -or
        $jsText -notmatch "groupEnd: console_group_end" -or
        $jsText -notmatch "profileEnd: console_profile_end" -or
        $jsText -notmatch "stripAnsi: console_strip_ansi" -or
        $jsText -notmatch "setColor: console_set_color" -or
        $jsText -notmatch "resetColor: console_reset_color" -or
        $jsText -notmatch "isTTY: console_is_tty" -or
        $jsText -notmatch "function check_eq" -or
        $jsText -notmatch "function assert_type" -or
        $jsText -notmatch "function assert_approx" -or
        $jsText -notmatch "function test_summary" -or
        $jsText -notmatch "test.check_match" -or
        $jsText -notmatch "test.approx") {
        throw "generated JS should include logging and testing runtime helpers"
    }
    if ($jsText -notmatch "function db_insert" -or
        $jsText -notmatch "function http_get" -or
        $jsText -notmatch "__sura_child_process" -or
        $jsText -notmatch "spawnSync\('curl'" -or
        $jsText -notmatch "function http_request_json_checked" -or
        $jsText -notmatch "function http_serve_static" -or
        $jsText -notmatch "function http_serve_routes" -or
        $jsText -notmatch "function http_server_stop" -or
        $jsText -notmatch "serve_routes: http_serve_routes" -or
        $jsText -notmatch "http.request_retry_json_checked" -or
        $jsText -notmatch "function query_build" -or
        $jsText -notmatch "function url_parse" -or
        $jsText -notmatch "http.url_build" -or
        $jsText -notmatch "function http_status_ok" -or
        $jsText -notmatch "http.status_retryable" -or
        $jsText -notmatch "function http_retry_after" -or
        $jsText -notmatch "http.backoff_delays" -or
        $jsText -notmatch "function form_build" -or
        $jsText -notmatch "form_build, form_parse" -or
        $jsText -notmatch "spec.form" -or
        $jsText -notmatch "function headers_get" -or
        $jsText -notmatch "function headers_redact" -or
        $jsText -notmatch "http.headers_has" -or
        $jsText -notmatch "http.headers_redact" -or
        $jsText -notmatch "function cookie_parse" -or
        $jsText -notmatch "function cookie_get" -or
        $jsText -notmatch "function http_content_type" -or
        $jsText -notmatch "http.is_json" -or
        $jsText -notmatch "function auth_basic" -or
        $jsText -notmatch "http.headers_merge") {
        throw "generated JS should include database and HTTP helper runtime shims"
    }
    if ($jsText -notmatch "function python_available" -or
        $jsText -notmatch "function ffi_call" -or
        $jsText -notmatch "function plugin_load_manifest" -or
        $jsText -notmatch "native interop is only available in the Sura native runtime") {
        throw "generated JS should include clear native interop stubs"
    }
    if ($jsText -notmatch "function text_chunks" -or
        $jsText -notmatch "string.chunks" -or
        $jsText -notmatch "string.words" -or
        $jsText -notmatch "function vector_search" -or
        $jsText -notmatch "function rag_prepare" -or
        $jsText -notmatch "function tensor_matmul" -or
        $jsText -notmatch "function tensor_clip" -or
        $jsText -notmatch "function tensor_mean" -or
        $jsText -notmatch "function tensor_variance" -or
        $jsText -notmatch "function tensor_std" -or
        $jsText -notmatch "function tensor_max" -or
        $jsText -notmatch "function tensor_argmin" -or
        $jsText -notmatch "function tensor_argmax" -or
        $jsText -notmatch "function tensor_zscore" -or
        $jsText -notmatch "function tensor_softmax" -or
        $jsText -notmatch "rag.messages") {
        throw "generated JS should include AI vector/RAG/tensor runtime shims"
    }
    if ($jsText -notmatch "function schema_to_json_schema" -or
        $jsText -notmatch "function template_render" -or
        $jsText -notmatch "function json_try_parse" -or
        $jsText -notmatch "function json_pretty" -or
        $jsText -notmatch "function json_has_path" -or
        $jsText -notmatch "function json_merge_patch" -or
        $jsText -notmatch "function json_delete_path" -or
        $jsText -notmatch "function json_set_path" -or
        $jsText -notmatch "function jsonl_parse" -or
        $jsText -notmatch "function csv_parse" -or
        $jsText -notmatch "function ini_parse" -or
        $jsText -notmatch "json.ini_stringify" -or
        $jsText -notmatch "json.pretty" -or
        $jsText -notmatch "function pluck" -or
        $jsText -notmatch "json.count_by" -or
        $jsText -notmatch "json.template_render" -or
        $jsText -notmatch "function tool_spec" -or
        $jsText -notmatch "function llm_request_schema_json" -or
        $jsText -notmatch "function llm_tools" -or
        $jsText -notmatch "function llm_request_tools_json" -or
        $jsText -notmatch "function llm_request_tools_schema_json" -or
        $jsText -notmatch "function llm_extract_json" -or
        $jsText -notmatch "function llm_usage" -or
        $jsText -notmatch "function llm_cost" -or
        $jsText -notmatch "function llm_budget" -or
        $jsText -notmatch "function llm_chat" -or
        $jsText -notmatch "function llm_chat_request" -or
        $jsText -notmatch "function llm_tool_calls" -or
        $jsText -notmatch "function llm_tool_result" -or
        $jsText -notmatch "function llm_run_tools" -or
        $jsText -notmatch "function llm_next_messages" -or
        $jsText -notmatch "function llm_next_request" -or
        $jsText -notmatch "function llm_next_schema_request" -or
        $jsText -notmatch "function sse_parse" -or
        $jsText -notmatch "json.sse_data" -or
        $jsText -notmatch "function llm_stream_text" -or
        $jsText -notmatch "llm.chat" -or
        $jsText -notmatch "llm.usage" -or
        $jsText -notmatch "llm.cost" -or
        $jsText -notmatch "llm.budget" -or
        $jsText -notmatch "llm.chat_request" -or
        $jsText -notmatch "llm.extract_json" -or
        $jsText -notmatch "llm.tools" -or
        $jsText -notmatch "llm.request_tools_json" -or
        $jsText -notmatch "llm.request_tools_schema_json" -or
        $jsText -notmatch "llm.tool_calls" -or
        $jsText -notmatch "llm.tool_result" -or
        $jsText -notmatch "llm.run_tools" -or
        $jsText -notmatch "llm.next_messages" -or
        $jsText -notmatch "llm.next_request" -or
        $jsText -notmatch "llm.next_schema_request" -or
        $jsText -notmatch "llm.stream_text") {
        throw "generated JS should include schema/tool/LLM runtime shims"
    }
    if ($jsText -notmatch '\$\{agent\["name"\]\}' -or
        $jsText -notmatch '\$\{__sura_add\(agent\["score"\], 3\)\}' -or
        $jsText -notmatch '\$\{__sura_add\(agent\.score, 3\)\}' -or
        $jsText -notmatch 'json \{\} done') {
        throw "generated JS should lower Sura string interpolation expressions and preserve empty brace literals"
    }
    if ($jsText -notmatch 'function fib\(n\)' -or
        $jsText -notmatch 'function typed_scale\(value, factor\)' -or
        $jsText -match ': int' -or
        $jsText -match '->') {
        throw "generated JS should strip Sura type annotations"
    }
    if ($jsText -notmatch 'function imported_double\(value\)' -or
        $jsText -notmatch 'js_import_value' -or
        $jsText -notmatch 'imported_double\(js_import_value\)') {
        throw "generated JS should recursively expand Sura import files before lowering"
    }
    if ($jsText -notmatch 'const JsMode = \{' -or
        $jsText -notmatch 'READY: "READY"' -or
        $jsText -notmatch 'SCORE: 7' -or
        $jsText -notmatch 'LABEL: "mode"') {
        throw "generated JS should lower Sura enum declarations"
    }

    $serverFile = Join-Path $temp "http_server.js"
    $portFile = Join-Path $temp "http_port.txt"
    $serverOut = Join-Path $temp "http_server.out"
    $serverErr = Join-Path $temp "http_server.err"
    @'
const fs = require('fs');
const http = require('http');

const portFile = process.argv[2];
const server = http.createServer((req, res) => {
  let body = '';
  req.setEncoding('utf8');
  req.on('data', chunk => { body += chunk; });
  req.on('end', () => {
    const url = new URL(req.url, 'http://127.0.0.1');
    res.setHeader('Content-Type', 'application/json');
    if (url.pathname === '/json') {
      res.end(JSON.stringify({ok: true, method: req.method, query: url.searchParams.get('q') || '', header: req.headers['x-sura'] || ''}));
      return;
    }
    if (url.pathname === '/echo') {
      res.end(JSON.stringify({ok: true, method: req.method, body, content_type: req.headers['content-type'] || ''}));
      return;
    }
    res.statusCode = 404;
    res.end(JSON.stringify({ok: false, path: url.pathname}));
  });
});

server.listen(0, '127.0.0.1', () => {
  fs.writeFileSync(portFile, String(server.address().port), 'utf8');
});

function close() {
  server.close(() => process.exit(0));
}
process.on('SIGTERM', close);
process.on('SIGINT', close);
'@ | Set-Content -LiteralPath $serverFile -Encoding UTF8

    $serverProcess = Start-Process -FilePath $node.Source -ArgumentList @($serverFile, $portFile) -PassThru -WindowStyle Hidden -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr
    for ($i = 0; $i -lt 50 -and -not (Test-Path -LiteralPath $portFile); $i++) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $portFile)) {
        $errText = if (Test-Path -LiteralPath $serverErr) { Get-Content -LiteralPath $serverErr -Raw } else { "" }
        throw "local HTTP smoke server did not start. $errText"
    }
    $port = ([System.IO.File]::ReadAllText($portFile, [System.Text.Encoding]::UTF8)).Trim()
    if (-not $port) { throw "local HTTP smoke server did not report a port" }

    $staticPort = Get-FreeTcpPort
    $routePort = Get-FreeTcpPort
    $oldHttpUrl = [Environment]::GetEnvironmentVariable("SURA_JS_HTTP_URL", "Process")
    $oldStaticPort = [Environment]::GetEnvironmentVariable("SURA_JS_STATIC_PORT", "Process")
    $oldRoutePort = [Environment]::GetEnvironmentVariable("SURA_JS_ROUTE_PORT", "Process")
    [Environment]::SetEnvironmentVariable("SURA_JS_HTTP_URL", "http://127.0.0.1:$port", "Process")
    [Environment]::SetEnvironmentVariable("SURA_JS_STATIC_PORT", "$staticPort", "Process")
    [Environment]::SetEnvironmentVariable("SURA_JS_ROUTE_PORT", "$routePort", "Process")
    try {
        $nodeResult = Invoke-NodeCapture $node.Source $out
        $nodeCode = $nodeResult.ExitCode
        $joined = $nodeResult.Joined
        if ($nodeCode -ne 0 -or $joined -notmatch "js target: PASS") {
            Write-Output $joined
            throw "generated JS target did not pass"
        }

        $fullAstJson = Join-Path $temp "test_js_target.ast.json"
        $fullAstOut = Join-Path $temp "test_js_target.ast.js"
        & $enginePath --ast-json --out $fullAstJson $Source | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "full JS target AST JSON export failed with exit code $LASTEXITCODE"
        }
        & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $fullAstJson -Out $fullAstOut -AstJson -Engine $enginePath | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "full JS target AST JSON transpile failed with exit code $LASTEXITCODE"
        }
        $fullAstJs = [System.IO.File]::ReadAllText($fullAstOut, [System.Text.Encoding]::UTF8)
        if ($fullAstJs -notmatch "AST JSON input: sura\.ast\.v1" -or
            $fullAstJs -notmatch "class __SuraAstClass_JsVec2" -or
            $fullAstJs -notmatch "function JsVec2\(\.\.\.args\)" -or
            $fullAstJs -notmatch 'assert_eq\(`hello \$\{who\}`' -or
            $fullAstJs -notmatch '__sura_ast_match') {
            throw "full AST JSON JS should lower callable classes, interpolation, and match nodes"
        }
        $fullAstResult = Invoke-NodeCapture $node.Source $fullAstOut
        $fullAstCode = $fullAstResult.ExitCode
        $fullAstJoined = $fullAstResult.Joined
        if ($fullAstCode -ne 0 -or $fullAstJoined -notmatch "js target: PASS") {
            Write-Output $fullAstJoined
            throw "full AST JSON JS target did not pass"
        }
    }
    finally {
        [Environment]::SetEnvironmentVariable("SURA_JS_HTTP_URL", $oldHttpUrl, "Process")
        [Environment]::SetEnvironmentVariable("SURA_JS_STATIC_PORT", $oldStaticPort, "Process")
        [Environment]::SetEnvironmentVariable("SURA_JS_ROUTE_PORT", $oldRoutePort, "Process")
    }

    $operatorSource = Join-Path (Split-Path -Parent $PSScriptRoot) "tests\js_operator_parity.sura"
    $operatorAstJson = Join-Path $temp "js_operator_parity.ast.json"
    $operatorAstOut = Join-Path $temp "js_operator_parity.ast.js"
    & $enginePath --ast-json --out $operatorAstJson $operatorSource | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "JS operator parity AST JSON export failed with exit code $LASTEXITCODE"
    }
    & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $operatorAstJson -Out $operatorAstOut -AstJson -Engine $enginePath | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "JS operator parity AST transpile failed with exit code $LASTEXITCODE"
    }
    $operatorAstJs = [System.IO.File]::ReadAllText($operatorAstOut, [System.Text.Encoding]::UTF8)
    if ($operatorAstJs -notmatch '__sura_add\(' -or $operatorAstJs -notmatch '__sura_in\(') {
        throw "AST JSON JS should route Sura addition and membership through runtime helpers"
    }
    $operatorAstResult = Invoke-NodeCapture $node.Source $operatorAstOut
    if ($operatorAstResult.ExitCode -ne 0 -or $operatorAstResult.Joined -notmatch "js operator parity: PASS") {
        Write-Output $operatorAstResult.Joined
        throw "JS operator parity AST runtime failed"
    }

    $stableCases = @(
        @{ source = "tests\01_basic.sura"; expect = "01_basic: PASS" },
        @{ source = "tests\02_variables.sura"; expect = "02_variables: PASS" },
        @{ source = "tests\03_control.sura"; expect = "03_control: PASS" },
        @{ source = "tests\04_functions.sura"; expect = "04_functions: PASS" },
        @{ source = "tests\05_arrays.sura"; expect = "05_arrays: PASS" },
        @{ source = "tests\06_math.sura"; expect = "06_math: PASS" },
        @{ source = "tests\08_exceptions.sura"; expect = "08_exceptions: PASS" },
        @{ source = "tests\09_string_interpolation.sura"; expect = "09_string_interpolation: PASS" },
        @{ source = "tests\js_operator_parity.sura"; expect = "js operator parity: PASS" },
        @{ source = "test_stdlib.sura"; expect = "=== Type conversion ===" },
        @{ source = "test_null_optional.sura"; expect = "" },
        @{ source = "test_methods_chain.sura"; expect = "" },
        @{ source = "test_when_match.sura"; expect = "" },
        @{ source = "test_for_in_improved.sura"; expect = "" }
    )
    foreach ($case in $stableCases) {
        $stableSource = Join-Path (Split-Path -Parent $PSScriptRoot) $case.source
        $stableOut = Join-Path $temp ((Split-Path $case.source -Leaf) + ".js")
        & $ps -NoProfile -ExecutionPolicy Bypass -File $Transpiler -Source $stableSource -Out $stableOut | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "stable JS runtime parity transpile failed: $($case.source)"
        }
        $stableResult = Invoke-NodeCapture $node.Source $stableOut
        $stableCode = $stableResult.ExitCode
        $stableJoined = $stableResult.Joined
        if ($stableCode -ne 0 -or ($case.expect -and $stableJoined -notmatch [regex]::Escape($case.expect))) {
            Write-Output $stableJoined
            throw "stable JS runtime parity failed: $($case.source)"
        }
    }

    "js_target_smoke: PASS"
}
finally {
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
